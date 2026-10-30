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
# The compiler must use the POSIX thread model: the app (and the relay) use
# std::thread / std::condition_variable, which MinGW's default win32 thread model
# does not implement. Ubuntu's g++-mingw-w64-i686 ships both binaries
# (i686-w64-mingw32-g++-posix and -win32), so the -posix one is preferred.
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
    # POSIX threads first: std::condition_variable does not exist in the win32
    # thread model build of MinGW's standard library.
    for candidate in i686-w64-mingw32-g++-posix x86_64-w64-mingw32-g++-posix \
                     i686-w64-mingw32-g++ x86_64-w64-mingw32-g++; do
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

# The app needs std::thread/std::condition_variable. A MinGW built with the win32
# thread model cannot compile them at all, which says nothing about the XP API
# surface - skip instead of reporting a false failure.
PROBE="${TMPDIR:-/tmp}/mub-xp-probe.cpp"
cat > "$PROBE" <<'PROBE_EOF'
#include <condition_variable>
#include <mutex>
#include <thread>
int main() { std::mutex m; std::condition_variable c; std::thread t([] {}); t.join(); return 0; }
PROBE_EOF
if ! "${COMPILER[@]}" -std=c++17 -c -o /dev/null "$PROBE" 2>/dev/null; then
    echo "skip: $LABEL uses the win32 thread model (no std::condition_variable);"
    echo "      install the POSIX variant (g++-mingw-w64-i686-posix) to run this check."
    exit 0
fi
OUT="${TMPDIR:-/tmp}/mub-xp-check"
rm -rf "$OUT"
mkdir -p "$OUT"

DEFINES=(-DUNICODE -D_UNICODE -DWIN32_LEAN_AND_MEAN -DNOMINMAX
         -D_WIN32_WINNT=0x0501 -DWINVER=0x0501 -D_CRT_SECURE_NO_WARNINGS -DMUB_TARGET_XP=1)
# -pthread selects the POSIX thread model when the compiler supports both.
EXTRA=(-pthread)

FILES=(src/core/*.cpp src/win/*.cpp src/net/*.cpp src/server/*.cpp src/client/*.cpp
       src/ui/*.cpp src/main.cpp relay/relay.cpp)

FAILED=0
COMPILED=0
for f in "${FILES[@]}"; do
    [ -f "$f" ] || continue
    OBJ="$OUT/$(echo "$f" | tr '/' '_').o"
    if ! "${COMPILER[@]}" -std=c++17 -c -o "$OBJ" "${DEFINES[@]}" "${EXTRA[@]}" -Isrc "$f" \
            2>"$OUT/log.txt"; then
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
