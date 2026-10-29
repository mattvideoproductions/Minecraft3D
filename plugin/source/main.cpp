// Minecraft 3D mod - Luma3DS 3GX plugin (CTRPluginFramework)
// Minecraft: New Nintendo 3DS Edition, USA 00040000001B8700.
#include <CTRPluginFramework.hpp>
#include <cstring>
#include <string>
#include <vector>

#include "config.hpp"
#include "game_versions.hpp"
#include "hooks.hpp"
#include "input_swap.hpp"
#include "memory.hpp"
#include "profiler.hpp"
#include "stereo.hpp"
#include "version.hpp"
#include "version_check.hpp"
#include "comfort.hpp"
#include "world_opts.hpp"

extern "C" void svcInvalidateEntireInstructionCache(void);   // Luma custom SVC (libctrpf csvc.s)

// ---------------------------------------------------------------------------
// C-stick fix (v0.1.1). libctru's hidInit() - which CTRPF calls right after
// PatchProcess, BEFORE the game is allowed to start - ends with
//     if (hidShouldUseIrrst()) return irrstInit();
// and the default hidShouldUseIrrst() is "is this a New 3DS". irrstInit()
// opens ir:rst and sends IRRST Initialize (header 0x00020080, 10 ms, raw 0).
// Minecraft never uses ir:rst: its only IR service is ir:USER (string at
// 0x991284, opened by the SDK IR library at 0x132974), i.e. it reads the
// New 3DS C-stick through the ir module's ir:USER (Circle Pad Pro) path.
// With ir:rst already initialised by the plugin, that path gets no stick
// data on hardware (Azahar does not model this). The weak libctru symbol is
// overridden here so CTRPF never touches ir:rst. CTRPF only needs the face
// buttons (hotkey = Select). `irrst=on` in mc3d_cfg.txt restores the old
// behaviour for an A/B test on hardware. Evidence: the project's design notes (not in this repo) "C-stick".
extern "C" bool hidShouldUseIrrst(void)
{
    return mc3d::cfg::IrrstAllowed();
}

namespace CTRPluginFramework
{
    // ---------------------------------------------------------------------
    // Runs once, before the game starts executing.
    // ---------------------------------------------------------------------
    void PatchProcess(FwkSettings &settings)
    {
        settings.WaitTimeToBoot = Seconds(3);

        mc3d::cfg::Load();                   // per-hook flags + irrst= (read before CTRPF's hidInit)
        mc3d::version::Select();
        if (!mc3d::version::IsSupported())
            return;                          // idle: no hooks, no patches

        mc3d::mem::PrepareTextWrites();      // RWX once, before the game runs
        // Order matters (v0.3): the profiler owns the world-render hook and
        // the RunCmdlist site that Comfort 3D builds on.
        mc3d::prof::InstallHooks();          // idle until the profiler is switched on
        mc3d::stereo::InstallHooks();        // present() frame boundary + stereo (v1.0 kill switch / v9.12)
        mc3d::comfort::Install();            // v0.3: top-clear mark + level-render site
        mc3d::world::Install();              // v0.3: render-distance clamp site
        mc3d::input::Install();              // v0.3.3: A/B swap (game pad reads only)

        // CTRPF invalidates the I-cache BEFORE it writes each hook branch
        // (HookManager.cpp); no game thread has run yet, but close the window.
        svcInvalidateEntireInstructionCache();
    }

    // ---------------------------------------------------------------------
    // Menu. Toggle entries carry a no-op game function so CTRPF shows a
    // checkbox; PollMenu() mirrors them into the modules every frame on the
    // plugin thread, so every option is reversible at runtime.
    // ---------------------------------------------------------------------
    static void NoOp(MenuEntry *) {}

    static MenuEntry *e_stereo, *e_forceSave, *e_fallback, *e_leftSource, *e_stereoDebug, *e_osd, *e_log;
    static MenuEntry *e_turnComp, *e_leftVram, *e_menuPop, *e_mode, *e_depth;
    static MenuEntry *e_lock, *e_autoLower;
    static MenuEntry *e_gov, *e_rd;             // v0.3.2
    static MenuEntry *e_abSwap;                 // v0.3.3

    namespace osd { void Build(); }

    // Azahar v0.3.0: closing the plugin menu with Select sometimes passed the
    // press on to the game (which opens its pause menu). CTRPF calls this
    // on the plugin thread BEFORE the game resumes: hold the game until
    // Select is released (at most 1 s), so it never sees the press.
    static void OnPauseResume(bool goingToPause)
    {
        if (goingToPause)
            return;
        Controller::Update();
        for (int i = 0; i < 60 && Controller::IsKeyDown(Key::Select); i++)
        {
            svcSleepThread(16000000);
            Controller::Update();
        }
    }

    static u32 s_polls = 0;
    static bool s_loadedNotified = false;

