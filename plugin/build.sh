#!/usr/bin/env bash
# Build Minecraft3D.3gx from the devkitPro msys2 shell (C:\devkitPro\msys2\msys2_shell.bat)
#   ./build.sh            release build  -> Minecraft3D.3gx
#   ./build.sh debug      debug build    -> Minecraft3D-debug.3gx
#   ./build.sh clean
#
# devkitPro's make rules cannot cope with spaces in paths (e.g. "My Projects"), so the build runs through a directory junction in msys2's /tmp whose path
# has no spaces: <msys2 /tmp>\mc3d-root -> <parent of the repo folder>.
set -e

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITARM="${DEVKITARM:-$DEVKITPRO/devkitARM}"
export PATH="$DEVKITARM/bin:$DEVKITPRO/tools/bin:$PATH"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../.." && pwd)"              # folder holding this repo and toolchain/
REL="${HERE#$ROOT/}"                           # <repo>/plugin

case "$ROOT" in
  *" "*)
    TMPW="$(cygpath -w "${TEMP:-/tmp}")"
    LINKW="$TMPW\\mc3d-root"
    LINK="$(cygpath -u "$LINKW")"
    if [ ! -d "$LINK/$REL" ]; then
      # rmdir on a junction removes only the link, never the target's contents
      [ -e "$LINK" ] && MSYS2_ARG_CONV_EXCL='*' cmd /c rmdir "$LINKW" >/dev/null 2>&1 || true
      echo "Creating junction $LINKW -> $(cygpath -w "$ROOT")"
      MSYS2_ARG_CONV_EXCL='*' cmd /c mklink /J "$LINKW" "$(cygpath -w "$ROOT")" >/dev/null
    fi
    BUILD_DIR="$LINK/$REL"
    ;;
  *) BUILD_DIR="$HERE" ;;
esac

cd "$BUILD_DIR"
make -j"$(nproc)" "${@:-release}"

if [ "${1:-release}" = "release" ]; then
  ls -l "$HERE/Minecraft3D.3gx"
fi
