#!/usr/bin/env bash
# Cross-compiles Injector.exe from Linux.
#
# This is a plain Win32 program with no Detours and no MSVC extensions, which is
# what lets it build here. Injector.dll still needs a Windows toolchain.
#
#   sudo apt install mingw-w64
set -euo pipefail

cc="${MINGW:-x86_64-w64-mingw32-g++}"
here="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
out="${1:-$here/Injector.exe}"

"$cc" \
  -municode \
  -std=c++17 \
  -O2 \
  -Wall -Wextra \
  -static -static-libgcc -static-libstdc++ \
  -o "$out" \
  "$here/main.cpp"

echo "built $out"
