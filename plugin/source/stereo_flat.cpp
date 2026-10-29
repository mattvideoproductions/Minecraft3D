// v9.12.0 (1.9.19) stereo.
//
// The 1.9.19 update removed the game's right-eye renderer (the project's design notes (not in this repo)).
// v0.3 has three modes (stereo.hpp):
//
// Comfort (default, v0.3): both eyes rendered in the SAME frame by replaying
//   the frame's top-screen command range for the right eye (comfort.cpp),
//   presented together, with the game paced to 30 fps. Anything unexpected
//   shows that frame 2D (left picture in both eyes), never a broken frame.
//
// TrueDepth ("Experimental: alternate eyes", v0.2): ALTERNATE-EYE rendering.
//   - Every frame the game renders the world once, as usual. A MITM on
//     WorldConstants::refresh 0x4F7B40 (the only place the WVP/PROJ uniforms
//     are computed: P*(V*W) from three matrix stacks) temporarily applies a
//     clip-space shear to the top of the PROJECTION stack for this frame's
//     eye, calls the original, and restores the matrix:
//         row0 += k*row3 ; row0.w -= k*C   (k = +-a/2, a = slider*px/200)
//     => disparity d(D) = slider*px*(1 - C/D): far things behind the screen,
//     things nearer than C blocks in front. Orthographic matrices (HUD, GUI)
//     get a constant (0 in-world = screen depth; pop-out in menus).
//   - present(): after the game's top B2L, the same top RT is copied into
//     this eye's own VRAM buffer (3 per eye, own rotation), and the next
//     frame is set up for the other eye.
//   - cmdlist-done callback: the newest left and right buffers are swapped
//     in together (left address replaced too, right = r2, mode 0x702).
//   - Turn compensation (option): the older eye is shifted by the camera
//     yaw change since it was rendered (scanout row offset, +-16 px of
//     padding, edges filled with the picture's own edge rows).
//   No extra GPU rendering (one 400x240 transfer per frame + 2 edge fills).
//
// Flat (v0.1, fallback): the finished top image copied into a right buffer and
//   scanned out shifted (one depth plane). v0.2: the left eye is ALSO scanned
//   out of a plugin VRAM buffer by default (see "glitch lines" below).
//
// Glitch lines (v0.1.1 on hardware, fixed in v0.2): nngxSwapBuffersByAddress
// derives the LCD framebuffer format's DMA-size bits from the LEFT address
// only (0x7C6D18 -> 0x4C809C: FCRAM -> 0x100, VRAM -> 0x300). The game's left
// buffers are FCRAM, so v0.1's VRAM-B right eye was fetched with FCRAM's
// 8-word (64 B) bursts, from the VRAM bank the GPU hammers (top depth, the
// bottom screen's colour+depth). Both eyes now come from VRAM-B with the
// VRAM setting (0x300), the configuration Terraria's VRAM framebuffers run
// glitch-free on hardware; "Left eye from VRAM" off restores v0.1.1 exactly.
//
// Buffers: allocated ONCE at boot in GxDevice::init (LESSONS #2), VRAM-B only
// (LESSONS VRAM-A note): 3 right + 3 left, 240 x (400 + 2*16) RGB8. If the
// left set does not fit, only the right set is made (v0.1.1 behaviour, flat only).
// With the slider at 0 (or 3D off) nothing is copied, the projection is not
// touched, r1/r2 stay the game's (left, 0) and the mode stays 0x700: stock.
#include <cmath>
#include <cstring>
#include <string>

#include "stereo.hpp"
#include "stereo_flat.hpp"
#include "hooks.hpp"
#include "config.hpp"
#include "memory.hpp"
#include "version_check.hpp"
#include "profiler.hpp"
#include "comfort.hpp"
#include "pica_shadow.hpp"
#include "world_opts.hpp"

#include <3ds.h>
#include <CTRPluginFramework.hpp>

using namespace CTRPluginFramework;

extern "C" void mc3d_stub_after_top_b2l(void);
extern "C" void mc3d_stub_before_top_swap(void);
extern "C" void mc3d_after_top_b2l(u32 idx, u32 dev);
extern "C" u32  mc3d_swap_eyes(u32 left, u32 *leftOut);

namespace mc3d::stereo
{
    namespace
    {
        constexpr int kPadRows   = 16;
        constexpr int kWidth     = 240;               // pixels per row (screen-vertical)
        constexpr int kHeight    = 400;               // rows (screen-horizontal)
        constexpr int kBpp       = 3;                 // RGB8 (0x8051)
        constexpr int kRowBytes  = kWidth * kBpp;     // 720
        constexpr int kBufRows   = kHeight + 2 * kPadRows;
        constexpr u32 kBufBytes  = static_cast<u32>(kWidth * kBufRows * kBpp);   // 311,040
        constexpr int kBuffers   = 3;                 // per eye
        constexpr int kL = 0, kR = 1;

        constexpr u32 kDisplayTop = 0x400, kDisplayBottom = 0x401, kDisplayTopExt = 0x410;
        constexpr u32 kFmtRGB8    = 0x8051;
        constexpr u32 kAreaVramB  = 0x30000;
        constexpr u32 kParamAddr  = 0x600;
        constexpr u32 kOffActiveDisplay = 0x124;      // nngxState: active display slot (0/1/2), 0x7C4D50
        constexpr u32 kVramStart = 0x1F000000, kVramAEnd = 0x1F300000, kVramEnd = 0x1F600000;
        constexpr u32 kVramBUsableEnd = 0x1F5F8000;   // nngx command/scratch area above (0x7C64E4)
        constexpr u32 kVramBMargin    = 0x10000;      // headroom left for the game

        // ---- flat presets (v0.1) ----
        struct PresetDef { const char *name; int px; };
        const PresetDef kPresets[] = {
            { "Subtle (4 px)",   4 },
            { "Normal (8 px)",   8 },
            { "Deep (12 px)",   12 },
            { "Default (14 px)", 14 },   // v0.1.1 default (Matt's hardware pick)
            { "Max (16 px)",    16 },
            { "Pop-out (-6 px)", -6 },
        };
        constexpr int kPresetCount = sizeof(kPresets) / sizeof(kPresets[0]);

        // ---- depth presets (Comfort + alternate eyes; v0.3, Matt's v0.2 feedback) ----
        // px = disparity of infinitely far things at full slider; cap = the
        // most any geometry may come OUT of the screen (crossed disparity, px
        // at full slider). The shear is linear in 1/D, so the cap is met by
        // choosing the zero-parallax distance C from the nearest distance the
        // camera can get to a surface (kDmin, the player's 0.3-block half
        // width): px*(C/kDmin - 1) = cap  =>  C = kDmin*(1 + cap/px).
        // (v0.2.0 audit S2: the uncapped C = 1.5..4 gave -56..-148 px against a wall.)
        // hand = the held item / arm (drawn after the level render), a
        // constant class: negative = slightly in front of the screen
        // (comfort3d_design.md 6.1). "Deep" is the default.
        constexpr float kDmin = 0.3f;
        struct TdPresetDef { const char *name; float px; float cap; float hand; };
        const TdPresetDef kTdPresets[] = {
            { "Deep (18 deep, 4 out, hand -3)",       18.0f, 4.0f, -3.0f },
            { "Immersive (18 deep, 12 out, hand -6)", 18.0f, 12.0f, -6.0f },
            { "Normal (14 deep, 6 out, hand -2)",     14.0f, 6.0f, -2.0f },
            { "Gentle (10 deep, 3 out)",              10.0f, 3.0f, 0.0f },
            { "Window (14 deep, nothing out)",        14.0f, 0.0f, 2.0f },
            { "Zero (test: no depth)",                 0.0f, 0.0f, 0.0f },
        };
        constexpr int kTdPresetCount = sizeof(kTdPresets) / sizeof(kTdPresets[0]);
        constexpr int kTdZero = kTdPresetCount - 1;
        // Constant classes (px at full slider; + = behind the screen)
        constexpr float kPanPx   = 10.0f;   // menus: perspective (title panorama)
        constexpr float kMenuUiPx = -6.0f;  // menus: orthographic UI and text (Matt: "text should pop out")
        constexpr float kHudPx   = 0.0f;    // in-world orthographic HUD: at the screen
        constexpr int   kCompMax = 16;      // turn compensation clamp (rows = the padding)

