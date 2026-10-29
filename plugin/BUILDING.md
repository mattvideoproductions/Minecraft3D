# Building Minecraft3D.3gx

See "Building from source" in [../INSTALL.md](../INSTALL.md). Short version:

- devkitPro with devkitARM, CTRPluginFramework (libctrpf) and 3gxtool 1.3.
- Put `libctrpf/` and `3gxtool-1.3/` in a `toolchain/` folder two levels above `plugin/`
  (`<projects>/toolchain/`, with this repo at `<projects>/<repo>/`), or pass
  `TOOLCHAIN=`, `CTRPFLIB=` or `GXTOOL=` to `make`.
- Windows: `./build.ps1` (release), `./build.ps1 debug`, `./build.ps1 clean`.
- msys2/Linux: `./build.sh`, `./build.sh debug`, `./build.sh clean`.
- Output: `plugin/Minecraft3D.3gx`.
- Host tests: `plugin/tests/run_tests.ps1` (Python model of the pure code, plus a C++ test).
