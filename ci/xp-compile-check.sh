#!/usr/bin/env bash
# Mini USB Bridge - compile the whole Windows source tree against the Windows XP
# SP3 API surface (_WIN32_WINNT=0x0501).
#
# The compiler's headers hide every function that does not exist on XP, so this
# check fails exactly when somebody starts using a newer API directly instead of
# going through src/win/wincompat.cpp. It complements ci/check_legacy_compat.py
# (a textual lint) with a real compile.
#
#   bash ci/xp-compile-check.sh
#   XP_CXX="zig c++ -target x86-windows-gnu" bash ci/xp-compile-check.sh
#
# Skips cleanly when no MinGW cross compiler is installed (the GitHub Linux job
# installs g++-mingw-w64-i686, so it does run there).
# SPDX-License-Identifier: MIT
set -uo pipefail
cd "$(dirname "$0")/.."

if [ -n "${XP_CXX:-}" ]; then
    # shellcheck disable=SC2206
    COMPILER=($XP_CXX)
    LABEL="$XP_CXX"
else
    COMPILER=()
    LABEL=""
    for candidate in i686-w64-mingw32-g++ x86_64-w64-mingw32-g++; do
        if command -v "$candidate" >/dev/null 2>&1; then
            COMPILER=("$candidate")
            LABEL="$candidate"
            break
        fi
    done
fi

if [ ${#COMPILER[@]} -eq 0 ]; then
    echo "skip: no MinGW cross compiler found (install: sudo apt-get install -y g++-mingw-w64-i686)"
    exit 0
fi

echo "XP target compile check with: $LABEL"
OUT="${TMPDIR:-/tmp}/mub-xp-check"
rm -rf "$OUT"
mkdir -p "$OUT"

DEFINES=(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX
         -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -D_CRT_SECURE_NO_WARNINGS -DMUB_TARGET_XP=1)

FILES=(src/core/*.cpp src/win/*.cpp src/net/*.cpp src/server/*.cpp src/client/*.cpp
       src/ui/*.cpp src/main.cpp relay/relay.cpp)

FAILED=0
COMPILED=0
for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    OBJ="$OUT/$(echo "$f" | tr '/' '_').o"
    if ! "${COMPILER[@]}" -std=c++17 -c -o "$OBJ" "${DEFINES[@]}" -Isrc "$f" 2>"$OUT/log.txt"; then
        echo "  FAIL $f"
        head -n 12 "$OUT/log.txt" | sed 's/^/       /'
        FAILED=$((FAILED + 1))
    fi
    COMPILED=$((COMPILED + 1))
done

echo "compiled $COMPILED translation units against the Windows XP API surface"
if [ "$FAILED" -ne 0 ]; then
    echo "XP compatibility: FAIL ($FAILED file(s))"
    exit 1
fi
echo "XP compatibility: PASS"
