#pragma once
// Phase-1 profiler: cheap counters in game hooks, rolled up once per second.
//
// Game side (any thread): hooks add to 32-bit atomics (ldrex/strex). The
// chunk-rebuild hooks keep one accumulator per CPU core (svcGetProcessorID),
// because chunk meshing runs on the Streaming workers pinned to core 2 on
// both builds (the project's design notes (not in this repo) "Threads / cores"): a duration is always
// measured start-to-end on the same core.
// Present hook (main thread): once per second the counters are swapped out
// into a Snapshot (seqlock-published) and, during a dump, into a 60-entry array.
// It also reads the game's own always-on profiler (v9.12: 0xB07AC0) as deltas.
// Plugin thread (Poll): formats + writes the SD log / CSV. No file I/O and no
// float formatting ever runs on a game thread.
#include <3ds/types.h>
#include <string>
#include "c3_stats.hpp"

namespace mc3d::prof
{
    constexpr u32 kTicksPerSec = 268111856u;   // SYSCLOCK_ARM11
    constexpr u32 kCores = 4;                  // svcGetProcessorID: 0..3 on New 3DS

    struct Timed { u32 n, ticks, maxTicks; };

    // One section of the game's built-in profiler, as a 1 s delta.
    struct GameSec { u32 n; float exclMs, inclMs; };
    constexpr u32 kGameSecCount = 4;           // ids 12, 13, 0x15, 0x16 (see profiler.cpp)

    struct Snapshot
    {
        u32   second;        // seconds since the profiler started
        u32   windowTicks;   // length of this window
        u32   frames;
        u32   frameTicks;    // sum of present-to-present
        u32   presentTicks;  // sum of time inside present() (GPU wait for the previous list, vsync pacing)
        Timed world;         // world render (main thread CPU)
        u32   draws, drawsIndexed;
        Timed syncBuild[kCores];    // synchronous chunk build (main thread)
        Timed rebuildStart[kCores]; // RenderChunk::startRebuild
        Timed rebuildEnd[kCores];   // RenderChunk::endRebuild
        Timed jobs;          // WorkerPool main-thread job budget
        // v0.1.1
        Timed jobRun;        // one coroutine inside the job budget (max = worst single job)
        Timed flush, wait, submit;  // present(): cmdlist flush / wait for previous list (GPU) / RunCmdlist
        Timed gpu;           // RunCmdlist -> cmdlist-done callback, per list (GPU busy + queueing)
        Timed osd;           // our bottom-screen OSD draw (GSP thread)
        u32   worstFrameTicks;      // longest present-to-present in the window, OSD redraw frames excluded
        u32   osdFrameTicks;        // longest frame that included an OSD re-render (once per second)
        u32   slowFrames;           // frames > 33.3 ms
        GameSec game[kGameSecCount];
        u8    sliderPct;
        u8    stereoState;   // stereo::State
        s8    disparity;     // signed rows (v9.12 flat), 0 in 2D
        s8    presetPx;      // v9.12 depth preset (pixels at full slider)
        u16   mode;          // nngx display mode (0x700 / 0x702)
        u8    stereoOn;      // menu toggle
        u8    pad;
        // v0.1.1 (audit S1): nngx command-list request queue of the game's lists
        u16   reqPeak;       // highest requests used in one list (read just before RunCmdlist)
        u16   reqMax;        // that list's capacity (0x80)
        u32   reqSkips;      // 3D frames shown 2D because the list was too full for our copies
        // v0.2 M0
        u32   cmdBytesPeak;  // highest command-buffer bytes used by one list (at the top B2L)
        u32   cmdBytesMax;   // that list's buffer size (0xE7400)
        u32   projWorld, projOther, projOrtho;   // WorldConstants::refresh calls by class (window totals)
        u32   eyeL, eyeR, eyeMono;               // frames rendered per eye (true depth)
        u32   projBottom;    // refreshes left alone because they target the bottom screen (aspect gate)
        float kind[4][3];    // up to 4 distinct world-perspective matrices: m[0], m[10], m[14]
        u32   kindN[4];
        u32   vramBHead;     // GxDevice VRAM-B bump pointer (dev+0x64), sampled every second
        u32   vramBHeadMax;  // highest seen since boot
        s16   compPx;        // last turn-compensation shift (px)
        u8    stereoMode;    // stereo::Mode
        u8    pad2;
        // v0.3
        ComfortStats c3;     // replay, fallbacks, pacer, per-eye presentation, GPU by list kind
        WorldStats   w;      // render distance chain, leaves, far fog, heaps
    };

