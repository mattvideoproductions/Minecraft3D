#pragma once
// Stereo 3D. Two designs, picked by the detected game version (game::StereoKind):
//
// KillSwitch (v1.0, stereo.cpp): the game's own (switched-off) right-eye
//   renderer is switched on per frame from the end of GxDevice::present.
//   See the project's design notes (not in this repo) "v1.0 stereo".
//
// v9.12.0 (stereo_flat.cpp), v0.2: two modes.
//   TrueDepth (default): alternate-eye rendering. Each frame the game renders
//     ONE eye: the plugin shears the projection matrix (WorldConstants::refresh
//     hook) for the left or right eye in turn, copies the finished top image
//     into that eye's own VRAM buffer, and presents the newest left and right
//     images together (0x702). Zero extra GPU work; each eye refreshes at half
//     the frame rate. Optional turn compensation re-aligns the older eye by the
//     camera yaw change (scanout row shift). See the project's design notes (not in this repo).
//   Flat: the v0.1 flat plane (one depth), kept as a fallback.
// Both eyes scan out of plugin VRAM-B buffers (v0.2 glitch-line fix: the
// display DMA size comes from the LEFT address, so an FCRAM left + VRAM right
// pair ran the VRAM eye with FCRAM's 64-byte bursts).
//
// Flat details (v0.1): the right-eye renderer was removed
//   from the game, so the plugin
//     - allocates 3 right-eye display buffers once, in VRAM, at the end of
//       GxDevice::init (boot), padded by kPadRows black rows on each side;
//     - after the game's top-screen block-to-linear copy in present(), adds a
//       second copy of the same render target into right buffer [idx] (same
//       command list, same triple-buffer index as the left eye);
//     - in the command-list-done callback, passes that buffer (scanned out
//       N rows earlier/later = N pixels horizontal shift) as the right-eye
//       address of nngxSwapBuffersByAddress and sets display mode 0x702.
//   The whole picture sits on ONE depth plane (no per-object depth).
// With the slider at 0 (or stereo off) nothing is copied, r2 stays 0 and the
// mode stays 0x700: the game runs stock.
#include <3ds/types.h>
#include <string>

// 3D slider position in [0.0, 1.0] (0 = 3D off), from the OS shared page.
extern "C" float stereo_get_slider(void);

namespace mc3d::stereo
{
    enum class State : u8
    {
        Unavailable,    // fields missing for this game version / hooks not installed
        Off,            // 2D, stock behaviour
        Stereo,         // v1.0: real stereo (right-eye replay + 0x702)
        WaitCmdlist,    // v1.0: 3D wanted but the game did not save the cmdlist this frame
        Fallback,       // v1.0: 0x702 + shifted left image
        Fault,          // sanity check failed; stays 2D
        WaitInit,       // v9.12: GxDevice::init not seen yet (no right buffers)
        NoBuffers,      // v9.12: right-eye buffer allocation failed
        Flat,           // v9.12: flat parallax active
        Depth,          // v9.12 v0.2: true depth (alternate-eye) active
        Comfort,        // v9.12 v0.3: both eyes in the same frame (replay)
        Mono3D,         // v9.12 v0.3: Comfort wanted, this frame shown 2D (fallback)
    };

    // v9.12 stereo mode. v0.3: Comfort is the default; the v0.2 alternate-eye
    // mode stays as "Experimental: alternate eyes", Flat as a fallback.
    enum class Mode : u8
    {
        TrueDepth,      // v0.2 alternate-eye ("Experimental: alternate eyes")
        Flat,           // v0.1 flat plane
        Comfort,        // v0.3 same-frame replay, 30 fps lock (default)
    };
    const char *ModeMenuName(Mode m);
    void        SetLock30(bool on);         // Comfort: 30 fps lock (default on)
    bool        Lock30();
    bool        ThreeDActive();             // last frame: 3D on, slider > 0, stereo usable
    bool        ComfortActive();            // last frame: Comfort 3D drawing (slider > 0)
    bool        ComfortSelected();          // Comfort mode + "3D on" + usable, whatever the slider
    int         TdHandPx(int i);
    void        SetMode(Mode m);
    Mode        GetMode();
    const char *ModeName(Mode m);
    int         TdPresetCount();
    const char *TdPresetName(int i);
    void        SetTdPreset(int i);
    int         TdPreset();
    void        SetTurnComp(bool on);       // default off (v0.3)
    bool        TurnComp();
    void        SetLeftVram(bool on);       // flat mode: left eye from VRAM too (default on = glitch fix)
    bool        LeftVram();
    void        SetMenuPop(bool on);        // menus: UI in front, panorama behind (default on)
    bool        MenuPop();
    int         CurrentPresetPx();          // depth px at full slider of the active mode's preset
    void        SetHudSize(int i);          // v0.3.2: top-screen HUD 0 Normal (default), 1 Small, 2 Smaller
    int         HudSize();
    const char *HudSizeName(int i);
    int         HoldFrames();               // v0.3.1 hold limit (cfg hold=N), for the boot log line

    // PatchProcess: install the present() frame hook (also drives the
    // profiler) and, on v9.12, the flat-parallax hooks.
    void        InstallHooks();
    bool        FrameHookInstalled();
    std::string HookStatus();
    bool        IsFlatParallax();           // detected version uses the flat design

    // Plugin thread: options (read by the game threads)
    void        SetEnabled(bool on);            // default on

    // v1.0 only
    void        SetForceCmdlistSave(bool on);   // default off
    void        SetFallback(bool on);           // default off
    void        SetFallbackShift(int rowsAtFullSlider);   // -12..12
    int         FallbackShift();

    // v9.12 only (flat parallax)
    int         PresetCount();
    const char *PresetName(int i);
    int         PresetPixels(int i);        // >0: behind the screen, <0: pops out
    void        SetPreset(int i);
    int         Preset();
    enum class RightSource : u8
    {
        VramCopy,       // default: copy into the plugin's VRAM right buffers
        LeftBuffer,     // diagnostic: scan out the game's own left buffer shifted (edge shows neighbouring memory)
    };
    void        SetRightSource(RightSource s);
    RightSource GetRightSource();
    const char *RightSourceName(RightSource s);
    int         CurrentDisparity();         // signed rows of the last 3D frame (0 in 2D)
    u32         CurrentMode();              // nngx display mode (0x700 / 0x702), 0 if unknown

    State       GetState();
    const char *StateName(State s);
    std::string DebugText();
}
