#include "profiler.hpp"
#include "config.hpp"
#include "hooks.hpp"
#include "stereo.hpp"
#include "version_check.hpp"
#include "version.hpp"
#include "comfort.hpp"
#include "world_opts.hpp"
#include "pica_shadow.hpp"

#include <3ds.h>
#include <CTRPluginFramework.hpp>
#include <cstdio>
#include <cstring>
#include <cmath>

extern "C" void mc3d_tick_flush_b(void);
extern "C" void mc3d_tick_flush_a(void);
extern "C" void mc3d_tick_wait_b(void);
extern "C" void mc3d_tick_wait_a(void);
extern "C" void mc3d_tick_run_b(void);
extern "C" void mc3d_tick_run_a(void);
extern "C" void mc3d_tick_job_b(void);
extern "C" void mc3d_tick_job_a(void);
extern "C" void mc3d_ptick(u32 id);

using namespace CTRPluginFramework;

namespace mc3d::prof
{
    namespace
    {
        constexpr const char *kLogPath = "/luma/plugins/00040000001B8700/mc3d_log.txt";
        // CSV dumps never overwrite: mc3d_profile_001.csv, _002, ... (first free number).
        constexpr const char *kCsvPattern = "/luma/plugins/00040000001B8700/mc3d_profile_%03d.csv";
        char s_csvPath[96] = "/luma/plugins/00040000001B8700/mc3d_profile_001.csv";
        constexpr u32 kDumpSeconds = 60;

        // ---- live counters (written by game threads) ----
        struct Live
        {
            Timed world, jobs, jobRun, flush, wait, submit, gpu, osd;
            Timed syncBuild[kCores], rebuildStart[kCores], rebuildEnd[kCores];
            u32   draws, drawsIndexed;
        };
        Live s_live;

        volatile bool s_enabled = false;      // counters wanted (log or OSD on)
        volatile bool s_logging = false;      // write mc3d_log.txt once per second
        volatile bool s_osdShown = false;     // for the log context only
        volatile bool s_dumpRequested = false;
        volatile bool s_active = false;       // read by hooks
        volatile bool s_stereoMenuOn = true;

        // Game's built-in profiler sections read every second (v9.12):
        //   12   level render (wrapper 0x221BB8 around LevelRenderer::renderLevel 0x3C49D4)
        //   13   chunk scheduling + visibility culling (0x1D6AF8 -> 0x1D6CAC)
        //   0x15 / 0x16  render frame (0x2311B8)
        // Record layout from profile_record 0x1098F4: {u32 n; pad; f64 excl;
        // f64 incl; f64 maxIncl} at ctx + 32*id, seconds (0x10F4C8 time base).
        constexpr u32 kGameSecIds[kGameSecCount] = { 12, 13, 0x15, 0x16 };
        constexpr const char *kGameSecNames[kGameSecCount] = { "lvl12", "cull13", "rf15", "rf16" };
        struct GameSecPrev { u32 n; double excl, incl; };
        GameSecPrev s_gamePrev[kGameSecCount];

        inline void Add(u32 &x, u32 v)  { __atomic_fetch_add(&x, v, __ATOMIC_RELAXED); }
        inline u32  Take(u32 &x)        { return __atomic_exchange_n(&x, 0u, __ATOMIC_RELAXED); }

        inline void Record(Timed &t, u32 dt)
        {
            Add(t.n, 1);
            Add(t.ticks, dt);
            u32 cur = __atomic_load_n(&t.maxTicks, __ATOMIC_RELAXED);
            while (dt > cur && !__atomic_compare_exchange_n(&t.maxTicks, &cur, dt, true,
                                                            __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            {
            }
        }

        inline Timed TakeTimed(Timed &t)
        {
            Timed r;
            r.n = Take(t.n);
            r.ticks = Take(t.ticks);
            r.maxTicks = Take(t.maxTicks);
            return r;
        }

        inline u32 Now32() { return static_cast<u32>(svcGetSystemTick()); }

        inline u32 Core()
        {
            const s32 c = svcGetProcessorID();
            return (c >= 0 && c < static_cast<s32>(kCores)) ? static_cast<u32>(c) : kCores - 1;
        }

        Timed Sum(const Timed *t)
        {
            Timed r = { 0, 0, 0 };
            for (u32 c = 0; c < kCores; c++)
            {
                r.n += t[c].n;
                r.ticks += t[c].ticks;
                if (t[c].maxTicks > r.maxTicks)
                    r.maxTicks = t[c].maxTicks;
            }
            return r;
        }

        // Game profiler (main thread only; the sections we read are main-thread ones)
        const volatile u8 *GameSecPtr(u32 id)
        {
            const game::GameVersion *v = version::Current();
            if (!v || !v->gameProfiler)
                return nullptr;
            return reinterpret_cast<const volatile u8 *>(v->gameProfiler + 32 * id);
        }

        void ReadGameSecs(GameSec *out, bool resetOnly)
        {
            for (u32 i = 0; i < kGameSecCount; i++)
            {
                const volatile u8 *p = GameSecPtr(kGameSecIds[i]);
                if (!p)
                {
                    if (out)
                        out[i] = { 0, 0.0f, 0.0f };
                    continue;
                }
                const u32 n = *reinterpret_cast<const volatile u32 *>(p);
                const double excl = *reinterpret_cast<const volatile double *>(p + 8);
                const double incl = *reinterpret_cast<const volatile double *>(p + 16);
                GameSecPrev &pr = s_gamePrev[i];
                if (out && !resetOnly)
                {
                    const bool reset = n < pr.n;   // the game cleared its counters
                    out[i].n = reset ? n : n - pr.n;
                    out[i].exclMs = static_cast<float>((reset ? excl : excl - pr.excl) * 1000.0);
                    out[i].inclMs = static_cast<float>((reset ? incl : incl - pr.incl) * 1000.0);
                }
                pr = { n, excl, incl };
            }
        }

        // ---- hooks (keep them tiny; CallOriginal first, see hooks.hpp) ----
        hook::FunctionHook h_world, h_draw, h_drawIdx, h_sync, h_rs, h_re, h_jobs;
        hook::SubWrapHook  h_flush, h_wait, h_run, h_jobRun;

        // tick stubs (stereo_stubs.s) -> mc3d_ptick(id). Main thread only.
        u32 s_t0[4];                       // flush, wait, run, job start ticks
        volatile u32 s_submitTick[2];      // per command list, 0 = none pending

#define MC3D_TIMED_HOOK(fn, field)                                              \
        u64 fn(MC3D_HOOK_ARGS)                                                  \
        {                                                                       \
            if (!s_active)                                                      \
                return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);        \
            const u32 t0 = Now32();                                             \
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);     \
            Record(s_live.field, Now32() - t0);                                 \
            return r;                                                           \
        }

        // Same, one accumulator per core (start and end are read on the same
        // core: the game's threads are pinned, see profiler.hpp).
#define MC3D_CORE_HOOK(fn, field)                                               \
        u64 fn(MC3D_HOOK_ARGS)                                                  \
        {                                                                       \
            if (!s_active)                                                      \
                return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);        \
            const u32 t0 = Now32();                                             \
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);     \
            const u32 dt = Now32() - t0;                                        \
            Record(s_live.field[Core()], dt);                                   \
            return r;                                                           \
        }

        // World render (0x31DD8C): v0.2 also tracks the nesting for the
        // stereo projection classifier (world perspective vs hand/GUI).
        volatile s32  s_worldDepth = 0;
        volatile bool s_worldSeen = false;
        u64 Hook_World(MC3D_HOOK_ARGS)
        {
            s_worldDepth = s_worldDepth + 1;
            s_worldSeen = true;
            comfort::OnWorldEnter();        // v0.3: cursor mark (reads memory only)
            if (!s_active)
            {
                const u64 r0 = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
                comfort::OnWorldExit();
                s_worldDepth = s_worldDepth - 1;
                return r0;
            }
            const u32 t0 = Now32();
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            Record(s_live.world, Now32() - t0);
            comfort::OnWorldExit();
            s_worldDepth = s_worldDepth - 1;
            return r;
        }
        MC3D_TIMED_HOOK(Hook_Jobs, jobs)
        MC3D_CORE_HOOK(Hook_SyncBuild, syncBuild)
        MC3D_CORE_HOOK(Hook_RebuildStart, rebuildStart)
        MC3D_CORE_HOOK(Hook_RebuildEnd, rebuildEnd)
