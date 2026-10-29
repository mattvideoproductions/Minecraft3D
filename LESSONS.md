# Lessons from the Minecraft 3D mod (for every 3DS plugin port)

Hardware and engineering lessons from building stereoscopic 3D for Minecraft: New
Nintendo 3DS Edition, v0.1.0 to v0.3.3, and from the sister Terraria 3D mod. Read this
before writing hardware-facing code.

## Hardware crashes and black screens (the emulator passed every one of these)
1. **Instruction cache.** Use `svcInvalidateEntireInstructionCache`, as CTRPF does. The
   range variant runs in whatever process is current and caused a kernel data abort.
2. **Reserve stereo memory early, and test with the slider at 0 at boot.** Allocating
   right-eye buffers in-world fails on hardware because game textures fill VRAM; an
   emulator run with the slider already up hides it.
3. **VRAM-A is a budget, not free space.** Minecraft 1.9.19 leaves about 9 KiB of VRAM-A
   spare. While loading it allocates a 2.625 MiB texture area and never checks the result.
   911 KiB right-eye buffers in VRAM-A made that allocation return 0, the game DMA'd to
   address 0 and hung on "Loading". Map the game's VRAM use first; put right-eye buffers
   in VRAM-B below 0x1F5F8000 (nngx uses the last 32 KiB), and leave stereo off rather
   than allocate into the game's space.
4. **Keep both eyes in the same memory type.** The SDK sets the display DMA size from the
   LEFT framebuffer address alone: FCRAM gives 8-word bursts, VRAM gives the larger size.
   A VRAM right eye behind an FCRAM left eye ran with small bursts from a busy VRAM bank
   and showed "glitchy lines" that moved with bottom-screen activity. FCRAM buffers can
   also show black on hardware. Use VRAM for both.
5. **CTRPF breaks the game's C-stick on New 3DS.** By default CTRPF calls `hidInit()`,
   which on New 3DS runs `irrstInit()`; games that read the C-stick through ir:USER then
   get 0,0. Fix: `extern "C" bool hidShouldUseIrrst(void){ return false; }`. Never call
   `irrstInit` in a plugin unless the game uses ir:rst. (Reading the C-stick yourself
   through ir:rst is the opposite fix when the game's own path is dead; test on hardware.)
6. **Plugin memory.** The plugin's memory can come out of the game's. `UsePrivateMemory:
   true` in the `.plgInfo` keeps the plugin's 5 MiB out of the game's heap.
7. **Process exit.** Do not free the CTRPF menu or anything else at shutdown; deleting
   the menu after `menu->Run()` returns crashed.
8. **A hook on the GSP event thread runs on a ~4 KiB stack.** Format OSD strings on the
   plugin thread; only draw them there.
9. **Closing the CTRPF menu can trip the game's own pause.** Watch for page changes right
   after the menu closes. (Still an open issue in Minecraft: Select closing the menu can
   open the pause menu.)
10. **Display-controller-level features need hardware.** The colour LUT registers are not
    emulated by Azahar.

## Emulator limits (Azahar)
- It hides cache, VRAM-capacity, ir:USER, timing and display differences. Passing the
  emulator is not proof.
- Emulator frame timing is not hardware timing. A governor tuned at 60 fps in the
  emulator tripped constantly at 20-30 fps on a real console.
- A guest exception pops a crash dialog that steals focus; kill the emulator at once.
- Only one agent drives the emulator or desktop at a time.

## Stereo design
- **Dead features get deleted across updates.** Minecraft v1.0 has a complete stereo
  renderer behind a kill-switch byte; the 1.9.19 update removed it entirely. Reverse
  engineer the exact version that runs on the console.
- **A projection shear has no built-in limit on pop-out.** Disparity is s*px*(1 - C/D),
  which runs to minus infinity as D gets small (-56 px against a wall with C = 1.5,
  -148 px with C = 4). Choose the zero-parallax distance from the nearest the camera can
  ever get to a surface, and edit only the top screen's matrices (the bottom screen goes
  through the same uniform path; an aspect test on the projection tells them apart).