    static void PollMenu()
    {
        namespace st = mc3d::stereo;
        st::SetEnabled(e_stereo->IsActivated());
        mc3d::prof::SetStereoMenuState(e_stereo->IsActivated());
        if (e_forceSave)
            st::SetForceCmdlistSave(e_forceSave->IsActivated());
        if (e_fallback)
            st::SetFallback(e_fallback->IsActivated());
        if (e_leftSource)
            st::SetRightSource(e_leftSource->IsActivated() ? st::RightSource::LeftBuffer
                                                           : st::RightSource::VramCopy);
        if (e_turnComp)
            st::SetTurnComp(e_turnComp->IsActivated());
        if (e_leftVram)
            st::SetLeftVram(e_leftVram->IsActivated());
        if (e_menuPop)
            st::SetMenuPop(e_menuPop->IsActivated());
        if (e_lock)
            st::SetLock30(e_lock->IsActivated());
        if (e_autoLower)
            mc3d::world::SetAutoLower(e_autoLower->IsActivated());
        if (e_abSwap)
            mc3d::input::SetSwap(e_abSwap->IsActivated());
        // v0.3.2: governor (auto-flatten) is an experimental opt-in; only
        // pass a change on (SetGovernor(false) also ends a cool-down).
        if (e_gov && e_gov->IsActivated() != mc3d::comfort::GovernorEnabled())
            mc3d::comfort::SetGovernor(e_gov->IsActivated());
        // v0.3.2: the render distance entry shows what the game really
        // uses (refusals, 3D auto-lower), refreshed once per second.
        static u32 s_rdNamePolls = 0;
        if (e_rd && ++s_rdNamePolls >= 60)
        {
            s_rdNamePolls = 0;
            const std::string n = mc3d::world::RdStatusName();
            if (e_rd->Name() != n)
                e_rd->Name() = n;
        }
        if (const char *n = mc3d::world::TakeNotice())
            OSD::Notify(n);
        const bool osdOn = e_osd->IsActivated(), logOn = e_log->IsActivated();
        mc3d::prof::SetEnabled(osdOn || logOn);
        mc3d::prof::SetLogging(logOn);
        mc3d::prof::SetOsdShown(osdOn);
        mc3d::prof::Poll();
        osd::Build();

        // "Loaded" note: after ~10 s of game frames, i.e. after the loading
        // screen, so it does not stack on CTRPF's own "Plugin ready!" there
        // (v0.1.0 showed white flicker over the two at load). Short + white.
        if (!s_loadedNotified && ++s_polls >= 600)
        {
            s_loadedNotified = true;
            if (mc3d::version::IsSupported())
                OSD::Notify(MC3D_NAME " ready - Select: menu");
        }
    }

    static std::string DepthEntryName()
    {
        namespace st = mc3d::stereo;
        return std::string("Depth: ") + (st::GetMode() != st::Mode::Flat ? st::TdPresetName(st::TdPreset())
                                                                           : st::PresetName(st::Preset()));
    }

    static void DepthPresetMenuFunc(MenuEntry *entry)
    {
        namespace st = mc3d::stereo;
        std::vector<std::string> options;
        const bool td = st::GetMode() != st::Mode::Flat;
        for (int i = 0; i < (td ? st::TdPresetCount() : st::PresetCount()); i++)
            options.push_back(td ? st::TdPresetName(i) : st::PresetName(i));
        Keyboard kb(td ? Utils::Format(
                           "3D strength at full slider. deep = how far\n"
                           "the horizon sits behind the screen; out = the\n"
                           "most anything comes out (face to a wall);\n"
                           "hand = your held item. Now: %s", st::TdPresetName(st::TdPreset()))
                       : Utils::Format(
                           "How far the picture sits behind the screen at\n"
                           "full 3D slider (the whole picture is one flat\n"
                           "plane). Bigger = deeper. Pop-out = in front.\n"
                           "Current: %s", st::PresetName(st::Preset())), options);
        int r = kb.Open();
        if (r >= 0)
        {
            if (td)
                st::SetTdPreset(r);
            else
                st::SetPreset(r);
            entry->Name() = DepthEntryName();
        }
    }

    // v0.3.2: top-screen HUD size
    static std::string HudName()
    {
        return std::string("Top HUD size: ") + mc3d::stereo::HudSizeName(mc3d::stereo::HudSize());
    }

    static void HudMenuFunc(MenuEntry *entry)
    {
        namespace st = mc3d::stereo;
        std::vector<std::string> options = { "Normal (the game's size, default)", "Small (80 %)",
                                             "Smaller (64 %)" };
        Keyboard kb("Size of the hearts, hotbar, chat and other\n"
                    "game HUD on the TOP screen, in a world. Shrinks\n"
                    "toward the centre. EXPERIMENTAL: the pause\n"
                    "menu, inventory and chests shrink too. Bottom\n"
                    "screen and touch are not changed.", options);
        const int r = kb.Open();
        if (r >= 0 && r < 3)
        {
            st::SetHudSize(r);
            entry->Name() = HudName();
        }
    }