    // PatchProcess: install the profiling hooks (they stay idle until enabled).
    void        InstallHooks();
    std::string HookStatus();

    // Plugin thread
    void        SetEnabled(bool on);         // counters on (log and/or OSD wanted)
    void        SetLogging(bool on);         // append one line per second to mc3d_log.txt
    void        SetOsdShown(bool on);        // context column only
    bool        Enabled();
    void        SetStereoMenuState(bool on); // for the log context
    void        StartDump();                 // 60 s CSV, new numbered file each time
    int         DumpProgress();              // -1 idle, 0..60 seconds captured
    const char *DumpMessage();
    bool        Latest(Snapshot &out);       // false until the first second
    void        Poll();                      // write log line / CSV when due
    std::string Summary(const Snapshot &s, bool multiline);
    std::string OsdText(const Snapshot &s);  // compact, fits the bottom screen

    // Main thread, from the present hook
    bool        Active();                    // enabled or dumping
    void        OnPresent(bool active, u64 startTick, u64 endTick); // every frame

    // GSP thread: OSD draw cost; cmdlist-done for `list` (GPU time)
    void        RecordOsdDraw(u32 ticks);
    void        MarkOsdRedraw();             // this frame re-rendered the OSD text (not a cache copy)

    // Main thread: command-list request counts (stereo_flat.cpp, RunCmdlist tick)
    void        NoteCmdlistReqs(u32 used, u32 max);
    void        NoteReqSkip();
    void        OnCmdlistDone(u32 list);
    bool        RunHookInstalled();          // v0.3: RunCmdlist site wrapped (pacer + shadow need it)
    std::string C3Text(const Snapshot &s);   // v0.3 log fields (one line, " | " separated)

    // v0.2: world-render nesting (0x31DD8C hook; always tracked, even with the profiler off)
    bool        WorldHookInstalled();
    bool        InWorld();                   // main thread: inside the world render now
    bool        TakeWorldSeen();             // main thread: world rendered since the last call
    // v0.2 M0 counters (main thread)
    void        NoteCmdlistBytes(u32 used, u32 size);
    void        NoteProj(int cls);           // 0 world perspective, 1 other perspective, 2 ortho, 3 bottom (skipped)
    void        NoteWorldKind(float m0, float m10, float m14);
    void        NoteVramBHead(u32 head);
    void        NoteEye(int eye, int compPx);   // -1 mono, 0 left, 1 right

    const char *LogPath();
    const char *CsvPath();
    bool        WorldSeenThisFrame();        // main thread: world rendered in this frame so far (not consumed)

    // ---- v0.3.2: log file (plugin thread) ----
    // Every writer appends through AppendLog. Past kLogCap the file is
    // renamed to mc3d_log_old.txt (replacing the previous old one) and a
    // fresh file is started, at boot or mid-session, so logging never stops.
    bool        AppendLog(const std::string &text);
    void        BootLog(const std::string &cfgText);   // main(): rotate if needed + boot header line

    // ---- v0.3.2: per-switch event log ----
    // What the top screen shows, per frame (stereo_flat.cpp).
    enum class Pic : u8 { Off, Full3D, Held, Mono2D, FlatPlane, Alternate, Count };
    enum : u8
    {
        // why: values below kReasonCount are comfort::ReasonName indices
        kWhyNone = 0xFF,
        kWhyOff = 100, kWhySlider0, kWhyUnavailable, kWhyGovernor, kWhyNotReady, kWhyModeFlat, kWhyModeAlt,
        kWhyNoSlot, kWhyNoRoom,
        // render distance
        kWhyRd3DAuto, kWhyRdRestore, kWhyRdSetting, kWhyRdGame, kWhyRdLowMem, kWhyRdNotAllowed, kWhyRdStepDown,
    };
    enum class EvKind : u8 { Switch, Distance, DistanceRefused };
    // Game main thread only (single producer). Never does I/O: the record
    // goes into a 64-slot ring (full = dropped + counted), rate-limited to
    // 12 per second (the rest counted as suppressed). The plugin thread
    // writes the lines at most once per second (Poll).
    void        NoteFrameInterval(u32 gap10);   // every presented frame: interval since the last, 0.1 ms
    void        PushEvent(EvKind k, u8 from, u8 to, u8 why);
    const char *PicName(Pic p);
}
