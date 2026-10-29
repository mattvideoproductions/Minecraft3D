# Minecraft 3D mod v0.3.3: how to test (New 3DS XL)

Same game and update as before: Minecraft: New Nintendo 3DS Edition (USA), update v9.12.0 (1.9.19).

**Install:** copy `luma\plugins\00040000001B8700\Minecraft3D.3gx` over the old one. The boot message and "Plugin status" should say **v0.3.3**.

> v0.3.3 = v0.3.2 (approved on hardware) plus one new option, **Swap A/B buttons**. The only other change: the `hud=0|1|2` line in `mc3d_cfg.txt` now really works (v0.3.2 ignored it).
> The swap was checked in Azahar on the title menu only (physical B opened Play, physical A went back; toggling it off in the plugin menu restored the normal buttons). Not yet run on hardware.

**No need to delete the old `mc3d_log.txt` any more.** If it is over 1 MB, the plugin renames it to `mc3d_log_old.txt` at start-up and begins a new one.

## Defaults (plug and play, nothing to set)
- Comfort 3D, depth **Deep**, 30 fps lock on, turn compensation **off**.
- **Always full 3D.** The old auto-flatten is now a menu item, "Experimental: auto-flatten when slow". It is **off**.
- **Top HUD size: Normal** (the game's own size). Small (80 %) and Smaller (64 %) are experimental: they also shrink the pause menu, inventory and chests, because the plugin can't yet tell those apart from the in-world HUD.

## Please check
0. **NEW: Swap A/B buttons** (Select → last item "Swap A/B buttons", **off** by default).
   Turn it on, close the menu with Select, then check each of these. With it on, B does what A normally does and A does what B normally does:
   - **Title menu:** B opens "Play", A goes back.
   - **Pause menu** (Start in a world): B picks the highlighted item, A backs out.
   - **Inventory** and **crafting:** B does what A did there (take/place), A does what B did (close/back).
   - **Chest:** same as the inventory: B takes/places, A closes.
   - **In the world:** B jumps (A's old job), and A does what B used to do. X, Y, L, R, ZL, ZR, the C-stick, the circle pad and touch must be unchanged.
   - **The plugin menu itself keeps the real buttons:** A still selects, B still goes back, Select still opens and closes it.
   - **Toggling while holding a button:** hold B (= jump with the swap on) or hold A, open the plugin menu with Select, flip the option, close the menu and only then let go. Nothing may get "stuck" (no endless jumping, no button that stays pressed). The new mapping starts only once A and B are both released, so expect it on the next press.
   - Restart the game: the option starts **off** again unless `abswap=on` is in `mc3d_cfg.txt` (or CTRPF's own "auto-save enabled cheats" is on). The `====` boot line in `mc3d_log.txt` says `abswap on` or `abswap off`.
1. **HUD size, optional and experimental** (Select → Stereo 3D → "Top HUD size"): Normal (default) / Small / Smaller.
   - The game already draws its top HUD at 1 screen pixel per HUD pixel. Anything smaller can't be pixel-exact, so look out for uneven or blurry letters.
   - The HUD shrinks toward the screen centre. Hearts and the hotbar move up a little, and chat moves in from the edges.
   - Check: the crosshair still aims at the block you mine; the pause menu and inventory look right (no dark box that is too small); the bottom screen and touch are unchanged.
   - Expect the pause menu, inventory and chests to shrink as well (the dark background may cover only the middle of the screen). Tell us if any of them look broken; Normal puts everything back.
2. **Render distance menu text** (Select → World): the item name now shows what the game really uses.
   - Example: `4 -> 3 in 3D (auto)` means your distance is 4 and 3D draws 3.
   - Pick 6, close the menu, play for a second, then open the menu again. It should say `6 refused (low memory), 4 used` (choices apply only while the world is running). 5 and 6 are almost always refused (see below).
   - With "Lower distance while 3D is on" off, it should say just `4`.
3. **HOME button and lid**: in-world with 3D up, press HOME and go back to the game once, then close and open the lid once. Watch the first frames after each.
   - v0.3.1 could briefly show the picture from before HOME (up to 3 frames). v0.3.2 should never do that; at worst you see one flat frame.
4. Play normally. The picture should never flip to the flat plane.

## Why 6 chunks doesn't work
The game fills one of its memory areas completely as soon as a world loads, so there is never enough free memory to safely go above 4. v0.3.0 refused 6 with only a short pop-up, and the menu never showed what the game really used, so it looked stuck at 4. Now the menu and the log say so.

## What to send back
`luma/plugins/00040000001B8700/mc3d_log.txt` (and `mc3d_log_old.txt` if there is one). You don't need to switch on any logging:
- each start writes one `====` line with the settings in use;
- every switch between 3D, held, 2D and flat writes an `SW` line (time, from>to, reason, frame time, fps, render distance);
- every render-distance change or refusal writes an `RD` line;
- the `FB` summary is written every 10 s.
