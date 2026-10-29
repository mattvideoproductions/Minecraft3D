# Minecraft 3D (v0.3.3)

A Luma3DS 3GX plugin that adds real stereoscopic 3D to **Minecraft: New Nintendo 3DS
Edition** (USA) on the New Nintendo 3DS / New 3DS XL / New 2DS XL. The game never
shipped with working 3D on the update most people have; this plugin renders both eyes
with real per-pixel depth.

## Features

- **Comfort 3D**: both eyes are drawn in the same frame with real per-pixel depth
  (30 fps lock by default), so the picture does not shimmer between eyes.
- **Depth presets**: Deep (default), Immersive (things can come out of the screen
  more), plus Normal, Gentle, Window and a Zero test preset.
- **Held frames instead of a flat fallback**: if a frame cannot be drawn in 3D, the
  last good stereo pair is repeated for a moment instead of flashing a flat 2D frame.
- **Experimental auto-flatten** (off by default): can drop to a flat plane when the
  game is running slowly.
- **Render distance control** with an option to automatically lower the distance while
  3D is on, plus leaf and far-fog options.
- **HUD size** (Normal / Small / Smaller). Small and Smaller also shrink menus
  (pause, inventory, chests), because the plugin cannot yet tell them from the HUD.
- **A/B swap** option (off by default).
- **C-stick preserved**: the game's own C-stick input keeps working.
- **Logging** to the SD card (`mc3d_log.txt`), with rotation, plus an optional
  on-screen stats display and CSV profile.

## Requirements

- New Nintendo 3DS / New 3DS XL / New 2DS XL with Luma3DS (plugin loader built in).
  The Old 3DS is not supported.
- **Minecraft: New Nintendo 3DS Edition, USA** (title ID `00040000001B8700`),
  **updated to v9.12.0 (game 1.9.19)**. Other regions and versions are not supported;
  the plugin verifies the game's code and stays idle on anything it does not know.

## Quick install

1. Copy `releases/v0.3.3/Minecraft3D.3gx` to
   `SD:/luma/plugins/00040000001B8700/Minecraft3D.3gx`.
2. In the Rosalina menu (L + D-pad Down + SELECT), set "Plugin loader" to Enabled.
3. Launch Minecraft, push the 3D slider up, and press **SELECT** for the plugin menu.

Full instructions, the config file and building from source: see [INSTALL.md](INSTALL.md).
A test checklist for this release is in [releases/v0.3.3/HOW_TO_TEST.md](releases/v0.3.3/HOW_TO_TEST.md).

## Known issues

- Closing the plugin menu with SELECT can open the game's pause menu.
- Long lines in the plugin menu are clipped in the right eye.
- A render distance above 4 is refused: the game fills the memory it needs as soon as a
  world loads, so there is no safe room. The menu says what is really in use.
- HUD size Small / Smaller is experimental and also shrinks the pause menu, inventory and chests.

## Legal

This repository contains no game files; you need your own legally owned copy of
Minecraft: New Nintendo 3DS Edition. Minecraft 3D is a fan mod, not affiliated with
Mojang, Microsoft, Other Ocean Interactive or Nintendo. Minecraft is a trademark of
its respective owners.

The plugin's own code is MIT licensed (see [LICENSE](LICENSE)). The built `.3gx`
links CTRPluginFramework and libctru, which carry their own licenses.

## Credits

Developed by Claude (Anthropic's AI) as an AI-driven reverse-engineering project, with
mattvideoproductions as project lead and hardware tester on a New 3DS XL.

Built with:

- [CTRPluginFramework](https://gitlab.com/thepixellizeross/ctrpluginframework) and 3gxtool (GPL-3.0; see the upstream repository)
- [libctru](https://github.com/devkitPro/libctru) (zlib license)
- [devkitPro](https://devkitpro.org/) (devkitARM toolchain; licenses per component)
- [Luma3DS](https://github.com/LumaTeam/Luma3DS) plugin loader (GPL-3.0 with additional terms)

Public community projects were read as background research only. No code from them is
included, but they helped orient the reverse engineering:

- [STBrian/LunaCore](https://github.com/STBrian/LunaCore) (MIT)
- [s4ammy/3DSMinecraft](https://github.com/s4ammy/3DSMinecraft) (MIT)
- [kejran/ctraft](https://github.com/kejran/ctraft) (MIT)
- [ENDERMANYK/Minecraft-3ds-shader](https://github.com/ENDERMANYK/Minecraft-3ds-shader) (MIT)

Not affiliated with Mojang, Microsoft, Other Ocean Interactive or Nintendo. No game
files are included; you need your own copy.