    static void ModeMenuFunc(MenuEntry *entry)
    {
        namespace st = mc3d::stereo;
        const st::Mode modes[] = { st::Mode::Comfort, st::Mode::TrueDepth, st::Mode::Flat };
        std::vector<std::string> options;
        for (st::Mode m : modes)
            options.push_back(st::ModeMenuName(m));
        Keyboard kb(Utils::Format(
                    "Comfort 3D: both eyes drawn together, 30 fps.\n"
                    "Alternate eyes: v0.2, one eye per frame (can\n"
                    "hurt when turning). Flat: one flat picture.\n"
                    "Now: %s", st::ModeMenuName(st::GetMode())), options);
        int r = kb.Open();
        if (r >= 0 && r < 3)
        {
            st::SetMode(modes[r]);
            entry->Name() = std::string("Mode: ") + st::ModeMenuName(modes[r]);
            if (e_depth)
                e_depth->Name() = DepthEntryName();
        }
    }

    // ---- v0.3 world options ----
    static std::string RdName() { return mc3d::world::RdStatusName(); }     // v0.3.2: what the game uses

    static void RdMenuFunc(MenuEntry *entry)
    {
        namespace w = mc3d::world;
        const int front = w::LevelsFront(), back = w::LevelsBack();
        std::vector<std::string> options = { "Game setting (no change)" };
        std::vector<int> values = { 0 };
        if (front > 0 && back >= front)
            for (int c = front < 2 ? 2 : front; c <= back + 2; c++)
            {
                options.push_back(c > back ? Utils::Format("%d chunks - EXPERIMENTAL (usually refused)", c)
                                           : Utils::Format("%d chunks", c));
                values.push_back(c);
            }
        Keyboard kb(Utils::Format(
                    "How far the world loads and draws (normal\n"
                    "play). Game range %d-%d. Above %d needs free\n"
                    "memory the game does not leave in a world, so\n"
                    "it is normally REFUSED (the menu then says so).",
                    front, back, back), options);
        const int r = kb.Open();
        if (r >= 0 && r < static_cast<int>(values.size()))
        {
            w::SetRenderDistance(values[r]);
            entry->Name() = values[r] == 0 ? std::string("Render distance: game setting")
                                           : Utils::Format("Render distance: %d (applies in a world)", values[r]);
        }
    }

    static std::string Rd3DName()
    {
        const int c = mc3d::world::Distance3D();
        return c == 0 ? std::string("3D render distance: auto") : Utils::Format("3D render distance: %d", c);
    }

    static void Rd3DMenuFunc(MenuEntry *entry)
    {
        std::vector<std::string> options = { "Auto (4 chunks, 3 if yours is 4 or less)", "4 chunks", "3 chunks",
                                             "2 chunks - EXPERIMENTAL" };
        const int values[] = { 0, 4, 3, 2 };
        Keyboard kb("How far the world is DRAWN while 3D is on\n"
                    "(never more than your normal distance). Less\n"
                    "= smoother 3D. Loading is not changed.", options);
        const int r = kb.Open();
        if (r >= 0 && r < 4)
        {
            mc3d::world::SetDistance3D(values[r]);
            entry->Name() = Rd3DName();
        }
    }

    static std::string LeavesName()
    {
        const auto m = mc3d::world::GetLeaves();
        return std::string("Fast leaves: ") + (m == mc3d::world::Leaves::Auto ? "auto (with 3D)"
                                              : m == mc3d::world::Leaves::Fast ? "always" : "off");
    }

    static void LeavesMenuFunc(MenuEntry *entry)
    {
        std::vector<std::string> options = { "Auto: fast in Comfort 3D mode (default)", "Always fast",
                                             "Off (the game's see-through leaves)" };
        Keyboard kb("Fast leaves are solid (not see-through): much\n"
                    "less work in forests. Every switch rebuilds the\n"
                    "world around you: a few seconds of blocks\n"
                    "popping in. Auto follows the MODE, not the slider.", options);
        const int r = kb.Open();
        if (r >= 0 && r < 3)
        {
            mc3d::world::SetLeaves(r == 0 ? mc3d::world::Leaves::Auto
                                          : r == 1 ? mc3d::world::Leaves::Fast : mc3d::world::Leaves::Fancy);
            entry->Name() = LeavesName();
        }
    }

    static std::string FarName()
    {
        const int b = mc3d::world::FarFog();
        return b == 0 ? std::string("Far fog (exp.): off") : Utils::Format("Far fog (exp.): %d blocks", b);
    }

