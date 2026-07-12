#!/bin/sh
# build.sh -- build the `ud` interpreter (Linux / macOS / MSYS2).
# Produces a small, optimized, stripped binary.
set -e

CC="${CC:-gcc}"
OUT=ud
case "$(uname -s 2>/dev/null)" in
    *NT*|*MINGW*|*MSYS*|*CYGWIN*) OUT=ud.exe ;;
esac

echo "Compiling UD -> $OUT"
"$CC" -std=c11 -O2 -ffunction-sections -fdata-sections \
      -fno-asynchronous-unwind-tables -fno-unwind-tables \
      -Isrc -o "$OUT" src/*.c -s -Wl,--gc-sections

echo "Done: $OUT ($(wc -c < "$OUT") bytes)"