#undef MC3D_TIMED_HOOK
#undef MC3D_CORE_HOOK

        u64 Hook_Draw(MC3D_HOOK_ARGS)
        {
            if (s_active)
                Add(s_live.draws, 1);
            return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
        }

        u64 Hook_DrawIndexed(MC3D_HOOK_ARGS)
        {
            if (s_active)
                Add(s_live.drawsIndexed, 1);
            return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
        }

        // ---- per-second roll-up (main thread) ----
        u32  s_windowStart = 0;
        u32  s_lastPresent = 0;
        u32  s_frames = 0, s_frameTicks = 0, s_presentTicks = 0;
        u32  s_worstFrame = 0, s_slowFrames = 0;
        u32  s_osdFrame = 0;                // worst frame that contained an OSD re-render
        volatile bool s_osdRedraw = false;  // GSP thread sets, main thread consumes
        u32  s_reqPeak = 0, s_reqMax = 0, s_reqSkips = 0;   // main thread only
        // v0.2 M0 (main thread only)
        u32  s_cmdPeak = 0, s_cmdMax = 0;
        u32  s_proj[4] = {};
        float s_kind[4][3] = {};
        u32  s_kindN[4] = {};
        u32  s_vramHead = 0, s_vramHeadMax = 0;
        u32  s_eyes[3] = {};
        s32  s_compPx = 0;
        u32  s_second = 0;
        bool s_wasActive = false;

        // seqlock-published latest snapshot
        volatile u32 s_seq = 0;
        Snapshot     s_latest;

        // dump
        Snapshot      s_dump[kDumpSeconds];
        volatile u32  s_dumpCount = 0;
        volatile bool s_dumping = false;
        volatile bool s_dumpReady = false;
        const char   *s_dumpMsg = "idle";

        void Publish(const Snapshot &s)
        {
            __atomic_store_n(&s_seq, s_seq + 1, __ATOMIC_RELEASE);   // odd: writing
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            s_latest = s;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            __atomic_store_n(&s_seq, s_seq + 1, __ATOMIC_RELEASE);   // even: stable
        }

        void ResetM0()
        {
            s_cmdPeak = 0;
            s_proj[0] = s_proj[1] = s_proj[2] = s_proj[3] = 0;
            for (u32 i = 0; i < 4; i++)
                s_kindN[i] = 0;
            s_eyes[0] = s_eyes[1] = s_eyes[2] = 0;
        }

        void ResetLive()
        {
            TakeTimed(s_live.world); TakeTimed(s_live.jobs); TakeTimed(s_live.jobRun);
            TakeTimed(s_live.flush); TakeTimed(s_live.wait); TakeTimed(s_live.submit);
            TakeTimed(s_live.gpu); TakeTimed(s_live.osd);
            for (u32 c = 0; c < kCores; c++)
            {
                TakeTimed(s_live.syncBuild[c]); TakeTimed(s_live.rebuildStart[c]); TakeTimed(s_live.rebuildEnd[c]);
            }
            Take(s_live.draws); Take(s_live.drawsIndexed);
            ReadGameSecs(nullptr, true);
            ComfortStats c;
            comfort::TakeStats(c);
            WorldStats w;
            world::TakeStats(w);
        }

        u32 s_lastLoggedSeq = 0;
        bool s_logHeaderDone = false;

        bool AppendFile(const char *path, const std::string &text, bool truncate)
        {
            File f;
            int mode = File::WRITE | File::CREATE | (truncate ? File::TRUNCATE : File::APPEND);
            if (File::Open(f, path, mode) != File::SUCCESS)
                return false;
            const bool ok = f.Write(text.data(), text.size()) == File::SUCCESS;
            f.Flush();
            f.Close();
            return ok;
        }

        // ---- v0.3.2: the log file, with a size cap that never stops it ----
        // v0.3.1 stopped the FB lines for good once the file was over 1 MiB
        // (audit v0.3.1 N1: never reset, so every later session wrote
        // nothing). Now: past the cap the file is renamed to
        // mc3d_log_old.txt (replacing the previous old one) and a fresh one
        // is started, at boot or mid-session. Plugin thread only.
        constexpr const char *kLogOldPath = "/luma/plugins/00040000001B8700/mc3d_log_old.txt";
        constexpr u64 kLogCap = 1024 * 1024;
        bool s_logBroken = false;           // rotation failed: stop writing this session
        u32  s_logRotations = 0;

        bool RotateLog()
        {
            File::Remove(kLogOldPath);      // may not exist
            return File::Rename(kLogPath, kLogOldPath) == File::SUCCESS;
        }

        // v0.3.1: always-on fallback log. v0.3.0 wrote mc3d_log.txt only
        // with "Log stats to SD" on, so Matt's hardware session left no data.
        // Every 10 s (plugin thread, one open/append/close, never per frame)
        // one "FB" line with the Comfort 3D counts since the previous line:
        // frames tried / both eyes / previous pair held / 2D, pacer late
        // frames, governor trips, and the fallback reasons. Nothing is
        // written while Comfort is not being tried (slider 0, menus off).
        // v0.3.2: written through AppendLog (size cap = rotation).
        constexpr u64 kFbPeriod = 10ull * kTicksPerSec;
        u64  s_fbStart = 0, s_fbNext = 0;
        bool s_fbHeader = false;
        ComfortTotals s_fbLast = {};

        void FallbackLog()
        {
            const u64 now = svcGetSystemTick();
            if (!s_fbStart)
            {
                s_fbStart = now;
                s_fbNext = now + kFbPeriod;
                return;
            }
            if (now < s_fbNext)
                return;
            s_fbNext = now + kFbPeriod;
            ComfortTotals t;
            comfort::ReadTotals(t);
            const ComfortTotals l = s_fbLast;
            s_fbLast = t;
            if (t.tried == l.tried && t.govTrips == l.govTrips && t.govCoolFrames == l.govCoolFrames)
                return;
            std::string line;
            if (!s_fbHeader)
                line += Utils::Format("---- " MC3D_NAME ", game %s: FB = Comfort 3D fallback counts per 10 s "
                                      "(always written) ----\n", version::VersionName());
            line += Utils::Format("t=%lus FB %s slider %d%% | tried %lu both %lu held %lu 2D %lu | pace runs %lu "
                                  "late %lu | governor %s trips %lu cool-frames %lu",
                                  static_cast<u32>((now - s_fbStart) / kTicksPerSec),
                                  stereo::StateName(stereo::GetState()),
                                  static_cast<int>(stereo_get_slider() * 100.0f + 0.5f), t.tried - l.tried,
                                  t.replayed - l.replayed, t.held - l.held, t.mono - l.mono, t.paceRuns - l.paceRuns,
                                  t.paceLate - l.paceLate, comfort::GovernorEnabled() ? "on" : "off",
                                  t.govTrips - l.govTrips, t.govCoolFrames - l.govCoolFrames);
            bool any = false;
            for (u32 r = 1; r < kReasonCount; r++)
                if (t.reason[r] != l.reason[r])
                {
                    line += Utils::Format("%s%s=%lu", any ? " " : " | fallback ",
                                          comfort::ReasonName(static_cast<u8>(r)), t.reason[r] - l.reason[r]);
                    any = true;
                }
            line += "\n";
            if (AppendLog(line))
                s_fbHeader = true;
        }

        // ---- v0.3.2: per-switch event log ----
        // Producer: the game main thread (present hook: stereo_flat.cpp and
        // world_opts.cpp), into a lock-free single-producer ring. Consumer:
        // the plugin thread (Poll), at most once per second. No file I/O,
        // no formatting and no allocation on a game thread.
        pica::EventRing    s_evRing;
        pica::EventLimiter s_evLimit;       // main thread
        volatile u32 s_frameGap10 = 0;      // main thread: last frame interval, 0.1 ms
        float        s_frameEmaMs = 33.3f;  // main thread: smoothed interval
        u64  s_evNext = 0;
        u32  s_evBootMs = 0;
        u32  s_evDroppedSeen = 0, s_evSuppressedSeen = 0;

        u32 NowMsAbs() { return static_cast<u32>(svcGetSystemTick() / (kTicksPerSec / 1000)); }

        const char *WhyName(u8 why)
        {
            if (why < kReasonCount)
                return comfort::ReasonName(why);
            switch (why)
            {
                case kWhyNone:          return "-";
                case kWhyOff:           return "3D-off-in-menu";
                case kWhySlider0:       return "slider-0";
                case kWhyUnavailable:   return "stereo-unavailable";
                case kWhyGovernor:      return "governor(auto-flatten)";
                case kWhyNotReady:      return "comfort-not-ready";
                case kWhyModeFlat:      return "mode-flat";
                case kWhyModeAlt:       return "mode-alternate";
                case kWhyNoSlot:        return "no-free-slot";
                case kWhyNoRoom:        return "request-queue-full";
                case kWhyRd3DAuto:      return "3D-auto-lower";
                case kWhyRdRestore:     return "3D-auto-restore";
                case kWhyRdSetting:     return "plugin-setting";
                case kWhyRdGame:        return "game-setting";
                case kWhyRdLowMem:      return "low-memory";
                case kWhyRdNotAllowed:  return "not-allowed";
                case kWhyRdStepDown:    return "low-memory-step-down";
                default:                return "?";
            }
        }

        void FlushEvents()
        {
            const u64 now = svcGetSystemTick();
            if (now < s_evNext)
                return;
            s_evNext = now + kTicksPerSec;
            std::string out;
            pica::Event e;
            u32 n = 0;
            while (n < pica::kEventSlots && pica::EventPop(s_evRing, e))
            {
                n++;
                const u32 t = e.ms - s_evBootMs;
                out += Utils::Format("t=%lu.%03lus ", t / 1000, t % 1000);
                if (e.kind == static_cast<u8>(EvKind::Switch))
                    out += Utils::Format("SW %s>%s why %s", PicName(static_cast<Pic>(e.from)),
                                         PicName(static_cast<Pic>(e.to)), WhyName(e.why));
                else if (e.kind == static_cast<u8>(EvKind::Distance))
                    out += Utils::Format("RD %d>%d chunks why %s", static_cast<int>(e.from), static_cast<int>(e.to),
                                         WhyName(e.why));
                else
                    out += Utils::Format("RD %d refused why %s (stays %d)", static_cast<int>(e.from), WhyName(e.why),
                                         static_cast<int>(e.to));
                out += Utils::Format(" | frame %lu.%lums fps~%lu.%lu rd %ld\n",
                                     static_cast<u32>(e.frameMs10 / 10u), static_cast<u32>(e.frameMs10 % 10u),
                                     static_cast<u32>(e.fps10 / 10u), static_cast<u32>(e.fps10 % 10u),
                                     static_cast<long>(e.rd));
            }
            const u32 dropped = s_evRing.dropped, suppressed = s_evLimit.suppressed;
            if (dropped != s_evDroppedSeen || suppressed != s_evSuppressedSeen)
            {
                const u32 t = NowMsAbs() - s_evBootMs;
                out += Utils::Format("t=%lu.%03lus EV suppressed %lu (flapping, over 12/s) dropped %lu (queue full)\n",
                                     t / 1000, t % 1000, suppressed - s_evSuppressedSeen, dropped - s_evDroppedSeen);
                s_evDroppedSeen = dropped;
                s_evSuppressedSeen = suppressed;
            }
            if (!out.empty())
                AppendLog(out);
        }

        double Ms(u32 ticks)  { return ticks / (kTicksPerSec / 1000.0); }
        double Us(u32 ticks)  { return ticks / (kTicksPerSec / 1000000.0); }

        std::string TimedCsv(const Timed &t)
        {
            return Utils::Format("%lu,%.0f,%.0f", t.n, t.n ? Us(t.ticks) / t.n : 0.0, Us(t.maxTicks));
        }

        // "name n/s avg max [ cN n/avg ... ]" for a per-core set
        std::string CoreText(const char *name, const Timed *t, double secs)
        {
            const Timed a = Sum(t);
            std::string s = Utils::Format("%s %.0f/s avg %.0fus max %.0fus [", name, a.n / secs,
                                          a.n ? Us(a.ticks) / a.n : 0.0, Us(a.maxTicks));
            for (u32 c = 0; c < kCores; c++)
                if (t[c].n)
                    s += Utils::Format(" c%lu %lu/%.0fus", c, t[c].n, Us(t[c].ticks) / t[c].n);
            return s + " ]";
        }
    }

    // ---------------------------------------------------------------------
    void InstallHooks()
    {
        const game::GameVersion *v = version::Current();
        if (!v)
            return;
        using namespace cfg;
        if (Hook(kWorld))
            h_world.Install(v->fnWorldRender, Hook_World, hook::ExpectedFirstWord(v->fnWorldRender));
        if (Hook(kDraw))
            h_draw.Install(v->fnDraw, Hook_Draw, hook::ExpectedFirstWord(v->fnDraw));
        if (Hook(kDraw))
            h_drawIdx.Install(v->fnDrawIndexed, Hook_DrawIndexed, hook::ExpectedFirstWord(v->fnDrawIndexed));
        if (Hook(kSync))
            h_sync.Install(v->fnBuildImmediate, Hook_SyncBuild, hook::ExpectedFirstWord(v->fnBuildImmediate));
        if (Hook(kRebuild))
            h_rs.Install(v->fnStartRebuild, Hook_RebuildStart, hook::ExpectedFirstWord(v->fnStartRebuild));
        if (Hook(kRebuild))
            h_re.Install(v->fnEndRebuild, Hook_RebuildEnd, hook::ExpectedFirstWord(v->fnEndRebuild));
        if (Hook(kJobs))
            h_jobs.Install(v->fnJobBudget, Hook_Jobs, hook::ExpectedFirstWord(v->fnJobBudget));
        // v0.1.1 timing sites. SubWrapHook refuses a site whose word differs.
        if (Hook(kSplit))
        {
            if (v->sitePresentFlush)
                h_flush.Install(v->sitePresentFlush, v->insnPresentFlush, mc3d_tick_flush_b, mc3d_tick_flush_a);
            if (v->sitePresentWait)
                h_wait.Install(v->sitePresentWait, v->insnPresentWait, mc3d_tick_wait_b, mc3d_tick_wait_a);
            if (v->siteJobRun)
                h_jobRun.Install(v->siteJobRun, v->insnJobRun, mc3d_tick_job_b, mc3d_tick_job_a);
        }
        // RunCmdlist site: profiler timing AND (v0.3) the Comfort 3D pacer and
        // the end-of-list shadow parse, so it is also installed for "comfort".
        if ((Hook(kSplit) || Hook(kComfort)) && v->sitePresentRun)
            h_run.Install(v->sitePresentRun, v->insnPresentRun, mc3d_tick_run_b, mc3d_tick_run_a);
    }

    std::string HookStatus()
    {
        auto one = [](const char *name, const hook::FunctionHook &h) {
            return Utils::Format("%s %08lX %s\n", name, h.Target(), hook::ResultString(h.LastResult()));
        };
        return one("world  ", h_world) + one("draw   ", h_draw) + one("drawIdx", h_drawIdx) +
               one("sync   ", h_sync) + one("rbStart", h_rs) + one("rbEnd  ", h_re) + one("jobs   ", h_jobs) +
               Utils::Format("split flush %s wait %s run %s job %s\n",
                             hook::ResultString(h_flush.LastResult()), hook::ResultString(h_wait.LastResult()),
                             hook::ResultString(h_run.LastResult()), hook::ResultString(h_jobRun.LastResult()));
    }

    void SetEnabled(bool on) { s_enabled = on; s_active = s_enabled || s_dumping || s_dumpRequested; }
    void SetLogging(bool on) { s_logging = on; world::SetHeapStatsWanted(on); }
    bool RunHookInstalled() { return h_run.Installed(); }

    void RecordOsdDraw(u32 ticks)
    {
        if (s_active)
            Record(s_live.osd, ticks);
    }

    // GSP thread (cmdlist-done callback, before the top swap)
    void OnCmdlistDone(u32 list)
    {
        const u32 t0 = __atomic_exchange_n(&s_submitTick[list & 1], 0u, __ATOMIC_ACQ_REL);
        if (!t0)
            return;
        const u32 dt = Now32() - t0;
        if (s_active)
            Record(s_live.gpu, dt);
        comfort::OnListGpu(list, dt);   // v0.3: split by list kind (both eyes / one)
    }
    void MarkOsdRedraw() { __atomic_store_n(&s_osdRedraw, true, __ATOMIC_RELEASE); }

    // Main thread (present hook / RunCmdlist tick): keep the per-second peak.
    void NoteCmdlistReqs(u32 used, u32 max)
    {
        if (used > s_reqPeak)
            s_reqPeak = used;
        s_reqMax = max;
    }
    void NoteReqSkip() { s_reqSkips++; }

    bool WorldHookInstalled() { return h_world.Installed(); }
    bool InWorld()            { return s_worldDepth > 0; }
    bool TakeWorldSeen()
    {
        const bool r = s_worldSeen;
        s_worldSeen = false;
        return r;
    }
    bool WorldSeenThisFrame() { return s_worldSeen; }

    // ---- v0.3.2: log file (plugin thread) ----
    bool AppendLog(const std::string &text)
    {
        if (s_logBroken || text.empty())
            return false;
        File f;
        if (File::Open(f, kLogPath, File::WRITE | File::CREATE | File::APPEND) != File::SUCCESS)
            return false;
        std::string out;
        if (static_cast<u64>(f.GetSize()) > kLogCap)
        {
            f.Close();
            if (!RotateLog() ||
                File::Open(f, kLogPath, File::WRITE | File::CREATE | File::APPEND) != File::SUCCESS)
            {
                s_logBroken = true;     // keep the card tidy: never grow past the cap
                return false;
            }
            s_logRotations++;
            out = "---- log over 1 MiB: older lines moved to mc3d_log_old.txt ----\n";
        }
        out += text;
        const bool ok = f.Write(out.data(), out.size()) == File::SUCCESS;
        f.Flush();
        f.Close();
        return ok;
    }

    void BootLog(const std::string &cfgText)
    {
        s_evBootMs = NowMsAbs();
        // Utils::Format has a 256-byte buffer: the settings text is appended as is.
        AppendLog(Utils::Format("==== " MC3D_NAME " boot, game %s | ", version::VersionName()) + cfgText +
                  " ====\n");
    }

    // ---- v0.3.2: per-switch event log (game main thread) ----
    void NoteFrameInterval(u32 gap10)
    {
        s_frameGap10 = gap10;
        if (gap10 > 0 && gap10 < 10000)     // a HOME/sleep/load gap is not a frame rate
            s_frameEmaMs += (static_cast<float>(gap10) * 0.1f - s_frameEmaMs) * 0.1f;
    }

    void PushEvent(EvKind k, u8 from, u8 to, u8 why)
    {
        const u32 now = NowMsAbs();
        if (!pica::LimiterAllow(s_evLimit, now))
            return;
        pica::Event e = {};
        e.ms = now;
        e.kind = static_cast<u8>(k);
        e.from = from;
        e.to = to;
        e.why = why;
        const u32 gap = s_frameGap10;
        e.frameMs10 = static_cast<u16>(gap > 65535u ? 65535u : gap);
        const float fps = s_frameEmaMs > 1.0f ? 1000.0f / s_frameEmaMs : 0.0f;
        e.fps10 = static_cast<u16>(fps * 10.0f + 0.5f);
        e.rd = world::DrawnChunks();
        pica::EventPush(s_evRing, e);
    }

    const char *PicName(Pic p)
    {
        switch (p)
        {
            case Pic::Off:       return "2D-off";
            case Pic::Full3D:    return "3D";
            case Pic::Held:      return "held";
            case Pic::Mono2D:    return "2D";
            case Pic::FlatPlane: return "flat-plane";
            case Pic::Alternate: return "alternate";
            default:             return "?";
        }
    }
    void NoteCmdlistBytes(u32 used, u32 size)
    {
        if (used > s_cmdPeak)
            s_cmdPeak = used;
        s_cmdMax = size;
    }
    void NoteProj(int cls)
    {
        if (cls >= 0 && cls < 4)
            s_proj[cls]++;
    }
    void NoteWorldKind(float m0, float m10, float m14)
    {
        for (u32 i = 0; i < 4; i++)
        {
            if (s_kindN[i] && s_kind[i][0] == m0 && s_kind[i][1] == m10 && s_kind[i][2] == m14)
            {
                s_kindN[i]++;
                return;
            }
            if (!s_kindN[i])
            {
                s_kind[i][0] = m0; s_kind[i][1] = m10; s_kind[i][2] = m14;
                s_kindN[i] = 1;
                return;
            }
        }
    }
    void NoteVramBHead(u32 head)
    {
        s_vramHead = head;
        if (head > s_vramHeadMax)
            s_vramHeadMax = head;
    }
    void NoteEye(int eye, int compPx)
    {
        s_eyes[eye < 0 ? 0 : eye == 0 ? 1 : 2]++;
        s_compPx = compPx;
    }

    void SetOsdShown(bool on) { s_osdShown = on; }
    void SetStereoMenuState(bool on) { s_stereoMenuOn = on; }
    bool Enabled()           { return s_enabled; }
    bool Active()            { return s_active; }
    const char *LogPath()    { return kLogPath; }
    const char *CsvPath()    { return s_csvPath; }

    void StartDump()
    {
        if (s_dumping || s_dumpRequested)
            return;
        if (s_dumpReady)
            return;     // the last recording is not written yet (Poll does it next frame)
        // Plugin thread (menu): pick the first unused file name, never overwrite.
        int freeNo = 0;
        for (int i = 1; i <= 999 && !freeNo; i++)
        {
            std::snprintf(s_csvPath, sizeof(s_csvPath), kCsvPattern, i);
            if (File::Exists(s_csvPath) != 1)
                freeNo = i;
        }
        if (!freeNo)
        {
            // 001..999 all exist: stop instead of overwriting _999.
            s_dumpMsg = "no free CSV number (001-999 used)";
            OSD::Notify("CSV not started: 999 files, delete some");
            return;
        }
        s_dumpMsg = "starting";
        s_dumpRequested = true;
        s_active = true;
    }

    int DumpProgress()
    {
        if (s_dumping || s_dumpRequested)
            return static_cast<int>(s_dumpCount);
        return -1;
    }

    const char *DumpMessage() { return s_dumpMsg; }

    bool Latest(Snapshot &out)
    {
        for (int tries = 0; tries < 8; tries++)
        {
            const u32 a = __atomic_load_n(&s_seq, __ATOMIC_ACQUIRE);
            if (a == 0)
                return false;
            if (a & 1)
                continue;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            out = s_latest;
            __atomic_thread_fence(__ATOMIC_SEQ_CST);
            if (__atomic_load_n(&s_seq, __ATOMIC_ACQUIRE) == a)
                return true;
        }
        return false;
    }

    // Main thread, after the original present() returned.
    void OnPresent(bool active, u64 startTick, u64 endTick)
    {
        if (!active)
        {
            s_wasActive = false;   // next activation starts a clean window
            return;
        }
        const u32 start = static_cast<u32>(startTick);
        const u32 now = static_cast<u32>(endTick);

        if (!s_wasActive)
        {
            // (re)start a clean window; drop whatever the hooks counted while
            // the flag was flipping.
            ResetLive();
            s_windowStart = s_lastPresent = now;
            s_frames = s_frameTicks = s_presentTicks = 0;
            s_worstFrame = s_slowFrames = s_osdFrame = 0;
            s_reqPeak = s_reqSkips = 0;
            ResetM0();
            __atomic_store_n(&s_osdRedraw, false, __ATOMIC_RELAXED);
            s_wasActive = true;
            if (s_dumpRequested)
            {
                s_dumpCount = 0;
                s_dumping = true;
                s_dumpRequested = false;
                s_dumpMsg = "recording";
            }
            return;
        }

        s_frames++;
        const u32 ft = now - s_lastPresent;
        s_frameTicks += ft;
        // The once-per-second OSD re-render (CTRPF font + uncached reads on
        // the GSP thread, audit N3) is kept out of "worst frame" and logged
        // on its own as "osd frame".
        if (__atomic_exchange_n(&s_osdRedraw, false, __ATOMIC_ACQ_REL))
        {
            if (ft > s_osdFrame)
                s_osdFrame = ft;
        }
        else if (ft > s_worstFrame)
            s_worstFrame = ft;
        if (ft > kTicksPerSec / 30)
            s_slowFrames++;
        s_presentTicks += now - start;
        s_lastPresent = now;

        if (s_dumpRequested && !s_dumping)
        {
            s_dumpCount = 0;
            s_dumping = true;
            s_dumpRequested = false;
            s_dumpMsg = "recording";
        }

        const u32 window = now - s_windowStart;
        if (window < kTicksPerSec)
            return;

        Snapshot s;
        s.second = ++s_second;
        s.windowTicks = window;
        s.frames = s_frames;
        s.frameTicks = s_frameTicks;
        s.presentTicks = s_presentTicks;
        s.world = TakeTimed(s_live.world);
        s.draws = Take(s_live.draws);
        s.drawsIndexed = Take(s_live.drawsIndexed);
        for (u32 c = 0; c < kCores; c++)
        {
            s.syncBuild[c] = TakeTimed(s_live.syncBuild[c]);
            s.rebuildStart[c] = TakeTimed(s_live.rebuildStart[c]);
            s.rebuildEnd[c] = TakeTimed(s_live.rebuildEnd[c]);
        }
        s.jobs = TakeTimed(s_live.jobs);
        s.jobRun = TakeTimed(s_live.jobRun);
        s.flush = TakeTimed(s_live.flush);
        s.wait = TakeTimed(s_live.wait);
        s.submit = TakeTimed(s_live.submit);
        s.gpu = TakeTimed(s_live.gpu);
        s.osd = TakeTimed(s_live.osd);
        s.worstFrameTicks = s_worstFrame;
        s.osdFrameTicks = s_osdFrame;
        s.slowFrames = s_slowFrames;
        s.reqPeak = static_cast<u16>(s_reqPeak);
        s.reqMax = static_cast<u16>(s_reqMax);
        s.reqSkips = s_reqSkips;
        s.cmdBytesPeak = s_cmdPeak;
        s.cmdBytesMax = s_cmdMax;
        s.projWorld = s_proj[0]; s.projOther = s_proj[1]; s.projOrtho = s_proj[2]; s.projBottom = s_proj[3];
        for (u32 i = 0; i < 4; i++)
        {
            s.kindN[i] = s_kindN[i];
            for (u32 j = 0; j < 3; j++)
                s.kind[i][j] = s_kind[i][j];
        }
        s.vramBHead = s_vramHead;
        s.vramBHeadMax = s_vramHeadMax;
        s.eyeL = s_eyes[1]; s.eyeR = s_eyes[2]; s.eyeMono = s_eyes[0];
        s.compPx = static_cast<s16>(s_compPx);
        s.stereoMode = static_cast<u8>(stereo::GetMode());
        s.pad2 = 0;
        ReadGameSecs(s.game, false);
        s.sliderPct = static_cast<u8>(stereo_get_slider() * 100.0f + 0.5f);
        s.stereoState = static_cast<u8>(stereo::GetState());
        s.disparity = static_cast<s8>(stereo::CurrentDisparity());
        s.presetPx = static_cast<s8>(stereo::IsFlatParallax() ? stereo::CurrentPresetPx()
                                                              : stereo::FallbackShift());
        s.mode = static_cast<u16>(stereo::CurrentMode());
        s.stereoOn = s_stereoMenuOn ? 1 : 0;
        s.pad = 0;
        comfort::TakeStats(s.c3);
        s.c3.mode = static_cast<u8>(stereo::GetMode());
        s.c3.active = stereo::ThreeDActive() ? 1 : 0;
        world::TakeStats(s.w);
        Publish(s);

        if (s_dumping)
        {
            if (s_dumpCount < kDumpSeconds)
                s_dump[s_dumpCount] = s;
            s_dumpCount = s_dumpCount + 1;
            if (s_dumpCount >= kDumpSeconds)
            {
                s_dumping = false;
                s_dumpReady = true;
                s_dumpMsg = "writing";
            }
        }

        s_windowStart = now;
        s_frames = s_frameTicks = s_presentTicks = 0;
        s_worstFrame = s_slowFrames = s_osdFrame = 0;
        s_reqPeak = s_reqSkips = 0;
        ResetM0();
        s_active = s_enabled || s_dumping || s_dumpRequested;
        if (!s_active)
            s_wasActive = false;
    }

    // v0.3: one " | " separated chunk of Comfort 3D / world fields.
    // Vblank period of the 3DS LCD: 4,481,136 cycles of 268,111,856 Hz.
    std::string C3Text(const Snapshot &s)
    {
        constexpr double kVblMs = 16.7139;
        const ComfortStats &c = s.c3;
        const WorldStats &w = s.w;
        std::string out = Utils::Format("C3 %s %s tried %lu both %lu held %lu mono %lu",
                                        stereo::ModeName(static_cast<stereo::Mode>(c.mode)),
                                        c.active ? "3D" : "2D", c.tried, c.replayed, c.held, c.monoFallback);
        bool any = false;
        for (u32 r = 0; r < kReasonCount; r++)
            if (c.reason[r])
            {
                out += Utils::Format("%s%s=%u", any ? " " : " | fallback ", comfort::ReasonName(static_cast<u8>(r)),
                                     c.reason[r]);
                any = true;
            }
        const double rep = c.replayed ? c.replayed : 1;
        if (c.reqFrames)
        {
            out += Utils::Format(" | in-range requests in %lu frames: dma %lu render %lu fill %lu transfer %lu texcopy %lu"
                                 " | segmented %lu",
                                 c.reqFrames, c.reqTypes[0], c.reqTypes[1], c.reqTypes[2], c.reqTypes[3], c.reqTypes[4],
                                 c.segmented);
            for (u32 i = 0; i < c.reqDumpN && i < 4; i++)
                out += Utils::Format(" | req%lu %08lX %08lX %08lX %08lX %08lX %08lX %08lX", i, c.reqDump[i][0],
                                     c.reqDump[i][1], c.reqDump[i][2], c.reqDump[i][3], c.reqDump[i][4],
                                     c.reqDump[i][5], c.reqDump[i][6]);
            static const char *const cn[3] = { "gd-split", "ClearTargets", "FbClear" };
            for (int k = 0; k < 3; k++)
                if (c.callerN[k])
                    out += Utils::Format(" | %s x%lu from %08lX..%08lX", cn[k], c.callerN[k], c.caller[k][0],
                                         c.caller[k][1]);
        }
        if (c.reqRefused || c.govTrips || c.govCoolFrames)
            out += Utils::Format(" | refused-requests %lu | governor trips %lu cool-frames %lu", c.reqRefused,
                                 c.govTrips, c.govCoolFrames);
        out += Utils::Format(" | parse %.2f/%.2fms emit %.2fms build %.2fms copy %.0fKB pro %.0f epi %.0f words",
                             c.tried ? Ms(c.parseTicks) / c.tried : 0.0, Ms(c.parseMaxTicks),
                             c.tried ? Ms(c.emitTicks) / c.tried : 0.0, Ms(c.buildTicks) / rep,
                             c.copyBytes / 1024.0 / rep, c.proWords / rep, c.epiWords / rep);
        out += Utils::Format(" | sites world %lu hand %lu hud %lu menu %lu proj8 %lu/%lu menuframes %lu",
                             c.sitesWorld, c.sitesHand, c.sitesHud, c.sitesMenu, c.proj8Match,
                             c.proj8Match + c.proj8Miss, c.menuFrames);
        out += Utils::Format(" | pace %s runs %lu wait %.2fms/f max %.1fms late %lu timeout %lu",
                             c.paceOn ? "30" : "off", c.paceRuns, c.paceRuns ? Ms(c.paceWaitTicks) / c.paceRuns : 0.0,
                             Ms(c.paceMaxWaitTicks), c.paceLate, c.paceTimeout);
        for (int e = 0; e < 2; e++)
        {
            const u32 n = c.hist[e][0] + c.hist[e][1] + c.hist[e][2] + c.hist[e][3];
            const double mean = n ? static_cast<double>(c.ivSum[e]) / n : 0.0;
            const double var = n ? static_cast<double>(c.ivSq[e]) / n - mean * mean : 0.0;
            out += Utils::Format(" | %s %lu imgs vbl 1:%lu 2:%lu 3:%lu 4+:%lu mean %.1fms jitter %.1fms lock30 %.0f%%",
                                 e ? "R" : "L", n, c.hist[e][0], c.hist[e][1], c.hist[e][2], c.hist[e][3],
                                 mean * kVblMs, var > 0.0 ? std::sqrt(var) * kVblMs : 0.0,
                                 n ? 100.0 * c.hist[e][1] / n : 0.0);
        }
        const double gC = c.gpuCN ? Ms(c.gpuCTicks) / c.gpuCN : 0.0, gM = c.gpuMN ? Ms(c.gpuMTicks) / c.gpuMN : 0.0;
        out += Utils::Format(" | eyes-split %lu vblanks %lu | gpu both-eyes %.2fms/list (%lu) one-eye %.2fms (%lu)",
                             c.eyeSplit, c.vblanks, gC, c.gpuCN, gM, c.gpuMN);
        if (c.gpuCN && c.gpuMN)
            out += Utils::Format(" right-eye ~%.2fms", gC - gM);
        out += Utils::Format(" | rd opt %ld ctx %ld lr %ld clamp %ld auto %s far %.1f override %.1f",
                             w.optRd, w.ctxBlocks, w.lrBlocks, w.clampBlocks, w.rdAuto ? "on" : "off", w.lrFar,
                             w.farOverride);
        out += Utils::Format(" | leaves %d (%s) rebuilds %u rd-changes %u guard %s refused %u stepdown %u",
                             w.leaves, w.leavesMode == 0 ? "auto" : w.leavesMode == 1 ? "fast" : "fancy", w.rebuilds,
                             w.rdChanges, w.guardOk ? "ok" : "no", w.guardRefusals, w.stepDowns);
        out += Utils::Format(" | heaps%s", w.heapWalkOk ? "" : " (walk incomplete)");
        for (u32 i = 0; i < w.heapCount && i < kHeapSlots; i++)
            out += Utils::Format(" s%u:%lu/%lu/%luK%s%s%s", w.heap[i].slot, w.heap[i].capKB, w.heap[i].freeKB,
                                 w.heap[i].largestKB, w.heap[i].ok ? "" : "?", w.heap[i].parent ? "(parent)" : "",
                                 w.heap[i].guarded ? "*" : "");
        out += Utils::Format(" | options.txt substitutions %u", static_cast<unsigned>(w.saveSubst));
        return out;
    }

    std::string Summary(const Snapshot &s, bool ml)
    {
        const double f = s.frames ? s.frames : 1;
        const double fps = s.windowTicks ? s.frames * (double)kTicksPerSec / s.windowTicks : 0.0;
        const double secs = s.windowTicks ? (double)s.windowTicks / kTicksPerSec : 1.0;
        const char *sep = ml ? "\n" : " | ";
        std::string out = Utils::Format(
            "fps %.1f frame %.2fms present %.2fms world %.2fms%s"
            "draws/f %.0f (+idx %.0f) jobs %.2fms/f%s",
            fps, Ms(s.frameTicks) / f, Ms(s.presentTicks) / f, Ms(s.world.ticks) / f, sep,
            s.draws / f, s.drawsIndexed / f, Ms(s.jobs.ticks) / f, sep);
        out += Utils::Format("present split flush %.2f wait %.2f submit %.2fms | gpu %.2fms/list (%lu) | "
                             "worst frame %.1fms (osd frame %.1fms) slow(>33ms) %lu | job max %.2fms (%lu runs) | "
                             "osd %.3fms/f%s",
                             Ms(s.flush.ticks) / f, Ms(s.wait.ticks) / f, Ms(s.submit.ticks) / f,
                             s.gpu.n ? Ms(s.gpu.ticks) / s.gpu.n : 0.0, s.gpu.n, Ms(s.worstFrameTicks),
                             Ms(s.osdFrameTicks), s.slowFrames, Ms(s.jobRun.maxTicks), s.jobRun.n,
                             Ms(s.osd.ticks) / f, sep);
        out += Utils::Format("cmdlist requests peak %u/%u, 3D skipped %lu frames%s", s.reqPeak, s.reqMax,
                             s.reqSkips, sep);
        out += Utils::Format("vramB head %08lX max %08lX | world persp kinds", s.vramBHead, s.vramBHeadMax);
        for (u32 i = 0; i < 4; i++)
            if (s.kindN[i])
                out += Utils::Format(" [%.3f %.4f %.4f]x%lu", s.kind[i][0], s.kind[i][1], s.kind[i][2], s.kindN[i]);
        out += Utils::Format(" | proj/f bottom %.1f%s", s.projBottom / f, sep);
        out += Utils::Format("cmdlist bytes peak %lu/%lu | proj/f world %.1f other %.1f ortho %.1f | "
                             "eyes L %lu R %lu mono %lu comp %dpx%s",
                             s.cmdBytesPeak, s.cmdBytesMax, s.projWorld / f, s.projOther / f, s.projOrtho / f,
                             s.eyeL, s.eyeR, s.eyeMono, s.compPx, sep);
        out += CoreText("sync", s.syncBuild, secs) + sep;
        out += CoreText("rbStart", s.rebuildStart, secs) + sep;
        out += CoreText("rbEnd", s.rebuildEnd, secs) + sep;
        bool anyGame = false;
        for (u32 i = 0; i < kGameSecCount; i++)
            if (s.game[i].n)
            {
                out += Utils::Format("%s%s %.2f/%.2fms", anyGame ? " " : "game(excl/incl per f) ", kGameSecNames[i],
                                     s.game[i].exclMs / f, s.game[i].inclMs / f);
                anyGame = true;
            }
        if (anyGame)
            out += sep;
        out += Utils::Format("3D %s (%s) menu %s slider %u%% preset %dpx disp %d mode %03X",
                             stereo::StateName(static_cast<stereo::State>(s.stereoState)),
                             stereo::ModeName(static_cast<stereo::Mode>(s.stereoMode)),
                             s.stereoOn ? "on" : "off", s.sliderPct, s.presetPx, s.disparity, s.mode);
        out += sep + C3Text(s);
        return out;
    }

    // Compact bottom-screen text: 5 lines of <= 52 chars (the bottom screen
    // fits 53 characters of CTRPF's 6 px font). Plugin thread only.
    std::string OsdText(const Snapshot &s)
    {
        const double f = s.frames ? s.frames : 1;
        const double fps = s.windowTicks ? s.frames * (double)kTicksPerSec / s.windowTicks : 0.0;
        const double secs = s.windowTicks ? (double)s.windowTicks / kTicksPerSec : 1.0;
        const Timed rb = Sum(s.rebuildEnd), sy = Sum(s.syncBuild);
        std::string out = Utils::Format("fps %.1f  frame %.1fms  world %.1fms\n",
                                        fps, Ms(s.frameTicks) / f, Ms(s.world.ticks) / f);
        out += Utils::Format("present %.1fms  jobs %.2fms  draws %.0f\n",
                             Ms(s.presentTicks) / f, Ms(s.jobs.ticks) / f, (s.draws + s.drawsIndexed) / f);
        out += Utils::Format("chunks %.0f/s avg %.0fus max %.0fus sync %.0f\n", rb.n / secs,
                             rb.n ? Us(rb.ticks) / rb.n : 0.0, Us(rb.maxTicks), sy.n / secs);
        out += Utils::Format("game: level %.1fms  cull %.1fms  osd %.2fms\n", s.game[0].exclMs / f,
                             s.game[1].exclMs / f, Ms(s.osd.ticks) / f);
        out += Utils::Format("wait %.1f gpu %.1fms  worst %.0fms  job max %.1f\n", Ms(s.wait.ticks) / f,
                             s.gpu.n ? Ms(s.gpu.ticks) / s.gpu.n : 0.0, Ms(s.worstFrameTicks), Ms(s.jobRun.maxTicks));
        out += Utils::Format("3D %s %u%% %dpx req %u/%u skip %lu%s\n",
                             stereo::StateName(static_cast<stereo::State>(s.stereoState)), s.sliderPct, s.presetPx,
                             s.reqPeak, s.reqMax, s.reqSkips, s_logging ? " log" : "");
        out += Utils::Format("list %luK  eyes L%lu R%lu  comp %dpx  proj %.0f/f\n",
                             s.cmdBytesPeak >> 10, s.eyeL, s.eyeR, s.compPx,
                             (s.projWorld + s.projOther + s.projOrtho) / f);
        const u32 nL = s.c3.hist[0][0] + s.c3.hist[0][1] + s.c3.hist[0][2] + s.c3.hist[0][3];
        const double rp = s.c3.tried ? s.c3.tried : 1;
        out += Utils::Format("C3 %lu/%lu h%lu p%.1f b%.1fms late%lu gov%lu\n", s.c3.replayed, s.c3.tried,
                             s.c3.held, Ms(s.c3.parseTicks) / rp,
                             s.c3.replayed ? Ms(s.c3.buildTicks) / s.c3.replayed : 0.0, s.c3.paceLate, s.c3.govTrips);
        out += Utils::Format("lock30 %.0f%% rd %ld>%ld lv %d",
                             nL ? 100.0 * s.c3.hist[0][1] / nL : 0.0,
                             static_cast<long>(s.w.ctxBlocks / 16), static_cast<long>(s.w.lrBlocks > 0 ? s.w.lrBlocks / 16 : -1),
                             static_cast<int>(s.w.leaves));
        return out;
    }

    // Plugin thread, every frame (menu callback).
    void Poll()
    {
        s_active = s_enabled || s_dumping || s_dumpRequested;
        FallbackLog();                      // v0.3.1: always on, every 10 s
        FlushEvents();                      // v0.3.2: switch / distance events, at most once per second

        Snapshot s;
        if (s_logging && Latest(s) && s_seq != s_lastLoggedSeq)
        {
            s_lastLoggedSeq = s_seq;
            std::string line;
            if (!s_logHeaderDone)
            {
                line += Utils::Format("---- " MC3D_NAME ", game %s (new session) ----\n", version::VersionName());
                s_logHeaderDone = true;
            }
            line += Utils::Format("t=%lus ", s.second) + Summary(s, false) +
                    (s_osdShown ? " | osd on\n" : " | osd off\n");
            AppendLog(line);
        }

        if (s_dumpReady)
        {
            s_dumpReady = false;
            std::string csv = Utils::Format("# " MC3D_NAME " profile, game %s, ticks/s %lu, osd %s\n",
                                            version::VersionName(), kTicksPerSec, s_osdShown ? "on" : "off");
            csv += "second,fps,frame_ms,present_ms,world_ms,draws_per_frame,draws_indexed_per_frame,"
                   "sync_builds,sync_avg_us,sync_max_us,rebuild_start_n,rebuild_start_avg_us,rebuild_start_max_us,"
                   "rebuild_end_n,rebuild_end_avg_us,rebuild_end_max_us,rebuild_end_core2_n,rebuild_end_core2_avg_us,"
                   "jobs_ms_per_frame,game_lvl12_excl_ms,game_lvl12_incl_ms,game_cull13_excl_ms,game_cull13_incl_ms,"
                   "game_rf15_incl_ms,game_rf16_incl_ms,slider_pct,stereo,stereo_menu,preset_px,disparity,mode,"
                   "flush_ms,gpu_wait_ms,submit_ms,gpu_ms_per_list,gpu_lists,worst_frame_ms,slow_frames,"
                   "job_max_ms,job_runs,osd_ms_per_frame,osd_frame_ms,cmdlist_req_peak,cmdlist_req_max,"
                   "skipped_3d_frames,stereo_mode,cmd_bytes_peak,eye_l,eye_r,eye_mono,comp_px,"
                   "proj_world_per_f,proj_other_per_f,proj_ortho_per_f,"
                   "c3_tried,c3_both,c3_parse_ms,c3_copy_kb,l_imgs,l_vbl1,l_vbl2,l_vbl3,l_vbl4p,"
                   "r_imgs,r_vbl2,eyes_split,pace_wait_ms,pace_late,gpu_both_ms,gpu_one_ms,"
                   "opt_rd,ctx_blocks,lr_blocks,clamp_blocks,lr_far,leaves,rebuilds,heap0_free_kb,"
                   "heap0_largest_kb,heap1_free_kb,heap1_largest_kb,guard_ok,c3_top_reason\n";
            const u32 n = s_dumpCount < kDumpSeconds ? s_dumpCount : kDumpSeconds;
            for (u32 i = 0; i < n; i++)
            {
                const Snapshot &d = s_dump[i];
                const double f = d.frames ? d.frames : 1;
                csv += Utils::Format("%lu,%.2f,%.3f,%.3f,%.3f,%.1f,%.1f,", i + 1,
                                     d.windowTicks ? d.frames * (double)kTicksPerSec / d.windowTicks : 0.0,
                                     Ms(d.frameTicks) / f, Ms(d.presentTicks) / f, Ms(d.world.ticks) / f,
                                     d.draws / f, d.drawsIndexed / f);
                csv += TimedCsv(Sum(d.syncBuild)) + "," + TimedCsv(Sum(d.rebuildStart)) + "," +
                       TimedCsv(Sum(d.rebuildEnd)) + ",";
                csv += Utils::Format("%lu,%.0f,", d.rebuildEnd[2].n,
                                     d.rebuildEnd[2].n ? Us(d.rebuildEnd[2].ticks) / d.rebuildEnd[2].n : 0.0);
                csv += Utils::Format("%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,", Ms(d.jobs.ticks) / f,
                                     d.game[0].exclMs / f, d.game[0].inclMs / f, d.game[1].exclMs / f,
                                     d.game[1].inclMs / f, d.game[2].inclMs / f, d.game[3].inclMs / f);
                csv += Utils::Format("%u,%s,%s,%d,%d,%03X,", d.sliderPct,
                                     stereo::StateName(static_cast<stereo::State>(d.stereoState)),
                                     d.stereoOn ? "on" : "off", d.presetPx, d.disparity, d.mode);
                csv += Utils::Format("%.3f,%.3f,%.3f,%.3f,%lu,%.2f,%lu,%.3f,%lu,%.3f,%.2f,%u,%u,%lu,",
                                     Ms(d.flush.ticks) / f, Ms(d.wait.ticks) / f, Ms(d.submit.ticks) / f,
                                     d.gpu.n ? Ms(d.gpu.ticks) / d.gpu.n : 0.0, d.gpu.n,
                                     Ms(d.worstFrameTicks), d.slowFrames, Ms(d.jobRun.maxTicks), d.jobRun.n,
                                     Ms(d.osd.ticks) / f, Ms(d.osdFrameTicks), d.reqPeak, d.reqMax, d.reqSkips);
                csv += Utils::Format("%s,%lu,%lu,%lu,%lu,%d,%.1f,%.1f,%.1f,",
                                     stereo::ModeName(static_cast<stereo::Mode>(d.stereoMode)), d.cmdBytesPeak,
                                     d.eyeL, d.eyeR, d.eyeMono, d.compPx, d.projWorld / f, d.projOther / f,
                                     d.projOrtho / f);
                const ComfortStats &c = d.c3;
                const u32 nL = c.hist[0][0] + c.hist[0][1] + c.hist[0][2] + c.hist[0][3];
                const u32 nR = c.hist[1][0] + c.hist[1][1] + c.hist[1][2] + c.hist[1][3];
                u32 top = 0;
                for (u32 r = 1; r < kReasonCount; r++)
                    if (c.reason[r] > c.reason[top])
                        top = r;
                csv += Utils::Format("%lu,%lu,%.3f,%.1f,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%lu,%.3f,%lu,%.3f,%.3f,",
                                     c.tried, c.replayed, c.tried ? Ms(c.parseTicks) / c.tried : 0.0,
                                     c.replayed ? c.copyBytes / 1024.0 / c.replayed : 0.0, nL, c.hist[0][0],
                                     c.hist[0][1], c.hist[0][2], c.hist[0][3], nR, c.hist[1][1], c.eyeSplit,
                                     c.paceRuns ? Ms(c.paceWaitTicks) / c.paceRuns : 0.0, c.paceLate,
                                     c.gpuCN ? Ms(c.gpuCTicks) / c.gpuCN : 0.0, c.gpuMN ? Ms(c.gpuMTicks) / c.gpuMN : 0.0);
                const WorldStats &w = d.w;
                csv += Utils::Format("%ld,%ld,%ld,%ld,%.1f,%d,%u,%lu,%lu,%lu,%lu,%u,%s\n", static_cast<long>(w.optRd),
                                     static_cast<long>(w.ctxBlocks), static_cast<long>(w.lrBlocks),
                                     static_cast<long>(w.clampBlocks), w.lrFar, static_cast<int>(w.leaves),
                                     static_cast<unsigned>(w.rebuilds),
                                     w.heapCount > 0 ? w.heap[0].freeKB : 0UL, w.heapCount > 0 ? w.heap[0].largestKB : 0UL,
                                     w.heapCount > 1 ? w.heap[1].freeKB : 0UL, w.heapCount > 1 ? w.heap[1].largestKB : 0UL,
                                     static_cast<unsigned>(w.guardOk),
                                     top && c.reason[top] ? comfort::ReasonName(static_cast<u8>(top)) : "none");
            }
            const bool ok = AppendFile(s_csvPath, csv, true);
            // Short and white: CTRPF also draws notifications into the right
            // eye (10 px to the left), and long green ones were part of the
            // v0.1.0 "green text" report.
            const char *name = std::strrchr(s_csvPath, '/');
            name = name ? name + 1 : s_csvPath;
            if (ok)
            {
                s_dumpMsg = "done";
                OSD::Notify(std::string("Saved ") + name);
            }
            else
            {
                s_dumpMsg = "CSV write failed";
                OSD::Notify(std::string("CSV write failed: ") + name);
            }
        }
    }
}