    static void FarMenuFunc(MenuEntry *entry)
    {
        std::vector<std::string> options = { "Off (default)", "48 blocks", "32 blocks", "24 blocks" };
        const int values[] = { 0, 48, 32, 24 };
        Keyboard kb("EXPERIMENTAL. Pulls the fog closer; leaves\n"
                    "beyond it are drawn solid, so forests get\n"
                    "lighter without a rebuild. Untested visually.", options);
        const int r = kb.Open();
        if (r >= 0 && r < 4)
        {
            mc3d::world::SetFarFog(values[r]);
            entry->Name() = FarName();
        }
    }

    static void FallbackShiftMenuFunc(MenuEntry *)
    {
        Keyboard kb(Utils::Format("Fallback depth: right-eye shift in rows (pixels)\n"
                                  "at full slider, -12..12. Negative = other direction.\nCurrent: %d",
                                  mc3d::stereo::FallbackShift()));
        kb.IsHexadecimal(false);
        float value = 0.0f;
        if (kb.Open(value, static_cast<float>(mc3d::stereo::FallbackShift())) == 0)
            mc3d::stereo::SetFallbackShift(static_cast<int>(value < 0.0f ? value - 0.5f : value + 0.5f));
    }

    static void ShowStatsMenuFunc(MenuEntry *)
    {
        mc3d::prof::Snapshot s;
        std::string msg;
        if (!mc3d::prof::Active())
            msg = "The profiler is off. Turn on \"Show stats on bottom\nscreen\" or \"Log stats to SD\", play a few seconds,\nthen open this again.\n\n";
        if (mc3d::prof::Latest(s))
            msg += Utils::Format("Second %lu, %lu frames\n", s.second, s.frames) + mc3d::prof::Summary(s, true);
        else
            msg += "No statistics yet.";
        MessageBox(MC3D_NAME " - latest stats", msg)();
    }

    static void DumpMenuFunc(MenuEntry *)
    {
        int p = mc3d::prof::DumpProgress();
        if (p >= 0)
        {
            MessageBox(MC3D_NAME, Utils::Format("Recording in progress: %d / 60 s", p))();
            return;
        }
        if (!mc3d::stereo::FrameHookInstalled())
        {
            MessageBox(MC3D_NAME, "Frame hook not installed - nothing to record.")();
            return;
        }
        mc3d::prof::StartDump();
        MessageBox(MC3D_NAME, std::string("Recording 60 s of stats (one row per second) to\n") +
                                  mc3d::prof::CsvPath() +
                                  "\nEvery recording gets a new file number; old ones\nare kept. Close the menu and play normally.")();
    }

    static void StatusMenuFunc(MenuEntry *)
    {
        using namespace mc3d;
        std::string msg;
        msg += Utils::Format("Title ID: %016llX  title ver %u\n", Process::GetTitleID(), Process::GetVersion());
        msg += std::string("Version check: ") + version::StatusString(version::GetStatus()) +
               " (" + version::VersionName() + ")\n";
        for (u32 i = 0; i < version::TablesTried(); i++)
            msg += Utils::Format("  table %s: %s\n", game::kVersions[i]->name,
                                 version::FirstMismatch(i) ? Utils::Format("mismatch at %08lX", version::FirstMismatch(i)).c_str()
                                                           : "match");
        msg += std::string("Patching: ") + (mem::PatchingAllowed() ? "enabled" : "LOCKED") + "\n";
        msg += Utils::Format("Hooks enabled: %03lX (%s)\n", cfg::Hooks(),
                             cfg::FromFile() ? "mc3d_cfg.txt" : "default: all");
        msg += std::string("C-stick: plugin ir:rst ") + (cfg::IrrstAllowed() ? "ON (irrst=on, test only)\n"
                                                                            : "off (game C-stick native)\n");
        msg += stereo::HookStatus() + prof::HookStatus() + input::HookStatus();
        if (stereo::IsFlatParallax())
            msg += Utils::Format("Comfort 3D: %s; 3D-distance site %s\n",
                                 comfort::Ready() ? "ready" : comfort::NotReadyWhy(),
                                 world::ClampInstalled() ? "ok" : "off");
        msg += stereo::DebugText() + "\n";
        msg += std::string("Dump: ") + prof::DumpMessage();
        MessageBox(MC3D_NAME, msg)();
    }