        using FnActive   = void (*)(u32);
        using FnGen      = void (*)(s32, u32 *);
        using FnBind     = void (*)(u32);
        using FnStorage  = void (*)(u32, s32, s32, u32);
        using FnGetParam = void (*)(u32, s32 *);
        using FnB2L      = s32 (*)(u32, u32, u32, u32, u32, u32, u32, u32, u32);
        using FnSetMode  = void (*)(u32);

        enum class Status : u8 { NotInstalled, WaitInit, Ready, AllocFailed, Fault };

        hook::FunctionHook  h_init;
        hook::FunctionHook  h_proj;
        hook::FunctionHook  h_rt;
        bool s_bottomRT = false;            // main thread: the bottom screen's target is bound
        hook::SubWrapHook   h_b2l;
        hook::SubWrapHook   h_swap;

        volatile Status s_status = Status::NotInstalled;
        u32  s_ids[2][kBuffers] = {};
        u32  s_buf[2][kBuffers] = {};       // [eye][slot]; s_buf[kR] = v0.1 right buffers
        bool s_haveL = false;               // left set allocated (VRAM-B had room)
        u32  s_allocInfo = 0;
        bool s_projOk = false;              // projection hook in and stacks verified

        // options (plugin thread writes, game threads read)
        volatile bool s_enabled = true;
        volatile int  s_preset = 3;          // flat: "Default (14 px)"
        volatile int  s_tdPreset = 0;        // "Deep" (v0.3 default)
        volatile Mode s_mode = Mode::Comfort;
        volatile bool s_turnComp = false;    // v0.3: off (Matt: "I can breathe a little better")
        volatile bool s_lock30 = true;       // Comfort: 30 fps lock
        volatile bool s_threeD = false;      // main thread: last frame wanted 3D
        volatile bool s_comfortOn = false;   // main thread: last frame was a Comfort 3D frame (wanted)
        volatile bool s_lastComfort = false; // last 3D frame was a Comfort (both-eye) frame
        volatile bool s_lastMono = false;    // last frame: Comfort wanted, shown 2D
        pica::Hold    s_hold;                // v0.3.1, main thread: keep the last pair over failed frames
        volatile bool s_leftVram = true;
        volatile bool s_menuPop = true;
        volatile int  s_hudSize = 0;         // v0.3.2: top HUD size 0 Normal (default: the scale also hits menus over the world), 1 Small, 2 Smaller
        u64  s_lastPresentTick = 0;          // v0.3.2, main thread: frame interval + HOME/sleep gap
        prof::Pic s_lastPic = prof::Pic::Off;   // v0.3.2, main thread: last picture kind (event log)
        bool s_lastPicNoSlot = false;        // v0.3.2, main thread: this Comfort frame had no slot free in both eyes
        volatile RightSource s_source = RightSource::VramCopy;

        // Per command list (0/1): what present() decided for the frame recorded
        // into that list; read by the swap callback when the list is done.
        // left = 0: keep the game's left buffer. right = 0: 2D (no record).
        struct Rec { u32 left, right; s8 disp; };
        Rec           s_recs[2] = {};
        volatile u32  s_recValid[2] = {};

        // ---- per-frame projection parameters (main thread only) ----
        // Set at the end of present() for the NEXT frame, read by the
        // WorldConstants::refresh hook while that frame is recorded.
        struct FrameParams
        {
            bool  active;       // shear this frame
            s8    eye;          // eye this frame is rendered for: kL / kR, -1 = none (mono)
            float e;            // -0.5 left, +0.5 right
            float a;            // world: slider*px/200 (NDC)
            float conv;         // world: zero-parallax distance (blocks)
            float kHand, kPan, kUi, kHud;   // constant classes, slider*px/200
            bool  menuFrame;    // no world render seen last frame
        };
        FrameParams s_fp = {};
        FrameParams s_fpRecorded = {};      // copy taken when the frame is presented

        // Camera of the frame being recorded (first in-world refresh).
        struct Cam { bool valid; float r0[3], r2[3]; float p00; };
        Cam s_camFrame = {};
        Cam s_camEye[2] = {};               // camera each eye's newest image was rendered with
        int s_latest[2] = { -1, -1 };       // newest slot per eye (true depth)
        // Last two slots handed to the swap per eye (on screen / swap
        // pending). A new copy goes to the third, whatever the mode, so mode
        // and option changes can never write the buffer being scanned out
        // (v0.2.0 audit N9). -1 = none.
        int s_recent[2][2] = { { -1, -1 }, { -1, -1 } };

        int PickSlot(int eye)
        {
            for (int k = 0; k < kBuffers; k++)
                if (k != s_recent[eye][0] && k != s_recent[eye][1])
                    return k;
            return 0;
        }
        void NoteSlot(int eye, int k)
        {
            s_recent[eye][1] = s_recent[eye][0];
            s_recent[eye][0] = k;
        }
        int s_lastComp = 0;

        // game-thread state / diagnostics
        bool          s_checked = false;
        volatile bool s_weSet3D = false;     // swap thread: we switched to 0x702
        volatile u32  s_faultInfo = 0;
        volatile u32  s_frames3D = 0;        // 3D frames recorded
        volatile u32  s_swaps3D = 0;         // swaps with a right address
        volatile u32  s_lastRight = 0, s_lastLeft = 0;
        volatile int  s_lastDisp = 0;
        volatile bool s_last3D = false;
        volatile bool s_lastDepth = false;   // last 3D frame was true depth

        // Command-list request headroom (v0.1.1, audit S1): nngx never checks
        // the request count, so we require room for our copies plus what the
        // game still adds after this hook (bottom B2L + final split) + margin.
        constexpr u32 kGameReqAfterHook = 2;
        constexpr u32 kReqSafety = 2;
        volatile u32 s_reqSkips = 0;
        volatile u32 s_reqUsed = 0, s_reqMax = 0;

        bool CmdlistReqs(const game::GameVersion *v, u32 &used, u32 &max)
        {
            if (!v->nngxState || !v->offBoundCmdlist || !v->clReqMax || !v->clReqUsed)
                return false;
            const u32 cl = *reinterpret_cast<volatile u32 *>(v->nngxState + v->offBoundCmdlist);
            if (!cl || (cl & 3))
                return false;
            used = *reinterpret_cast<volatile u32 *>(cl + v->clReqUsed);
            max = *reinterpret_cast<volatile u32 *>(cl + v->clReqMax);
            return max != 0 && max <= 0x10000 && used <= max;
        }

        // M0: command-buffer bytes used by the list being recorded.
        void NoteCmdlistBytes(const game::GameVersion *v)
        {
            if (!v->cmdCursor || !v->nngxState || !v->offBoundCmdlist || !v->clBufBase)
                return;
            const u32 cl = *reinterpret_cast<volatile u32 *>(v->nngxState + v->offBoundCmdlist);
            if (!cl || (cl & 3))
                return;
            const u32 base = *reinterpret_cast<volatile u32 *>(cl + v->clBufBase);
            const u32 size = *reinterpret_cast<volatile u32 *>(cl + v->clBufSize);
            const u32 cur = *reinterpret_cast<volatile u32 *>(v->cmdCursor);
            if (cur >= base && cur - base <= size)
                prof::NoteCmdlistBytes(cur - base, size);
        }