// Tick stubs (stereo_stubs.s), game main thread. Even id = before the sub,
// odd id = after. Integer-only, a few loads/stores; nothing when inactive.
extern "C" void mc3d_ptick(u32 id)
{
    using namespace mc3d::prof;
    // v0.3: right before RunCmdlist (0x4F8A30): finish the GPU-state shadow
    // of this list and run the 30 fps pacer, whether or not stats are on.
    if (id == 4)
    {
        mc3d::comfort::BeforeRunCmdlist();
        const mc3d::game::GameVersion *v = mc3d::version::Current();
        if (v && v->gxDeviceBase && v->devRunList)
        {
            const u32 dev = *reinterpret_cast<volatile u32 *>(v->gxDeviceBase + 4);
            if (dev)
            {
                // GPU time per list (also feeds the Comfort per-kind split)
                const u32 list = *reinterpret_cast<volatile u32 *>(dev + v->devRunList) & 1;
                const u32 t = Now32();
                __atomic_store_n(&s_submitTick[list], t ? t : 1u, __ATOMIC_RELEASE);
            }
        }
    }
    if (!s_active)
        return;
    const u32 now = Now32();
    const u32 slot = (id >> 1) & 3;
    if ((id & 1) == 0)
    {
        s_t0[slot] = now;
        if (slot == 2)
        {
            // (the GPU-time submit tick is stored above for id 4, after the pacer)
            const mc3d::game::GameVersion *v = mc3d::version::Current();
            // The list about to run is still the bound one (RunCmdlist 0x7C688C
            // reads [nngxState+0x9C]): its request count is final for this frame.
            if (v && v->nngxState && v->offBoundCmdlist && v->clReqMax && v->clReqUsed)
            {
                const u32 cl = *reinterpret_cast<volatile u32 *>(v->nngxState + v->offBoundCmdlist);
                if (cl && !(cl & 3))
                    NoteCmdlistReqs(*reinterpret_cast<volatile u32 *>(cl + v->clReqUsed),
                                    *reinterpret_cast<volatile u32 *>(cl + v->clReqMax));
            }
        }
        return;
    }
    const u32 dt = now - s_t0[slot];
    switch (slot)
    {
        case 0: Record(s_live.flush, dt); break;
        case 1: Record(s_live.wait, dt); break;
        case 2: Record(s_live.submit, dt); break;
        case 3: Record(s_live.jobRun, dt); break;
    }
}