    // ---------------------------------------------------------------------
    // OSD (bottom screen: stays readable with 3D on)
    // ---------------------------------------------------------------------
    // DrawStatus runs on the game's GSP event thread (CTRPF OSD hook inside
    // nngxSwapBuffersByAddress, ~4 KiB stack, high priority, same core as the
    // game's main thread). v0.1.0 drew up to 20 lines of up to 79 characters
    // with CTRPF's per-pixel font renderer EVERY frame there (~40k pixel
    // writes through function pointers, plus the overflow of long lines all
    // piled onto the clamped x=320 column). v0.1.1:
    //   - the text is built on the plugin thread at most once per second,
    //     short lines (<= 52 chars) only;
    //   - the GSP thread renders it with CTRPF only when it changed, then
    //     copies the rendered box into a cache; every other frame it just
    //     memcpy's the cached box back (one contiguous run per screen column).
    namespace osd
    {
        constexpr int kLines = 12, kLineCap = 53;            // 53 chars x 6 px = 318 px
        constexpr int kMaxW = 320, kMaxH = kLines * 10 + 4;
        struct LineSet { std::string line[kLines]; int count = 0; int width = 0; };
        LineSet  s_set[2];
        volatile int s_pub = -1;        // published set (-1 = nothing to draw)
        volatile int s_reading = -1;    // set the GSP thread is drawing now
        volatile u32 s_gen = 0;         // bumps every time a new set is published
        u64 s_lastBuild = 0;
        u32 s_lastMask = 0;

        // GSP-thread cache of the rendered box (column-major like the FB)
        u8  s_cache[kMaxW * kMaxH * 4];
        u32 s_cacheGen = ~0u, s_cacheBpp = 0, s_cacheW = 0, s_cacheH = 0, s_cacheFmt = ~0u;

        void Init()
        {
            for (auto &set : s_set)
                for (auto &l : set.line)
                    l.reserve(kLineCap + 1);
        }

        void Publish(const std::string &text)
        {
            int w = s_pub == 0 ? 1 : 0;
            if (__atomic_load_n(&s_reading, __ATOMIC_ACQUIRE) == w)
                return;                 // being drawn right now: try next time
            LineSet &set = s_set[w];
            int n = 0, maxLen = 0;
            size_t start = 0;
            while (start < text.size() && n < kLines)
            {
                size_t nl = text.find('\n', start);
                size_t len = (nl == std::string::npos ? text.size() : nl) - start;
                if (len > static_cast<size_t>(kLineCap))
                    len = kLineCap;     // never past the screen edge / reserved capacity
                set.line[n++].assign(text, start, len);
                if (static_cast<int>(len) > maxLen)
                    maxLen = static_cast<int>(len);
                if (nl == std::string::npos)
                    break;
                start = nl + 1;
            }
            set.count = n;
            set.width = maxLen * 6 + 4 > kMaxW ? kMaxW : maxLen * 6 + 4;
            __atomic_store_n(&s_pub, w, __ATOMIC_RELEASE);
            __atomic_store_n(&s_gen, s_gen + 1, __ATOMIC_RELEASE);
        }

        // plugin thread, every frame: cheap unless a rebuild is due
        void Build()
        {
            const bool prof = e_osd && e_osd->IsActivated();
            const bool dbg = e_stereoDebug && e_stereoDebug->IsActivated();
            const int p = mc3d::prof::DumpProgress();
            const u32 mask = (prof ? 1u : 0u) | (dbg ? 2u : 0u) | (p >= 0 ? 4u : 0u);
            const u64 now = svcGetSystemTick();
            if (mask == s_lastMask && now - s_lastBuild < mc3d::prof::kTicksPerSec)
                return;                 // at most once per second (or when an item is toggled)
            s_lastMask = mask;
            s_lastBuild = now;

            std::string text;
            if (prof)
            {
                mc3d::prof::Snapshot s;
                text += mc3d::prof::Latest(s) ? mc3d::prof::OsdText(s) : std::string("profiler: waiting...");
                text += "\n";
            }
            if (p >= 0)
                text += Utils::Format("CSV recording %d/60 s\n", p);
            if (dbg)
                text += mc3d::stereo::DebugText() + "\n";
            if (text.empty())
            {
                s_pub = -1;
                return;
            }
            Publish(text);
        }
    }