        const game::GameVersion *V() { return version::Current(); }

        inline u32 &DevWord(u32 dev, u32 off) { return *reinterpret_cast<u32 *>(dev + off); }
        inline u32 GxDevice()   { return *reinterpret_cast<volatile u32 *>(V()->gxDeviceBase + 4); }
        inline volatile u32 &ModeWord() { return *reinterpret_cast<volatile u32 *>(V()->nngxState + V()->offDisplayMode); }

        bool InVram(u32 addr, u32 len)
        {
            return addr >= kVramStart && addr < kVramEnd && len <= kVramEnd - addr;
        }

        int Round(float x) { return static_cast<int>(x < 0.0f ? x - 0.5f : x + 0.5f); }

        int Disparity(float slider)
        {
            const int px = kPresets[s_preset].px;
            int d = Round(slider * static_cast<float>(px));
            if (d > kPadRows) d = kPadRows;
            if (d < -kPadRows) d = -kPadRows;
            // px > 0 = behind the screen: the right eye's image moves RIGHT,
            // i.e. the scanout starts d rows earlier (Terraria 3D convention,
            // checked on hardware there).
            return -d;
        }

        // ---- boot: eye buffers (game main thread, inside GxDevice::init) ----
        bool AllocSet(const game::GameVersion *v, int eye)
        {
            const auto genBuffers = reinterpret_cast<FnGen>(v->fnGenDisplaybuffers);
            const auto bindBuffer = reinterpret_cast<FnBind>(v->fnBindDisplaybuffer);
            const auto storage    = reinterpret_cast<FnStorage>(v->fnDisplaybufferStorage);
            const auto getParam   = reinterpret_cast<FnGetParam>(v->fnGetDisplaybufferParam);
            genBuffers(kBuffers, s_ids[eye]);
            bool ok = true;
            for (int i = 0; i < kBuffers; i++)
            {
                s32 addr = 0;
                bindBuffer(s_ids[eye][i]);
                storage(kFmtRGB8, kWidth, kBufRows, kAreaVramB);
                getParam(kParamAddr, &addr);
                s_buf[eye][i] = static_cast<u32>(addr);
                if (s_buf[eye][i] < kVramAEnd || !InVram(s_buf[eye][i], kBufBytes) ||
                    s_buf[eye][i] + kBufBytes > kVramBUsableEnd || (s_buf[eye][i] & 0xF))
                    ok = false;
            }
            if (ok)
            {
                // Black padding / initial content. VRAM is mapped in the game
                // process; flush so nothing stays in the CPU cache (LESSONS #8).
                for (int i = 0; i < kBuffers; i++)
                {
                    std::memset(reinterpret_cast<void *>(s_buf[eye][i]), 0, kBufBytes);
                    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, s_buf[eye][i], kBufBytes);
                }
            }
            return ok;
        }

        u32 VramBFree(const game::GameVersion *v, u32 dev)
        {
            const u32 head = (DevWord(dev, v->devVramBNext) + 0xFF) & ~0xFFu;
            if (head < kVramAEnd || head > kVramBUsableEnd)
                return 0;
            return kVramBUsableEnd - head;
        }

        void AllocateBuffers(u32 dev)
        {
            const game::GameVersion *v = V();
            if (s_status != Status::WaitInit)
                return;
            if (dev == 0 || dev != GxDevice())
            {
                s_allocInfo = 0xD0000000u | (dev & 0xFFFFFFu);
                s_status = Status::AllocFailed;
                return;
            }
            const auto activeDisplay = reinterpret_cast<FnActive>(v->fnActiveDisplay);
            const u32 prevSlot = *reinterpret_cast<volatile u32 *>(v->nngxState + kOffActiveDisplay);

            // VRAM-B only (root cause of the v0.1.0 boot hang, notes/bisect_v0.1.0.md).
            // Budget: right set first (v0.1 behaviour), then the left set only
            // if it still leaves the margin. Measured in Azahar: the game never
            // allocates VRAM-B after GxDevice::init (head 0x1F4D7900 in-world
            // with the right set; 6 buffers leave ~250 KiB).
            if (VramBFree(v, dev) < kBuffers * kBufBytes + kVramBMargin)
            {
                s_allocInfo = 0xB0000000u | (DevWord(dev, v->devVramBNext) >> 8 & 0xFFFFFF);
                s_status = Status::AllocFailed;
                return;
            }
            activeDisplay(kDisplayTopExt);
            bool ok = AllocSet(v, kR);
            if (ok && VramBFree(v, dev) >= kBuffers * kBufBytes + kVramBMargin)
                s_haveL = AllocSet(v, kL);
            activeDisplay(prevSlot == 0 ? kDisplayTop : prevSlot == 2 ? kDisplayTopExt : kDisplayBottom);
            if (!ok)
            {
                s_allocInfo = 0xA0000000u | (s_buf[kR][0] >> 8 & 0xFFFFFF);
                s_status = Status::AllocFailed;
                return;
            }
            s_status = Status::Ready;
        }

        u64 Hook_GxInit(MC3D_HOOK_ARGS)
        {
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            AllocateBuffers(a0);
            return r;
        }

        // ---- projection shear (WorldConstants::refresh MITM, render thread) ----
        float *StackTop(u32 stackGlobal)
        {
            const u32 st = *reinterpret_cast<volatile u32 *>(stackGlobal);
            if (!st || (st & 3))
                return nullptr;
            const u32 begin = *reinterpret_cast<volatile u32 *>(st);
            const u32 end = *reinterpret_cast<volatile u32 *>(st + 4);
            if (!begin || end < begin + 0x44 || (end & 3))
                return nullptr;
            return reinterpret_cast<float *>(end - 0x44);
        }

        inline bool Near(float x, float y) { return std::fabs(x - y) < 1e-4f; }

        volatile bool s_recordProj = false;  // main thread: Comfort wants the world PROJ matrices

        u64 RefreshBody(MC3D_HOOK_ARGS)
        {
            const FrameParams &p = s_fp;
            if (!p.active)
            {
                // Comfort 3D: no shear here (the command stream is patched
                // per eye); only remember which PROJ matrices the world used,
                // to recognise renderchunk PROJ uploads (c8-c11) exactly.
                if (s_recordProj && !s_bottomRT && prof::InWorld())
                {
                    const float *m = StackTop(V()->projStack);
                    if (m && Near(m[3], 0.0f) && Near(m[7], 0.0f) && Near(m[11], -1.0f) && Near(m[15], 0.0f))
                        comfort::OnProjRefresh(m);
                }
                return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            }
            const game::GameVersion *v = V();
            float *m = StackTop(v->projStack);     // column-major: m[col*4 + row]
            if (!m)
                return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);