- **Alternate-eye stereo needs turn compensation.** If the eyes are one frame apart, a
  camera turn adds false disparity of about yaw rate x frame time. Shifting the older eye
  by the projected camera change fixes the world but makes HUD and hand wobble during turns.
- **Don't assume the game's triple buffer is round-robin.** Minecraft picks the first
  free display index, so it usually alternates 0 and 1. Any scheme that keeps an older
  image on screen needs its own buffer rotation.
- **Same-frame both-eye rendering beats alternating eyes.** Comfort 3D replays the
  frame's GPU command list for the right eye with per-eye projection; both eyes always
  show the same moment in the world. Prove each replay step before writing it and drop
  to a safe state on any anomaly.
- **GPU reads after CPU writes.** Flush the data cache and never reuse memory the GPU
  will read later in the frame; the emulator rarely shows this.

## Automatic fallbacks and diagnostics
- **Automatic quality fallbacks must not change the picture's look.** A governor that
  dropped to a flat plane for 10 s after 6 late frames in 60 looked fine at 60 fps but
  on hardware at 20-30 fps nearly every frame is late, so it flipped between flat and 3D
  constantly. A stable, slightly worse mode beats a flipping one. For a failed frame,
  repeat the last good stereo pair rather than show one 2D frame, and forget that pair
  after a gap of more than ~200 ms (HOME, sleep).
- **A fallback that reports "3D off" to other systems can trigger rebuilds** (render
  distance, leaves) that make the next retry fail harder.
- **Always write a cheap diagnostic log, not only when a debug option is on.** Write a
  few counters every ~10 s from the plugin thread, no per-frame I/O.
- **Log state changes as events.** One line per transition (time, from to, reason, frame
  time, context), queued from the game thread in a lock-free ring and written at most
  once per second. Counts alone cannot tell one long fallback from a hundred short ones.
- **Log caps must rotate, not stop.** Rename to `*_old.txt` past the cap and at boot,
  and write a boot line with the version and settings.
- **Measure the GPU before designing around "GPU slack".** Inferring CPU-bound from short
  present waits was wrong in-world; timing command-list submit to completion showed the
  GPU using 20 ms of a 24 ms frame.
- **A refused or overridden setting must show what is really in effect, in the menu.**
  A render distance of 6 was refused by a memory guard that could never pass in a world
  (the 11 MiB game heap is always full once a world loads), and the only signal was a
  two-second notice. Menu entries for anything the plugin can refuse, clamp or apply
  later must be refreshed from live values (`6 refused (low memory), 4 used`).
- **A memory guard needs evidence about which heap matters.** Log heaps by slot first,
  then guard the one that actually grows.

## Method that worked
- Identify draw passes by giving each a unique horizontal shift and measuring the shifts
  in a screenshot. Name passes from evidence, not guesses.
- Verify struct fields from the disassembly before using them.
- Version-check the exact title and update (signatures or a full code CRC) and refuse to
  patch anything else. Check original instruction words before hooking.
- Make visual mods reversible at runtime, and keep every option reversible.
- Check "the mod did it" against a stock run with the plugin off.
- Check the game's real HUD scale before promising a smaller pixel-perfect HUD: the
  Minecraft top HUD is already one screen pixel per GUI pixel, so anything smaller is
  fractional, and a projection-level scale also shrinks full-screen menus.

## Tooling mistakes
- A scratch script named `dis.py` shadowed Python's stdlib `dis`. Don't name files after
  stdlib modules, and run Python with `-I`.
- Scripted edits turned `"\\n"` into real newlines in C++ strings. Check string literals
  after scripted edits.
- devkitPro's make rules break on paths with spaces; build through a space-free junction.
- Windows SD: after a Shell eject the reader can show size 0 until the card is reseated.
  Flush the volume cache instead of ejecting.
- Cheats and test options change performance. Benchmark with them off, and log which
  options are on.
