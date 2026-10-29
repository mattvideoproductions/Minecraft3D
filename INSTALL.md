# Minecraft 3D mod v0.3.3: install guide

Minecraft 3D is a Luma3DS plugin (a `.3gx` file) that adds real stereoscopic 3D to
Minecraft: New Nintendo 3DS Edition. This guide takes about five minutes.

The mod contains no game files. You need your own legally owned copy of the game.

## Requirements

- A New 3DS / New 3DS XL / New 2DS XL running custom firmware with Luma3DS. The plugin
  loader is built into Luma3DS, so nothing else is needed. Old 3DS is not supported.
- **Minecraft: New Nintendo 3DS Edition, USA version** (title ID `00040000001B8700`),
  **updated to v9.12.0** (game 1.9.19, the final update).

Other regions or versions are not patched: the plugin checks the game's code against its
address tables and, if it does not match, stays idle (nothing is broken, nothing works).

## SD card layout

```
SD:/luma/plugins/00040000001B8700/Minecraft3D.3gx     <- the plugin (from releases/v0.3.3/)
SD:/luma/plugins/00040000001B8700/mc3d_cfg.txt        <- optional config (see below)
SD:/luma/plugins/00040000001B8700/mc3d_log.txt        <- written by the plugin
```

The folder name is the title ID. Create any missing folders.

## Install

1. Copy `releases/v0.3.3/Minecraft3D.3gx` to the path above.
2. Enable the plugin loader: open the Rosalina menu with **L + D-pad Down + SELECT**,
   choose "Plugin loader: [Disabled]" and press **A** so it reads Enabled. Luma
   remembers this.
3. Launch Minecraft. The 3D picture appears when the 3D slider is up.
4. Press **SELECT** for the plugin menu. "Plugin status" shows the version.

## The plugin menu (SELECT)

- **Stereo 3D**: 3D on/off, Mode (Comfort 3D / alternate eyes / flat plane), Depth preset
  (Deep default, Immersive, Normal, Gentle, Window, Zero), Top HUD size, 30 fps lock,
  turn compensation (alternate-eyes mode only), Experimental: auto-flatten when slow,
  3D menus, and some diagnostics.
- **World (distance, leaves)**: render distance, "Lower distance while 3D is on",
  the 3D render distance, leaves (fancy / fast / auto), far fog. The distance entry shows
  what is really in use, e.g. `4 -> 3 in 3D (auto)` or `6 refused (low memory), 4 used`.
  Choices apply while a world is running.
- **Performance**: stats on the bottom screen, log stats to SD, show latest stats,
  record 60 s to CSV.
- **Swap A/B buttons** (off by default).

Closing the menu with SELECT can open the game's pause menu (a known issue).

## Config file: `mc3d_cfg.txt`

Optional. One `key=value` per line, `#` starts a comment; a UTF-8 BOM, CRLF line ends and
spaces around `=` are accepted. A missing file or key means the built-in default. Values
set the menu's start-up state; everything stays changeable at runtime.

| Key | Values | Meaning |
|---|---|---|
| `mode` | `comfort`, `alternate` (or `depth`), `flat` | Stereo mode |
| `tdepth` | `0`..`5` | Comfort depth preset: 0 Deep, 1 Immersive, 2 Normal, 3 Gentle, 4 Window, 5 Zero |
| `flatdepth` | preset number | Depth preset for the flat-plane mode |
| `comp` | `on`/`off` | Turn compensation for alternate-eye mode |
| `lock` | `on`/`off` | 30 fps lock in Comfort 3D |
| `hold` | number | Frames to repeat the last good pair after a failed frame (default 3; `0` = off) |
| `gov` | `on`/`off` | Experimental auto-flatten when slow (default off) |
| `hud` | `0`, `1`, `2` | Top HUD size: Normal / Small / Smaller |
| `rd` | chunks, `0` = game | Render distance |
| `rdauto` | `on`/`off` | Lower the distance while 3D is on |
| `rd3d` | chunks, `0` = auto | Render distance used while 3D is on |
| `leaves` | `auto`/`on`/`off` | `on` = fast leaves |
| `farfog` | blocks, `0` = off | Far fog distance |
| `abswap` | `on`/`off` | Start with Swap A/B buttons on |
| `menupop` | `on`/`off` | 3D menus (text pops out) |
| `leftvram` | `on`/`off` | Glitch-line fix (left eye in VRAM) |
| `log` | `on` | Start with "Log stats to SD" on |
| `osd` | `on` | Start with stats on the bottom screen |
| `guardheap` | `0`..`9` | Diagnostic: guard only this heap slot |
| `hooks` | list of `present init b2l swap world draw sync rebuild jobs split proj prof comfort input all none` | Diagnostic: install only these hooks |
| `irrst` | `on` | Diagnostic: let the framework open ir:rst (this breaks the game's C-stick; leave off) |

Example:

```
tdepth=1        # Immersive
hud=0
rd=4
```

## Log file

The plugin writes `SD:/luma/plugins/00040000001B8700/mc3d_log.txt` without any setting:
one `====` line per start with the settings in use, one `SW` line for every change of the
top-screen picture (3D / held / 2D / flat), `RD` lines for render-distance changes or
refusals, and an `FB` summary every 10 s. Over 1 MiB it is renamed to `mc3d_log_old.txt`
and a new file is started. If something goes wrong, please send it along with what you
were doing.

## Updating and uninstalling

Replace `Minecraft3D.3gx` with the new file. To remove it, delete the file or turn off
"Plugin loader" in the Rosalina menu.

## Troubleshooting

- **No 3D at all**: check the plugin loader is enabled, the file is in the title-ID
  folder, and the game is the USA version at v9.12.0. "Plugin status" in the menu reports
  whether the version table matched.
- **Eye strain or double vision**: lower the 3D slider or use the Gentle or Window preset.
- **Render distance stuck at 4**: this is deliberate, see Known issues in the README.

## Building from source

Requirements (kept outside this repo):

- devkitPro with devkitARM (`DEVKITPRO` / `DEVKITARM` set; on Windows use its msys2 shell).
- CTRPluginFramework (libctrpf) and 3gxtool 1.3, placed in a `toolchain/` folder two
  levels above `plugin/`:

  ```
  <projects>/toolchain/libctrpf/
  <projects>/toolchain/3gxtool-1.3/3gxtool.exe
  <projects>/<repo>/plugin/
  ```

  Or override on the command line: `make TOOLCHAIN=/path/to/toolchain` (or `CTRPFLIB=` /
  `GXTOOL=`).

Build:

- Windows: `./build.ps1` (release), `./build.ps1 debug`, `./build.ps1 clean`. It runs
  `build.sh` inside the devkitPro msys2 shell (set `DEVKITPRO_WIN` if devkitPro is not in
  `C:\devkitPro`).
- msys2/Linux: `./build.sh`, `./build.sh debug`, `./build.sh clean`.

devkitPro's make rules break on paths that contain spaces, so when the repo's parent
path has a space `build.sh` builds through a space-free directory junction
(`%TEMP%\mc3d-root`). Output: `plugin/Minecraft3D.3gx`.

Host tests (no 3DS needed): `plugin/tests/run_tests.ps1` runs a Python model of the pure
logic (projection, frame holding, pacing, A/B latch, event log) and a C++ test of the same
code, which is compile-checked with devkitARM and run if a desktop `g++` is on `PATH`.

## Credits and legal

See the README. Minecraft 3D is a fan mod, not affiliated with Mojang, Microsoft, Other
Ocean Interactive or Nintendo. No game files are included or distributed; you must own the game.