            // Top screen only (v0.2.0 audit S1): the bottom screen is recorded
            // in the same frame and shown 2D, so a shear there would make it
            // jump sideways every frame. GxDevice::setRenderTarget(2) marks
            // the bottom stage; present() clears the flag for the next frame.
            // (An aspect test |m5/m0| = 400/240 was tried first: the title
            // panorama's projection is not 400/240, so it lost its depth.)
            if (s_bottomRT)
            {
                prof::NoteProj(3);
                return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            }
            // Row 3 = (m[3], m[7], m[11], m[15])
            const bool persp = Near(m[3], 0.0f) && Near(m[7], 0.0f) && Near(m[11], -1.0f) && Near(m[15], 0.0f);
            const bool ortho = Near(m[3], 0.0f) && Near(m[7], 0.0f) && Near(m[11], 0.0f) && Near(m[15], 1.0f);
            float k = 0.0f, kc = 0.0f;              // x' = x + k*w - kc
            if (persp)
            {
                const bool world = !prof::WorldHookInstalled() || prof::InWorld();
                // v0.3: the held item / arm comes after the level render
                // (`bl 0x221BB8` returned) and gets the constant hand class.
                if (world && comfort::AfterLevelRender())
                {
                    k = p.e * p.kHand;
                    prof::NoteProj(1);
                }
                else if (world)
                {
                    k = p.e * p.a;
                    kc = k * p.conv;
                    if (!s_camFrame.valid)
                    {
                        const float *vw = StackTop(v->viewStack);
                        if (vw)
                        {
                            s_camFrame.r0[0] = vw[0]; s_camFrame.r0[1] = vw[4]; s_camFrame.r0[2] = vw[8];
                            s_camFrame.r2[0] = vw[2]; s_camFrame.r2[1] = vw[6]; s_camFrame.r2[2] = vw[10];
                            s_camFrame.p00 = m[0];
                            s_camFrame.valid = true;
                        }
                    }
                    prof::NoteProj(0);
                    prof::NoteWorldKind(m[0], m[10], m[14]);
                }
                else
                {
                    k = p.e * (p.menuFrame ? p.kPan : p.kHand);
                    prof::NoteProj(1);
                }
            }
            else if (ortho)
            {
                k = p.e * (p.menuFrame ? p.kUi : p.kHud);
                prof::NoteProj(2);
            }
            if (k == 0.0f && kc == 0.0f)
                return hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);

            // Shear row 0 (logical x) by row 3 (w). Never touch y (the 90
            // degree screen rotation happens in the shaders, design 2.1).
            const float s0 = m[0], s4 = m[4], s8 = m[8], s12 = m[12];
            m[0] = s0 + k * m[3];
            m[4] = s4 + k * m[7];
            m[8] = s8 + k * m[11];
            m[12] = s12 + k * m[15] - kc;
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            m[0] = s0; m[4] = s4; m[8] = s8; m[12] = s12;
            return r;
        }

        // v0.3.2 HUD size. The top-screen GUI (hotbar, hearts, chat...) is
        // drawn with an orthographic PROJ after the world render. For those
        // refreshes only - top screen (not the bottom stage, so touch
        // targets and the bottom UI are untouched), outside the world
        // render, in a frame that drew the world (no title / loading /
        // world-less menu screens) - the matrix is scaled about the screen
        // centre for the call and restored. The centre stays put (the
        // crosshair keeps aiming at the same block); edge-anchored parts move
        // inward by (1 - s) of their distance from the centre. Row 3 stays
        // (0,0,0,1), so the Comfort replay still classes these as HUD
        // (screen depth). Factor: pica::HudScale (the 1.9.19 top HUD is
        // already 1 screen pixel per GUI pixel, so Small = 80 %).
        u64 Hook_WorldConstants(MC3D_HOOK_ARGS)
        {
            float *m = nullptr;
            float s = 1.0f;
            if (s_hudSize > 0 && !s_bottomRT && prof::WorldHookInstalled() && !prof::InWorld() &&
                prof::WorldSeenThisFrame())
            {
                m = StackTop(V()->projStack);
                // Ortho, and shaped like the 400x240 top screen (|m5/m0| =
                // 400/240): an ortho for an offscreen texture (map, icons)
                // or anything else keeps its own matrix.
                if (m && Near(m[3], 0.0f) && Near(m[7], 0.0f) && Near(m[11], 0.0f) && Near(m[15], 1.0f) &&
                    std::fabs(m[0]) > 1e-6f && std::fabs(std::fabs(m[5] / m[0]) - 400.0f / 240.0f) < 0.05f)
                    s = pica::HudScale(m[0], s_hudSize);
            }
            if (s >= 1.0f)
                return RefreshBody(MC3D_HOOK_PASS);
            float keep[8];
            const int rows[8] = { 0, 4, 8, 12, 1, 5, 9, 13 };      // rows 0 (x) and 1 (y), all columns
            for (int i = 0; i < 8; i++)
            {
                keep[i] = m[rows[i]];
                m[rows[i]] *= s;
            }
            const u64 r = RefreshBody(MC3D_HOOK_PASS);
            for (int i = 0; i < 8; i++)
                m[rows[i]] = keep[i];
            return r;
        }

        u64 Hook_SetRenderTarget(MC3D_HOOK_ARGS)
        {
            // pH mark (Comfort): the cursor BEFORE the bottom stage emits
            // anything. Only reads memory; nothing hooked is called.
            comfort::OnSetRenderTarget(a1);
            const u64 r = hook::FunctionHook::CallOriginal(MC3D_HOOK_PASS);
            s_bottomRT = a1 == 2;
            return r;
        }

        // Image shift (logical px, + = right) that re-aligns an image rendered
        // with camera `old` to the camera `now`: where the old view centre
        // lands in the new view. Exact for yaw/pitch at the centre (we use the
        // horizontal part only).
        int CompPx(const Cam &old, const Cam &now)
        {
            if (!old.valid || !now.valid)
                return 0;
            // old forward in world = -(view row 2)
            const float fx = -old.r2[0], fy = -old.r2[1], fz = -old.r2[2];
            const float vx = now.r0[0] * fx + now.r0[1] * fy + now.r0[2] * fz;
            const float vz = now.r2[0] * fx + now.r2[1] * fy + now.r2[2] * fz;
            if (!(-vz > 0.2f))
                return 0;                           // turned more than ~78 degrees: give up
            const float px = 200.0f * now.p00 * vx / -vz;
            int r = Round(px);
            if (r > kCompMax) r = kCompMax;
            if (r < -kCompMax) r = -kCompMax;
            return r;
        }

        // First 3D frame: the state we are about to drive must look like the RE says.
        bool SanityCheck(u32 dev)
        {
            const game::GameVersion *v = V();
            if (DevWord(dev, v->devTopWidth) != static_cast<u32>(kWidth) ||
                DevWord(dev, v->devTopHeight) != static_cast<u32>(kHeight))
            {
                s_faultInfo = 0x10000000u | (DevWord(dev, v->devTopWidth) << 12) | DevWord(dev, v->devTopHeight);
                return false;
            }
            for (int i = 0; i < kBuffers; i++)
                if (DevWord(dev, v->devTopDispAddr + 4 * i) == 0)
                {
                    s_faultInfo = 0x20000000u | i;
                    return false;
                }
            if (ModeWord() != game::kModeNormal)
            {
                s_faultInfo = 0x30000000u | ModeWord();
                return false;
            }
            if (DevWord(dev, v->devTopColorRes) == 0)
            {
                s_faultInfo = 0x40000000u;
                return false;
            }
            return true;
        }

        // Copies of the top RT into one eye buffer. `fill`: also refresh both
        // padding bands with the picture's own edge rows (true depth, for the
        // turn-compensation shift): rows 0..15 = image rows 0..15, rows
        // 416..431 = image rows 384..399. Same arguments as the game's own
        // copy at 0x4F8918 (fmt 1 = RGB8, no AA, no flip); GPU-only.
        void CopyEye(u32 dev, u32 dst, bool fill)
        {
            const game::GameVersion *v = V();
            const auto b2l = reinterpret_cast<FnB2L>(v->fnCopyB2L);
            const u32 res = DevWord(dev, v->devTopColorRes);
            const u32 w = DevWord(dev, v->devTopWidth), h = DevWord(dev, v->devTopHeight);
            // Fills first (full 400-row copies; the main copy then overwrites
            // rows 16..415). A 16-row strip version (srcY/h of gd B2L, audit
            // N10) could not be verified: Azahar keeps GPU output in host
            // textures, so guest-memory reads of the buffers are stale, and a
            // wrong strip would only show during turns. Kept the full copies
            // that were checked visually (v020_e_turn_comp.png).
            if (fill)
            {
                b2l(res, 0, 0, dst + 2 * kPadRows * kRowBytes, w, h, 1, 0, 0);
                b2l(res, 0, 0, dst, w, h, 1, 0, 0);
            }
            b2l(res, 0, 0, dst + kPadRows * kRowBytes, w, h, 1, 0, 0);
        }

        void SetNextFrame(bool depth, float slider)
        {
            FrameParams p = {};
            if (depth)
            {
                const TdPresetDef &d = kTdPresets[s_tdPreset];
                const bool zero = s_tdPreset == kTdZero;
                const int prevEye = s_fp.active ? s_fp.eye : kR;
                p.active = true;
                p.eye = static_cast<s8>(prevEye == kL ? kR : kL);
                p.e = p.eye == kL ? -0.5f : 0.5f;
                p.a = slider * d.px / 200.0f;
                p.conv = d.px > 0.0f ? kDmin * (1.0f + d.cap / d.px) : 1.0f;
                const float u = zero ? 0.0f : slider / 200.0f;
                p.kHand = u * d.hand;
                p.kHud = u * kHudPx;
                p.kPan = s_menuPop ? u * kPanPx : 0.0f;
                p.kUi = s_menuPop ? u * kMenuUiPx : 0.0f;
                p.menuFrame = prof::WorldHookInstalled() && !prof::TakeWorldSeen();
            }
            else
                prof::TakeWorldSeen();
            s_fp = p;
            s_camFrame.valid = false;
        }
    }

    // ---- frame: game main thread, inside present(), after the left-eye B2L ----
    namespace flat
    {
        // Comfort 3D parameters for this frame (full disparity in NDC).
        comfort::Params ComfortParams(float slider)
        {
            const TdPresetDef &d = kTdPresets[s_tdPreset];
            const bool zero = s_tdPreset == kTdZero;
            comfort::Params p = {};
            const float u = zero ? 0.0f : slider / 200.0f;
            p.a = slider * d.px / 200.0f;
            p.conv = d.px > 0.0f ? kDmin * (1.0f + d.cap / d.px) : 1.0f;
            p.hand = u * d.hand;
            p.hud = u * kHudPx;
            p.pan = s_menuPop ? u * kPanPx : 0.0f;
            p.ui = s_menuPop ? u * kMenuUiPx : 0.0f;
            return p;
        }

        // One slot that neither eye has on screen or pending (3 per eye,
        // each eye's last two are excluded).
        int PickBothSlot()
        {
            for (int j = 0; j < kBuffers; j++)
                if (j != s_recent[kR][0] && j != s_recent[kR][1] && j != s_recent[kL][0] && j != s_recent[kL][1])
                    return j;
            return -1;
        }

        void AfterTopB2L(u32 idx, u32 dev)
        {
            const game::GameVersion *v = V();
            const u32 list = DevWord(dev, v->devCurList) & 1;
            // v0.3.2: frame interval (event log) and the HOME / sleep gap:
            // after more than 200 ms without a frame, the last stereo pair is
            // stale and must not be held (audit v0.3.1 N3).
            {
                const u64 nowT = svcGetSystemTick();
                const u64 d = s_lastPresentTick ? nowT - s_lastPresentTick : 0;
                s_lastPresentTick = nowT;
                const u64 d10 = d / (prof::kTicksPerSec / 10000);      // 0.1 ms
                const u32 gap10 = d10 > 0xFFFFFFFFull ? 0xFFFFFFFFu : static_cast<u32>(d10);
                prof::NoteFrameInterval(gap10);
                pica::HoldGap(s_hold, gap10 / 10);
            }
            NoteCmdlistBytes(v);
            prof::NoteVramBHead(DevWord(dev, v->devVramBNext));
            s_bottomRT = false;             // next frame starts with the top screen

            // What the frame just recorded was rendered as.
            s_fpRecorded = s_fp;
            const Cam cam = s_camFrame;

            Rec rec = {};
            bool have = false, depthFrame = false, comfortFrame = false, monoFrame = false, heldFrame = false;
            const float slider = stereo_get_slider();
            const RightSource src = s_source;
            const bool haveTarget = src == RightSource::LeftBuffer || s_status == Status::Ready;
            if (s_enabled && slider > 0.0f && haveTarget && s_status != Status::Fault &&
                idx < static_cast<u32>(kBuffers) && dev == GxDevice())
            {
                if (!s_checked)
                {
                    s_checked = true;
                    if (!SanityCheck(dev))
                        s_status = Status::Fault;
                }
            }
            const bool go = s_enabled && slider > 0.0f && haveTarget && s_status != Status::Fault &&
                            idx < static_cast<u32>(kBuffers) && dev == GxDevice();
            s_threeD = go;
            // Comfort: both eye sets in VRAM, the replay hooks in, no diagnostic.
            // D-S1: the governor suspends Comfort (flat plane, unpaced) for a
            // cool-down when too many frames missed their 2-vblank slot.
            // v0.3.1: opt-in only (gov=on); with it off, nothing here changes
            // per frame, so Comfort never turns into the flat plane by itself.
            const bool wantComfort = go && s_mode == Mode::Comfort && s_haveL && src == RightSource::VramCopy &&
                                     s_status == Status::Ready && comfort::Ready() && comfort::GovernorAllows();
            const bool wantDepth = go && s_mode == Mode::TrueDepth && s_projOk && s_haveL &&
                                   src == RightSource::VramCopy && s_status == Status::Ready;
            // 30 fps lock only while Comfort 3D is active (slider 0 = stock, unpaced).
            comfort::SetPacer(wantComfort && s_lock30);
            s_comfortOn = wantComfort;
            s_recordProj = wantComfort;

            if (wantComfort)
            {
                // Both eyes need a slot neither has on screen. After a mode
                // change there may be none for a frame or two: then this
                // frame is shown 2D from a free LEFT slot (the right eye
                // shows that buffer too, so its own slots age out).
                int k = PickBothSlot();
                const bool canBoth = k >= 0;
                s_lastPicNoSlot = !canBoth;
                if (!canBoth)
                    k = PickSlot(kL);
                comfort::Result r = comfort::Result::None;
                if (canBoth)
                    r = comfort::Frame(dev, true, s_buf[kL][k], s_buf[kR][k], ComfortParams(slider));
                else
                    comfort::Frame(dev, false, 0, 0, ComfortParams(slider));   // shadow restarts next frame
                if (r == comfort::Result::Both)
                {
                    rec.left = s_buf[kL][k] + kPadRows * kRowBytes;
                    rec.right = s_buf[kR][k] + kPadRows * kRowBytes;
                    NoteSlot(kL, k);
                    NoteSlot(kR, k);
                    have = comfortFrame = true;
                    pica::HoldStep(s_hold, true, true);
                }
                else if (pica::HoldStep(s_hold, false, s_recent[kL][0] >= 0 && s_recent[kL][0] == s_recent[kR][0]) ==
                         pica::Show::Hold)
                {
                    // v0.3.1: this frame's replay failed: keep the previous
                    // stereo pair on screen (one repeated picture, depth
                    // kept) instead of dropping to 2D for a frame, for at
                    // most s_hold.maxRun frames in a row. The pair is the
                    // newest slot of BOTH eyes; the rotation is NOT advanced,
                    // so the pair and the slot before it stay excluded from
                    // the next copies, exactly as when it was first shown.
                    // (Result::LeftOnly wrote a PickBothSlot slot, never these.)
                    const int hk = s_recent[kL][0];
                    rec.left = s_buf[kL][hk] + kPadRows * kRowBytes;
                    rec.right = s_buf[kR][hk] + kPadRows * kRowBytes;
                    have = heldFrame = true;
                    comfort::NoteShown(true);
                }
                else
                {
                    comfort::NoteShown(false);
                    // Fallback: this frame 2D, but still from VRAM in mode
                    // 0x702 (no display-mode flip for one frame): the left
                    // picture in both eyes. LeftOnly = comfort already
                    // copied it; None = the top target is intact, copy it now
                    // if the request queue has room, else true 2D.
                    bool copied = r == comfort::Result::LeftOnly;
                    if (!copied)
                    {
                        u32 used = 0, max = 0;
                        if (CmdlistReqs(v, used, max) && max - used >= 1 + kGameReqAfterHook + kReqSafety)
                        {
                            const auto b2l = reinterpret_cast<FnB2L>(v->fnCopyB2L);
                            b2l(DevWord(dev, v->devTopColorRes), 0, 0, s_buf[kL][k] + kPadRows * kRowBytes,
                                DevWord(dev, v->devTopWidth), DevWord(dev, v->devTopHeight), 1, 0, 0);
                            copied = true;
                        }
                        else
                        {
                            s_reqSkips = s_reqSkips + 1;
                            prof::NoteReqSkip();
                        }
                    }
                    if (copied)
                    {
                        rec.left = rec.right = s_buf[kL][k] + kPadRows * kRowBytes;
                        NoteSlot(kL, k);
                        have = true;
                    }
                    else
                        NoteSlot(kL, -1);       // left eye: the game's own buffer
                    NoteSlot(kR, -1);           // right eye: no right-set buffer on screen
                    monoFrame = true;
                }
                comfort::NoteListKind(list, comfortFrame);
                if (have)
                {
                    s_frames3D = s_frames3D + 1;
                    s_lastDisp = 0;
                }
            }
            else
            {
                comfort::Frame(dev, false, 0, 0, comfort::Params{});    // shadow off
                comfort::NoteListKind(list, false);
                pica::HoldReset(s_hold);    // v0.3.1: a non-Comfort frame makes the last pair stale
            }

            if (go && !wantComfort)
            {
                const bool vramL = s_haveL && s_leftVram && src == RightSource::VramCopy;
                const bool comp = s_turnComp;
                const int disp = wantDepth ? 0 : Disparity(slider);
                u32 need;
                if (wantDepth)
                    need = s_fpRecorded.active && s_fpRecorded.eye >= 0 ? (comp ? 3u : 1u) : (comp ? 6u : 2u);
                else
                    need = (disp != 0 ? 2u : 1u) + (vramL ? 1u : 0u);
                need += kGameReqAfterHook + kReqSafety;
                u32 used = 0, max = 0;
                const bool room = src != RightSource::VramCopy ||
                                  (CmdlistReqs(v, used, max) && max - used >= need);
                if (src == RightSource::VramCopy)
                {
                    s_reqUsed = used;
                    s_reqMax = max;
                }
                if (!room)
                {
                    s_reqSkips = s_reqSkips + 1;
                    prof::NoteReqSkip();
                }
                else if (wantDepth)
                {
                    const int eye = s_fpRecorded.active ? s_fpRecorded.eye : -1;
                    if (eye < 0)
                    {
                        // Frame rendered without a shear (first 3D frame): both eyes.
                        for (int e = 0; e < 2; e++)
                        {
                            const int k = PickSlot(e);
                            CopyEye(dev, s_buf[e][k], comp);
                            s_latest[e] = k;
                            NoteSlot(e, k);
                            s_camEye[e] = cam;
                        }
                    }
                    else
                    {
                        // Own rotation per eye: slot k was last written 3 frames
                        // of this eye ago; the eye shows k-1 (or k-2 while a
                        // swap is pending), never k.
                        const int k = PickSlot(eye);
                        CopyEye(dev, s_buf[eye][k], comp);
                        s_latest[eye] = k;
                        NoteSlot(eye, k);
                        s_camEye[eye] = cam;
                    }
                    if (s_latest[kL] >= 0 && s_latest[kR] >= 0)
                    {
                        // Older eye re-aligned to the newest camera (rows = -px).
                        int offL = 0, offR = 0;
                        if (comp && eye >= 0)
                        {
                            const int other = eye == kL ? kR : kL;
                            const int px = CompPx(s_camEye[other], cam);
                            (other == kL ? offL : offR) = -px;
                            s_lastComp = px;
                        }
                        rec.left = s_buf[kL][s_latest[kL]] + (kPadRows + offL) * kRowBytes;
                        rec.right = s_buf[kR][s_latest[kR]] + (kPadRows + offR) * kRowBytes;
                        rec.disp = 0;
                        have = depthFrame = true;
                        prof::NoteEye(eye, s_lastComp);
                    }
                }
                else if (src == RightSource::VramCopy)
                {
                    // Flat (v0.1) plane.
                    const auto b2l = reinterpret_cast<FnB2L>(v->fnCopyB2L);
                    const u32 res = DevWord(dev, v->devTopColorRes);
                    const u32 w = DevWord(dev, v->devTopWidth), h = DevWord(dev, v->devTopHeight);
                    // v0.2: with the left eye in VRAM both eyes use our own
                    // rotation; otherwise (v0.1.1) the right buffer follows
                    // the game's display index and the left stays the game's.
                    int k = static_cast<int>(idx);
                    if (vramL)
                    {
                        // one slot free in BOTH eyes' recent lists (3 slots, 2+2 recent)
                        k = PickSlot(kR);
                        if (k == s_recent[kL][0] || k == s_recent[kL][1])
                            for (int j = 0; j < kBuffers; j++)
                                if (j != s_recent[kR][0] && j != s_recent[kR][1] &&
                                    j != s_recent[kL][0] && j != s_recent[kL][1])
                                    k = j;
                    }
                    const u32 dstR = s_buf[kR][k];
                    // Edge fill (v0.1.1): first copy the picture straight into
                    // the scan-out window (rows pad+disp ..); the main copy then
                    // overwrites rows pad..pad+399, so the |disp| exposed rows
                    // keep the game's own edge pixels at zero disparity.
                    if (disp != 0)
                        b2l(res, 0, 0, dstR + (kPadRows + disp) * kRowBytes, w, h, 1, 0, 0);
                    b2l(res, 0, 0, dstR + kPadRows * kRowBytes, w, h, 1, 0, 0);
                    if (vramL)
                    {
                        b2l(res, 0, 0, s_buf[kL][k] + kPadRows * kRowBytes, w, h, 1, 0, 0);
                        rec.left = s_buf[kL][k] + kPadRows * kRowBytes;
                        NoteSlot(kL, k);
                    }
                    NoteSlot(kR, k);
                    rec.right = dstR + (kPadRows + disp) * kRowBytes;
                    rec.disp = static_cast<s8>(disp);
                    have = true;
                }
                else
                {
                    // Diagnostic: right eye = the game's own left buffer (FCRAM), no shift.
                    rec.right = 1;      // marker: resolved in the swap callback
                    rec.disp = 0;
                    have = true;
                }
                if (have)
                {
                    s_frames3D = s_frames3D + 1;
                    s_lastDisp = rec.disp;
                }
            }
            // v0.3.2: one event per change of what the top screen shows
            // (main thread: ring push only, written by the plugin thread).
            {
                prof::Pic pic;
                u8 why = prof::kWhyNone;
                if (!go)
                {
                    pic = prof::Pic::Off;
                    why = !s_enabled ? prof::kWhyOff : slider <= 0.0f ? prof::kWhySlider0 : prof::kWhyUnavailable;
                }
                else if (wantComfort)
                {
                    pic = comfortFrame ? prof::Pic::Full3D : heldFrame ? prof::Pic::Held : prof::Pic::Mono2D;
                    if (!comfortFrame)
                        why = s_lastPicNoSlot ? static_cast<u8>(prof::kWhyNoSlot) : static_cast<u8>(comfort::LastReason());
                }
                else
                {
                    pic = !have ? prof::Pic::Mono2D : wantDepth ? prof::Pic::Alternate : prof::Pic::FlatPlane;
                    why = !have ? prof::kWhyNoRoom
                        : s_mode == Mode::Flat ? prof::kWhyModeFlat
                        : s_mode == Mode::TrueDepth ? prof::kWhyModeAlt
                        : !comfort::GovernorAllows() ? prof::kWhyGovernor : prof::kWhyNotReady;
                }
                if (pic != s_lastPic)
                {
                    prof::PushEvent(prof::EvKind::Switch, static_cast<u8>(s_lastPic), static_cast<u8>(pic), why);
                    s_lastPic = pic;
                }
                s_lastPicNoSlot = false;
            }
            comfort::EndFrame();            // this frame's marks are consumed
            s_last3D = have;
            s_lastDepth = have && depthFrame;
            s_lastComfort = have && (comfortFrame || heldFrame);
            s_lastMono = monoFrame;
            // Next frame's eye (true depth keeps alternating while active).
            SetNextFrame(wantDepth && have, slider);
            if (!wantDepth)
                s_latest[kL] = s_latest[kR] = -1;

            // The list is run later in this present() (0x4F8A30), so the swap
            // callback that reads this can only fire after the store.
            // Invalidate first, then write, then publish (audit N2: never a
            // half-written record, even if a callback ran very late).
            __atomic_store_n(&s_recValid[list], 0u, __ATOMIC_RELEASE);
            if (have)
            {
                s_recs[list] = rec;
                __atomic_store_n(&s_recValid[list], 1u, __ATOMIC_RELEASE);
            }
        }

        // ---- swap: nngx command-list-done callback thread ----
        // Returns the right-eye address (0 = 2D) and may replace the left one.
        u32 SwapEyes(u32 left, u32 *leftOut)
        {
            const game::GameVersion *v = V();
            const u32 dev = GxDevice();
            u32 right = 0;
            if (dev)
            {
                const u32 list = DevWord(dev, v->devRunList) & 1;
                const u32 idx = DevWord(dev, v->devSwapIdx);
                prof::OnCmdlistDone(list);      // GPU time (RunCmdlist tick -> here)
                if (__atomic_load_n(&s_recValid[list], __ATOMIC_ACQUIRE) && idx < static_cast<u32>(kBuffers) &&
                    left == DevWord(dev, v->devTopDispAddr + 4 * idx))
                {
                    const Rec r = s_recs[list];
                    if (r.right == 1)
                        right = left;           // diagnostic: same FCRAM buffer, no shift
                    else if (s_status == Status::Ready && r.right)
                    {
                        right = r.right;
                        if (r.left)
                        {
                            *leftOut = r.left;  // both eyes from VRAM: DMA bits 0x300
                            s_lastLeft = r.left;
                        }
                    }
                }
            }

            // v0.3 presentation stats: which picture each eye gets, per vblank.
            comfort::OnSwap(*leftOut, right);     // *leftOut = final left (game's or ours)

            const auto setMode = reinterpret_cast<FnSetMode>(v->fnSetDisplayMode);
            if (right)
            {
                if (ModeWord() != game::kModeStereo)
                    setMode(game::kModeStereo);
                s_weSet3D = true;
                s_lastRight = right;
                s_swaps3D = s_swaps3D + 1;
            }
            else if (s_weSet3D)
            {
                // Back to exactly stock: mode 0x700, right address 0, game's left.
                setMode(game::kModeNormal);
                s_weSet3D = false;
            }
            return right;
        }

        void Install()
        {
            const game::GameVersion *v = V();
            if (!v || v->stereoKind != game::StereoKind::FlatParallax)
                return;
            // Start-up options from mc3d_cfg.txt (all changeable in the menu).
            int o;
            // mode: 0 flat, 1 alternate eyes (v0.2 "depth"), 2 comfort (v0.3 default)
            if ((o = cfg::StereoOpt("mode")) >= 0) s_mode = o == 0 ? Mode::Flat : o == 1 ? Mode::TrueDepth : Mode::Comfort;
            if ((o = cfg::StereoOpt("lock")) >= 0) s_lock30 = o != 0;
            // v0.3.1: governor off by default; gov=on opts in (gov=off still accepted)
            if ((o = cfg::StereoOpt("gov")) >= 0) comfort::SetGovernor(o != 0);
            // v0.3.1: frames a failed replay may keep the previous pair (0 = v0.3.0: 2D at once)
            if ((o = cfg::StereoOpt("hold")) >= 0 && o <= 30) s_hold.maxRun = static_cast<u32>(o);
            if ((o = cfg::StereoOpt("tdepth")) >= 0 && o < kTdPresetCount) s_tdPreset = o;
            if ((o = cfg::StereoOpt("flatdepth")) >= 0 && o < kPresetCount) s_preset = o;
            if ((o = cfg::StereoOpt("comp")) >= 0) s_turnComp = o != 0;
            if ((o = cfg::StereoOpt("leftvram")) >= 0) s_leftVram = o != 0;
            if ((o = cfg::StereoOpt("menupop")) >= 0) s_menuPop = o != 0;
            if ((o = cfg::StereoOpt("hud")) >= 0 && o <= 2) s_hudSize = o;     // v0.3.2: 0 normal, 1 small, 2 smaller

            // Allocation hook first: without it there are no eye buffers
            // and the stereo stays 2D (the left-buffer diagnostic still works).
            if (cfg::Hook(cfg::kInit))
                h_init.Install(v->fnGxInit, Hook_GxInit, hook::ExpectedFirstWord(v->fnGxInit));
            s_status = h_init.Installed() ? Status::WaitInit : Status::AllocFailed;
            // Swap first, copy only if the swap hook is in: without the swap
            // nothing would ever show the copy (it would be wasted GPU time).
            // The swap alone is a pass-through (no record -> r1/r2 untouched).
            if (cfg::Hook(cfg::kSwap))
                h_swap.Install(v->siteTopSwap, v->insnTopSwap, mc3d_stub_before_top_swap, nullptr);
            if (cfg::Hook(cfg::kB2L) && h_swap.Installed())
                h_b2l.Install(v->siteTopB2L, v->insnTopB2L, nullptr, mc3d_stub_after_top_b2l);
            // True depth: the projection hook (only with the copy path in, and
            // only if the literal pool still names the stacks we read).
            if (cfg::Hook(cfg::kProj) && h_b2l.Installed() && v->fnWorldConstants && v->projStack &&
                v->viewStack && v->litProjStack &&
                *reinterpret_cast<const u32 *>(v->litProjStack) == v->projStack &&
                *reinterpret_cast<const u32 *>(v->litProjStack + 4) == v->viewStack)
            {
                // The bottom-screen gate is required: without it the bottom
                // screen would jitter, so no gate = no true depth.
                if (v->fnSetRenderTarget)
                    h_rt.Install(v->fnSetRenderTarget, Hook_SetRenderTarget,
                                 hook::ExpectedFirstWord(v->fnSetRenderTarget));
                if (h_rt.Installed())
                    h_proj.Install(v->fnWorldConstants, Hook_WorldConstants,
                                   hook::ExpectedFirstWord(v->fnWorldConstants));
                s_projOk = h_proj.Installed();
            }
            // Comfort 3D needs: the top-B2L copy path, setRenderTarget (pH
            // mark + bottom gate), the projection hook (PROJ matrices), the
            // world render hook (profiler) and its own clear/level hooks.
            comfort::SetHooksPresent(false, h_rt.Installed() && h_b2l.Installed(), prof::WorldHookInstalled(),
                                     h_proj.Installed());
        }

        std::string HookStatus()
        {
            return Utils::Format("gxInit %08lX %s\ntopB2L %08lX %s\ntopSwap %08lX %s\nproj %08lX %s rt %s\n",
                                 h_init.Target(), hook::ResultString(h_init.LastResult()),
                                 h_b2l.Target(), hook::ResultString(h_b2l.LastResult()),
                                 h_swap.Target(), hook::ResultString(h_swap.LastResult()),
                                 h_proj.Target(), hook::ResultString(h_proj.LastResult()),
                                 hook::ResultString(h_rt.LastResult()));
        }

        void SetEnabled(bool on) { s_enabled = on; }

        State GetState()
        {
            if (!h_b2l.Installed() || !h_swap.Installed())
                return State::Unavailable;
            switch (s_status)
            {
                case Status::NotInstalled: return State::Unavailable;
                case Status::Fault:        return State::Fault;
                case Status::WaitInit:
                    if (s_source != RightSource::LeftBuffer) return State::WaitInit;
                    break;
                case Status::AllocFailed:
                    if (s_source != RightSource::LeftBuffer) return State::NoBuffers;
                    break;
                case Status::Ready:        break;
            }
            if (s_lastMono)
                return State::Mono3D;
            return s_last3D ? (s_lastComfort ? State::Comfort : s_lastDepth ? State::Depth : State::Flat) : State::Off;
        }

        std::string DebugText()
        {
            const game::GameVersion *v = V();
            const u32 dev = v ? GxDevice() : 0;
            const u32 freeB = dev ? VramBFree(v, dev) : 0;
            // Every line <= 52 chars: the bottom screen fits 53 (6 px font).
            return Utils::Format(
                "3D %s %s slider %d%% %dpx\n"
                "mode %03lX %s Lvram %s copies %lu swaps %lu\n"
                "R %08lX L %08lX freeB %luK\n"
                "shown L %08lX R %08lX\n"
                "eye %d comp %dpx proj %s flt %08lX\n"
                "req %lu/%lu skip %lu al %08lX\n"
                "comfort %s ok %lu last %s lock %s",
                StateName(GetState()), ModeName(s_mode), (int)(stereo_get_slider() * 100.0f + 0.5f),
                CurrentPresetPx(), v ? ModeWord() : 0UL,
                s_source == RightSource::LeftBuffer ? "diag" : "vram", s_haveL ? (s_leftVram ? "on" : "off") : "n/a",
                s_frames3D, s_swaps3D, s_buf[kR][0], s_buf[kL][0], freeB >> 10,
                s_lastLeft, s_lastRight, (int)s_fp.eye, s_lastComp, s_projOk ? "ok" : "off", s_faultInfo,
                s_reqUsed, s_reqMax, s_reqSkips, s_allocInfo,
                comfort::Ready() ? "ready" : comfort::NotReadyWhy(), comfort::Replayed(),
                comfort::ReasonName(static_cast<u8>(comfort::LastReason())), comfort::PacerOn() ? "30" : "off");
        }
    }

    // ---- public option API (stereo.hpp) ----
    int         PresetCount()           { return kPresetCount; }
    const char *PresetName(int i)       { return (i >= 0 && i < kPresetCount) ? kPresets[i].name : "?"; }
    int         PresetPixels(int i)     { return (i >= 0 && i < kPresetCount) ? kPresets[i].px : 0; }
    void        SetPreset(int i)        { if (i >= 0 && i < kPresetCount) s_preset = i; }
    int         Preset()                { return s_preset; }
    void        SetRightSource(RightSource s) { s_source = s; }
    RightSource GetRightSource()        { return s_source; }
    const char *RightSourceName(RightSource s)
    {
        return s == RightSource::LeftBuffer ? "left-buffer (diag)" : "VRAM copy";
    }
    int CurrentDisparity()              { return s_last3D ? s_lastDisp : 0; }
    u32 CurrentMode()
    {
        const game::GameVersion *v = V();
        return (v && v->nngxState && v->offDisplayMode) ? ModeWord() : 0;
    }

    void        SetMode(Mode m)         { s_mode = m; }
    Mode        GetMode()               { return s_mode; }
    const char *ModeName(Mode m)        { return m == Mode::Flat ? "flat" : m == Mode::Comfort ? "comfort" : "alternate"; }
    const char *ModeMenuName(Mode m)
    {
        return m == Mode::Flat ? "Flat plane (fallback)" : m == Mode::Comfort ? "Comfort 3D (default)"
                                                                              : "Experimental: alternate eyes";
    }
    void        SetLock30(bool on)      { s_lock30 = on; }
    bool        Lock30()                { return s_lock30; }
    bool        ThreeDActive()          { return s_threeD; }
    bool        ComfortActive()         { return s_comfortOn; }
    bool        ComfortSelected()
    {
        return s_enabled && s_mode == Mode::Comfort && s_haveL && s_status == Status::Ready &&
               s_source == RightSource::VramCopy && comfort::Ready();
    }
    int         TdHandPx(int i)         { return (i >= 0 && i < kTdPresetCount) ? static_cast<int>(kTdPresets[i].hand) : 0; }
    int         TdPresetCount()         { return kTdPresetCount; }
    const char *TdPresetName(int i)     { return (i >= 0 && i < kTdPresetCount) ? kTdPresets[i].name : "?"; }
    void        SetTdPreset(int i)      { if (i >= 0 && i < kTdPresetCount) s_tdPreset = i; }
    int         TdPreset()              { return s_tdPreset; }
    void        SetTurnComp(bool on)    { s_turnComp = on; }
    bool        TurnComp()              { return s_turnComp; }
    void        SetLeftVram(bool on)    { s_leftVram = on; }
    bool        LeftVram()              { return s_leftVram; }
    void        SetMenuPop(bool on)     { s_menuPop = on; }
    bool        MenuPop()               { return s_menuPop; }
    void        SetHudSize(int i)       { if (i >= 0 && i <= 2) s_hudSize = i; }
    int         HudSize()               { return s_hudSize; }
    const char *HudSizeName(int i)      { return i == 0 ? "Normal (game)" : i == 1 ? "Small" : "Smaller"; }
    int         HoldFrames()            { return static_cast<int>(s_hold.maxRun); }
    int CurrentPresetPx()
    {
        return s_mode != Mode::Flat ? static_cast<int>(kTdPresets[s_tdPreset].px) : kPresets[s_preset].px;
    }
}

// C entry points for the asm stubs (stereo_stubs.s)
extern "C" void mc3d_after_top_b2l(u32 idx, u32 dev)   { mc3d::stereo::flat::AfterTopB2L(idx, dev); }
extern "C" u32  mc3d_swap_eyes(u32 left, u32 *leftOut) { return mc3d::stereo::flat::SwapEyes(left, leftOut); }