    // GSP event thread
    static bool DrawStatus(const Screen &screen)
    {
        if (screen.IsTop)
            return false;
        const int r = __atomic_load_n(&osd::s_pub, __ATOMIC_ACQUIRE);
        if (r < 0)
            return false;
        __atomic_store_n(&osd::s_reading, r, __ATOMIC_RELEASE);
        // Re-check: the writer may have flipped between the two loads.
        if (__atomic_load_n(&osd::s_pub, __ATOMIC_ACQUIRE) != r)
        {
            __atomic_store_n(&osd::s_reading, -1, __ATOMIC_RELEASE);
            return false;
        }
        const u32 t0 = static_cast<u32>(svcGetSystemTick());
        const osd::LineSet &set = osd::s_set[r];
        const u32 gen = __atomic_load_n(&osd::s_gen, __ATOMIC_ACQUIRE);
        const u32 bpp = screen.BytesPerPixel;
        const u32 w = static_cast<u32>(set.width);
        const u32 h = static_cast<u32>(set.count) * 10 + 4;
        const u32 colBytes = h * bpp;

        if (bpp < 2 || bpp > 4 || h > static_cast<u32>(osd::kMaxH) || w == 0)
        {
            __atomic_store_n(&osd::s_reading, -1, __ATOMIC_RELEASE);
            return false;
        }

        if (gen != osd::s_cacheGen || bpp != osd::s_cacheBpp || w != osd::s_cacheW ||
            h != osd::s_cacheH || static_cast<u32>(screen.Format) != osd::s_cacheFmt)
        {
            // Changed (<= once per second): render with CTRPF, then keep a copy.
            screen.DrawRect(0, 0, w, h, Color::Black);
            int y = 2;
            for (int i = 0; i < set.count; i++, y += 10)
                screen.Draw(set.line[i], 0, y, Color::White, Color::Black);
            for (u32 x = 0; x < w; x++)
                std::memcpy(osd::s_cache + x * colBytes, screen.GetFramebuffer(x, h - 1), colBytes);
            osd::s_cacheGen = gen;
            osd::s_cacheBpp = bpp;
            osd::s_cacheW = w;
            osd::s_cacheH = h;
            osd::s_cacheFmt = static_cast<u32>(screen.Format);
            mc3d::prof::MarkOsdRedraw();    // kept out of "worst frame" (logged as "osd frame")
        }
        else
        {
            // Every other frame: one memcpy per screen column (pixel (x, y)
            // lives at x*stride + (239-y)*bpp, so y = h-1 .. 0 is contiguous).
            for (u32 x = 0; x < w; x++)
                std::memcpy(screen.GetFramebuffer(x, h - 1), osd::s_cache + x * colBytes, colBytes);
        }
        __atomic_store_n(&osd::s_reading, -1, __ATOMIC_RELEASE);
        mc3d::prof::RecordOsdDraw(static_cast<u32>(svcGetSystemTick()) - t0);   // logged as osd ms/frame
        return true;
    }

