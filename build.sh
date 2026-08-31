#!/usr/bin/env bash
# ============================================================================
#  build.sh - one-shot cross-compile of ExecTI for Windows x86_64 + ARM64
# ============================================================================
#  Requires:
#    * mingw-w64  (Debian/Ubuntu:  sudo apt-get install mingw-w64)
#        -> provides x86_64-w64-mingw32-g++ for the x64 build
#    * llvm-mingw (https://github.com/mstorsjo/llvm-mingw)
#        -> provides aarch64-w64-mingw32-clang++ for the ARM64 build
#
#  If llvm-mingw lives somewhere else, point LLVM_MINGW at it, e.g.:
#      LLVM_MINGW=/opt/llvm-mingw ./build.sh
# ============================================================================
set -euo pipefail

LLVM_MINGW="${LLVM_MINGW:-/opt/llvm-mingw}"
if [ -d "$LLVM_MINGW/bin" ]; then
    export PATH="$LLVM_MINGW/bin:$PATH"
fi

echo "==> Toolchains"
command -v x86_64-w64-mingw32-g++      >/dev/null && echo "  x64  : $(x86_64-w64-mingw32-g++ --version | head -1)"      || { echo "  x64  : MISSING (install mingw-w64)"; MISS=1; }
command -v aarch64-w64-mingw32-clang++ >/dev/null && echo "  arm64: $(aarch64-w64-mingw32-clang++ --version | head -1)" || { echo "  arm64: MISSING (install llvm-mingw)"; MISS=1; }

if [ "${MISS:-0}" = "1" ]; then
    echo "!! Missing a toolchain (see above). Aborting." >&2
    exit 1
fi

echo "==> Building both architectures"
make all

echo "==> Artifacts"
for f in build/x86_64/execti.exe build/arm64/execti.exe; do
    [ -f "$f" ] && echo "  $f  ($(du -h "$f" | cut -f1))"
done
echo "Done."