    // ---------------------------------------------------------------------
    int main(void)
    {
        PluginMenu *menu = new PluginMenu("Minecraft 3D mod", 0, 3, 3,
            MC3D_NAME "\n"
            "Adds stereoscopic 3D to Minecraft: New Nintendo 3DS\n"
            "Edition (USA) and a performance profiler.\n"
            "v0.3: Comfort 3D - both eyes drawn in the same\n"
            "frame at a steady 30 fps.\n"
            "Press Select to open/close this menu.");
        menu->SynchronizeWithFrame(true);

        if (mc3d::version::GetStatus() == mc3d::version::Status::WrongTitle)
            OSD::Notify(MC3D_NAME ": wrong title, plugin idle", Color::Red);
        else if (!mc3d::version::IsSupported())
            OSD::Notify(MC3D_NAME ": unsupported game version, plugin idle", Color::Red);

        menu->Append(new MenuEntry("Plugin status (" MC3D_VERSION_STR ")", nullptr, StatusMenuFunc,
            "Opens a technical report: which game version was\n"
            "detected, which hooks are installed, the 3D state\n"
            "and memory. Only needed when something is wrong;\n"
            "a photo of it helps with bug reports."));

        // ---- Stereo 3D ----
        MenuFolder *stereo = new MenuFolder("Stereo 3D",
            "Settings for the 3D effect. The 3D slider on the\n"
            "console still controls how strong it is; slider\n"
            "at the bottom = normal 2D game.");
        e_forceSave = e_fallback = e_leftSource = nullptr;
        e_turnComp = e_leftVram = e_menuPop = e_mode = e_depth = nullptr;
        e_lock = e_autoLower = nullptr;
        e_gov = e_rd = nullptr;
        if (mc3d::stereo::IsFlatParallax())
        {
            // v9.12.0 (1.9.19): the game's right-eye renderer was removed;
            // v0.2 rebuilds real depth by rendering the eyes alternately.
            namespace st = mc3d::stereo;
            stereo->Append(e_stereo = new MenuEntry("3D on", NoOp,
                "Turns the 3D effect on or off. Off, or the 3D\n"
                "slider down = the normal 2D game, no extra work."));
            e_stereo->Enable();
            stereo->Append(e_mode = new MenuEntry(std::string("Mode: ") + st::ModeMenuName(st::GetMode()),
                                                  nullptr, ModeMenuFunc,
                "Comfort 3D (default): both eyes are drawn in\n"
                "the same frame, so turning stays comfortable;\n"
                "the game runs at a steady 30 fps. Alternate\n"
                "eyes (v0.2) and Flat plane are kept as backups."));
            stereo->Append(e_depth = new MenuEntry(DepthEntryName(), nullptr, DepthPresetMenuFunc,
                "How strong the 3D is at full slider. Deep is\n"
                "the default; Immersive lets near things come\n"
                "out more. 'Zero' = both eyes identical (test)."));
            stereo->Append(new MenuEntry(HudName(), nullptr, HudMenuFunc,
                "Top-screen HUD size (hearts, hotbar, chat) in\n"
                "a world. Normal (default) = the game's size.\n"
                "Small = 80 %, Smaller = 64 %, toward the centre.\n"
                "Experimental: menus over the world shrink too."));
            stereo->Append(e_lock = new MenuEntry("30 fps lock (Comfort 3D)", NoOp,
                "On (default): in Comfort 3D every picture stays\n"
                "on screen for exactly 2 screen refreshes, so the\n"
                "motion is even. Off = as fast as it can (test).\n"
                "3D off or slider down: never locked."));
            if (st::Lock30()) e_lock->Enable();
            stereo->Append(e_turnComp = new MenuEntry("Turn compensation (alternate eyes)", NoOp,
                "Only for 'Experimental: alternate eyes': shifts\n"
                "the older eye while you turn. Off by default.\n"
                "Comfort 3D does not need it."));
            if (st::TurnComp()) e_turnComp->Enable();
            stereo->Append(e_gov = new MenuEntry("Experimental: auto-flatten when slow", NoOp,
                "Off (default): Comfort 3D stays full 3D even\n"
                "when the game slows down. On: after many slow\n"
                "frames it shows the flat plane for 10 s, then\n"
                "tries 3D again (the v0.3.0 behaviour)."));
            if (mc3d::comfort::GovernorEnabled()) e_gov->Enable();
            stereo->Append(e_menuPop = new MenuEntry("3D menus (text pops out)", NoOp,
                "True depth: in menus and loading screens the\n"
                "text and buttons float in front and the\n"
                "panorama sits behind. In-game HUD stays put."));
            if (st::MenuPop()) e_menuPop->Enable();
            stereo->Append(e_leftVram = new MenuEntry("Glitch-line fix (left eye VRAM)", NoOp,
                "On (default): both eyes are shown from fast\n"
                "video memory. Fixes the flickering lines of\n"
                "v0.1.1 in 3D. Off = v0.1.1 behaviour (flat\n"
                "mode only; true depth always uses it)."));
            if (st::LeftVram()) e_leftVram->Enable();
            stereo->Append(e_leftSource = new MenuEntry("Test: right eye = left image", NoOp,
                "Hardware test, normally off. Shows the game's own\n"
                "picture in both eyes with no depth (2D look)."));
            stereo->Append(e_stereoDebug = new MenuEntry("Show 3D debug info", NoOp,
                "Prints technical 3D details on the bottom screen\n"
                "(state, mode, eye, buffers, free video memory).\n"
                "For bug reports; normally off."));
        }
        else
        {
            stereo->Append(e_stereo = new MenuEntry("Stereo 3D enabled", NoOp,
                "Real stereo 3D using the game's built-in right-eye renderer.\n"
                "Follows the 3D slider; slider at 0 = stock 2D."));
            e_stereo->Enable();
            stereo->Append(e_forceSave = new MenuEntry("Force cmdlist save", NoOp,
                "Experimental. Forces the game to save the GPU command list every\n"
                "frame (needed for the right-eye replay). Only applied while 3D is on.\n"
                "Turn on if the stereo debug line shows 'wait-cmdlist'."));
            stereo->Append(e_fallback = new MenuEntry("Fallback mode (flat depth)", NoOp,
                "Instead of real stereo: right eye = the left image shifted.\n"
                "No extra GPU cost, the whole picture sits at one depth."));
            stereo->Append(new MenuEntry("Fallback depth", nullptr, FallbackShiftMenuFunc,
                "Right-eye shift in pixels at full slider for fallback mode."));
            stereo->Append(e_stereoDebug = new MenuEntry("Show 3D debug info", NoOp,
                "Kill-switch flag, cmdlist-saved flag, display mode."));
        }
        menu->Append(stereo);

        // ---- World: render distance, leaves, fog (v0.3) ----
        if (mc3d::stereo::IsFlatParallax())
        {
            MenuFolder *worldF = new MenuFolder("World (distance, leaves)",
                "How far the world is drawn and how leaves look.\n"
                "Shorter distance and solid leaves make 3D\n"
                "smoother. Changes take effect in a world.");
            worldF->Append(e_rd = new MenuEntry(RdName(), nullptr, RdMenuFunc,
                "Your normal render distance (also without 3D).\n"
                "The name shows what the game really uses, e.g.\n"
                "'4 -> 3 in 3D (auto)' or '6 refused (low\n"
                "memory)'. Above the game's maximum = experimental."));
            worldF->Append(e_autoLower = new MenuEntry("Lower distance while 3D is on", NoOp,
                "On (default): in Comfort 3D the world is DRAWN\n"
                "less far (smoother 3D). Turning 3D off brings it\n"
                "back after 5 seconds (at most every 20 s, to\n"
                "avoid rebuilding when you brush the slider)."));
            if (mc3d::world::AutoLower()) e_autoLower->Enable();
            worldF->Append(new MenuEntry(Rd3DName(), nullptr, Rd3DMenuFunc,
                "How far to draw while 3D is on. Auto = 4\n"
                "chunks, or 3 if your normal distance is 4 or\n"
                "less. Never more than your normal distance."));
            worldF->Append(new MenuEntry(LeavesName(), nullptr, LeavesMenuFunc,
                "Solid leaves are much faster in forests.\n"
                "Auto = solid while Comfort 3D is selected (the\n"
                "slider never triggers it). Each switch rebuilds\n"
                "the world near you. Never saved to options.txt."));
            worldF->Append(new MenuEntry(FarName(), nullptr, FarMenuFunc,
                "EXPERIMENTAL, off by default: brings the fog\n"
                "closer so far trees cost less. Not yet\n"
                "checked on screen - tell us how it looks."));
            menu->Append(worldF);
        }

        // ---- Performance ----
        MenuFolder *perf = new MenuFolder("Performance",
            "Tools that measure how fast the game runs, so we\n"
            "can find slowdowns. All off by default; they cost\n"
            "nothing while off.");
        perf->Append(e_osd = new MenuEntry("Show stats on bottom screen", NoOp,
            "Live numbers on the bottom screen, updated once a\n"
            "second: fps, frame time, world-draw time, chunk\n"
            "building, the game's own level/cull timers, 3D.\n"
            "Not saved anywhere - use the log for that."));
        perf->Append(e_log = new MenuEntry("Log stats to SD", NoOp,
            "Writes one line of stats per second (same numbers\n"
            "plus 3D and OSD on/off) to the end of\n"
            "luma/plugins/00040000001B8700/mc3d_log.txt\n"
            "Nothing is shown on screen. Never overwrites."));
        perf->Append(new MenuEntry("Show latest stats", nullptr, ShowStatsMenuFunc,
            "Opens a box with the full numbers for the last\n"
            "second. Needs one of the two options above on."));
        perf->Append(new MenuEntry("Record 60 s to CSV", nullptr, DumpMenuFunc,
            "Records 60 seconds (one row per second) into a new\n"
            "spreadsheet file mc3d_profile_001.csv, _002, ...\n"
            "in luma/plugins/00040000001B8700/. Each recording\n"
            "gets the next number; old files are never replaced."));
        menu->Append(perf);
        if (mc3d::cfg::StereoOpt("log") == 1)
            e_log->Enable();
        if (mc3d::cfg::StereoOpt("osd") == 1)
            e_osd->Enable();

        // ---- v0.3.3: A/B swap. Created LAST on purpose: CTRPF numbers
        // entries in creation order and its optional "auto-save enabled
        // cheats" (CTRPFData.bin) stores those numbers, so every older
        // entry keeps its number and saved state. ----
        e_abSwap = nullptr;
        if (mc3d::input::Available())
        {
            menu->Append(e_abSwap = new MenuEntry("Swap A/B buttons", NoOp,
                "Off (default). On: the game sees A when you\n"
                "press B, and B when you press A: title, menus,\n"
                "inventory, chests and in the world. This menu\n"
                "keeps the real buttons. A switch takes effect\n"
                "once A and B are both released."));
            if (mc3d::input::SwapWanted())
                e_abSwap->Enable();
        }

        // v0.3.2: rotate an over-size log, then one boot line with the
        // settings in effect (plugin thread; always written).
        {
            namespace st = mc3d::stereo;
            namespace w = mc3d::world;
            std::string c = "unsupported game: plugin idle";
            if (mc3d::version::IsSupported() && st::IsFlatParallax())
                c = Utils::Format("mode %s, depth %s, 30fps-lock %s, turn-comp %s, auto-flatten %s, hold %d, ",
                                  st::ModeName(st::GetMode()), st::TdPresetName(st::TdPreset()),
                                  st::Lock30() ? "on" : "off", st::TurnComp() ? "on" : "off",
                                  mc3d::comfort::GovernorEnabled() ? "on" : "off", st::HoldFrames()) +
                    Utils::Format("hud %s, rd %d, lower-in-3D %s, rd3D %d, leaves %d, farfog %d, abswap %s",
                                  st::HudSizeName(st::HudSize()), w::RenderDistance(), w::AutoLower() ? "on" : "off",
                                  w::Distance3D(), static_cast<int>(w::GetLeaves()), w::FarFog(),
                                  !mc3d::input::Available() ? "n/a" : mc3d::input::SwapWanted() ? "on" : "off");
            else if (mc3d::version::IsSupported())
                c = "v1.0 build: game stereo renderer";
            mc3d::prof::BootLog(c + (mc3d::cfg::FromFile() ? " (mc3d_cfg.txt)" : " (defaults)"));
        }

        osd::Init();
        Process::OnPauseResume = OnPauseResume;
        *menu += PollMenu;
        OSD::Run(DrawStatus);

        menu->Run();

        // Run() only returns while the game process is shutting down. Free
        // nothing (deleting the menu crashed Terraria on hardware, LESSONS.md).
        return 0;
    }
}
