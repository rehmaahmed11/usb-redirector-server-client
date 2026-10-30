#!/usr/bin/env bash
# Mini USB Bridge - CI check for the portable part (Linux).
#
# Runs on a bare ubuntu-latest runner: no Windows, no Inno Setup. It proves the
# portable core and the relay are healthy and that the installer script only
# references files that exist, and it writes the same style of tool chain report
# as the Windows job (dist/ci/toolchain-report-linux.md) so a red build explains
# itself without digging through the log viewer.
#
#   bash ci/linux-check.sh
# SPDX-License-Identifier: MIT
set -uo pipefail
cd "$(dirname "$0")/.."

CI_DIR="dist/ci"
LOG_DIR="$CI_DIR/logs"
REPORT="$CI_DIR/toolchain-report-linux.md"
ROWS="$CI_DIR/_rows.md"
mkdir -p "$LOG_DIR" "$CI_DIR/linux"
: > "$ROWS"

CXX=${CXX:-g++}
PY=${PY:-python3}
CORE_BIN="$CI_DIR/linux/test_core"
RELAY_BIN="$CI_DIR/linux/mubb-relay"
STAGES=0
FAILED=0
FAILED_STAGE=""
FAILED_LOG=""

# stage <name> <command-as-displayed> <logfile> <shell command...>
stage() {
    local name="$1" shown="$2" log="$3"
    shift 3
    STAGES=$((STAGES + 1))
    echo ""
    echo "==> $name"
    echo "    $*"
    local start end code
    start=$(date +%s)
    if bash -c "$*" >"$log" 2>&1; then code=0; else code=$?; fi
    end=$(date +%s)
    local secs=$((end - start))
    if [ "$code" -eq 0 ]; then
        printf '   -> ok in %ss\n' "$secs"
        printf '| %d | %s | `%s` | ok | %ss | `%s` |\n' "$STAGES" "$name" "$shown" "$secs" "$(basename "$log")" >>"$ROWS"
    else
        printf '   -> FAILED (exit %d)\n' "$code"
        printf '| %d | %s | `%s` | **FAILED (exit %d)** | %ss | `%s` |\n' \
            "$STAGES" "$name" "$shown" "$code" "$secs" "$(basename "$log")" >>"$ROWS"
        FAILED=$((FAILED + 1))
        [ -z "$FAILED_STAGE" ] && FAILED_STAGE="$name" && FAILED_LOG="$log"
        tail -n 25 "$log" | sed 's/^/      /'
        return 1
    fi
    return 0
}

echo "Mini USB Bridge CI (Linux)"
echo "g++ : $($CXX --version 2>&1 | head -1)"
echo "py  : $($PY --version 2>&1 | head -1)"

# 1. build the portable core tests
stage "Build core tests" \
    "$CXX -std=c++17 -Wall -Wextra tests/test_core.cpp src/core/*.cpp" \
    "$LOG_DIR/10-core-build.log" \
    "$CXX -std=c++17 -O2 -Wall -Wextra -Isrc tests/test_core.cpp \
        src/core/base32.cpp src/core/logger.cpp src/core/pairing.cpp src/core/protocol.cpp \
        src/core/session_auth.cpp src/core/settings.cpp src/core/sha256.cpp \
        src/core/strings.cpp src/core/usbip_wire.cpp -o '$CORE_BIN'"

# 2. run them
if [ -f "$CORE_BIN" ]; then
    stage "Core unit tests" "$CORE_BIN" "$LOG_DIR/20-core-run.log" "'$CORE_BIN'"
fi

# 3. build the relay (also the Linux relay build users care about)
stage "Build relay" "$CXX -std=c++17 -O2 -pthread relay/relay.cpp" \
    "$LOG_DIR/30-relay-build.log" \
    "$CXX -std=c++17 -O2 -pthread relay/relay.cpp -o '$RELAY_BIN'"

# 4. relay protocol tests
if [ -f "$RELAY_BIN" ]; then
    stage "Relay protocol tests" "$PY tests/test_relay.py $RELAY_BIN" \
        "$LOG_DIR/40-relay-run.log" "'$PY' tests/test_relay.py '$RELAY_BIN'"
fi

# 5. the installer script must only reference files that exist
stage "Installer sources lint" "$PY ci/check_iss_sources.py installer/MiniUsbBridge.iss --allow 'dist/app/*'" \
    "$LOG_DIR/50-iss-lint.log" \
    "'$PY' ci/check_iss_sources.py installer/MiniUsbBridge.iss --allow 'dist/app/*'"

# 5b. bundled backends fetch script exists and --dry-run succeeds
stage "Backend fetch dry-run" "$PY ci/fetch-backends.py --dry-run" \
    "$LOG_DIR/52-fetch-dryrun.log" \
    "'$PY' ci/fetch-backends.py --dry-run"
# Also verify the ISS bundling patterns are correct (handles both vendor naming variants)
stage "Installer backend patterns" "check bundled Sources lines" \
    "$LOG_DIR/54-backend-patterns.log" \
    "grep -q 'usbipd-win\*.msi' installer/MiniUsbBridge.iss && grep -q 'USBip\*.exe' installer/MiniUsbBridge.iss && grep -q 'BUNDLED.md' installer/MiniUsbBridge.iss && echo 'backend bundling patterns OK'"

# 6. documentation sanity: every docs/*.md linked from the README must exist
LINK_CHECK="missing=0; for f in \$(grep -o 'docs/[A-Za-z0-9._-]*[.]md' README.md | sort -u); do if [ ! -f \"\$f\" ]; then echo \"missing: \$f\"; missing=1; fi; done; echo missing=\$missing; exit \$missing"
stage "Docs link check" "every docs/*.md mentioned in README.md exists" \
    "$LOG_DIR/60-docs.log" "$LINK_CHECK"

# ---------------------------------------------------------------------------
# report
# ---------------------------------------------------------------------------
{
    echo "# Mini USB Bridge - tool chain report (Linux)"
    echo ""
    echo "| | |"
    echo "|---|---|"
    if [ "$FAILED" -eq 0 ]; then
        echo "| Result | **PASS** - portable core, relay and installer sources verified |"
    else
        echo "| Result | **FAIL** at \"$FAILED_STAGE\" |"
    fi
    echo "| Trigger | \`${GITHUB_EVENT_NAME:-local}\` |"
    echo "| Ref | \`${GITHUB_REF:-local}\` |"
    echo "| Commit | \`${GITHUB_SHA:-$(git rev-parse HEAD 2>/dev/null)}\` |"
    echo "| Runner | $(uname -srm) |"
    echo "| Generated | $(date -u '+%Y-%m-%d %H:%M:%S') UTC |"
    echo ""
    echo "This job has no Windows and no Inno Setup, so it cannot produce the exe or the installer."
    echo "It proves the portable core, the relay and the installer *sources*; the Windows job next to it"
    echo "does the MSVC build and ISCC compile."
    echo ""
    echo "## Tool chain"
    echo ""
    echo '```'
    echo "g++ core+tests -> test_core -> g++ relay -> python relay protocol tests"
    echo "                -> check_iss_sources.py (Inno Setup [Files] pre-flight)"
    echo "                -> docs link check"
    echo '```'
    echo ""
    echo "## Stages"
    echo ""
    echo "| # | Stage | Command | Result | Time | Log |"
    echo "|---|---|---|---|---|---|"
    cat "$ROWS"
    echo ""
    echo "## Tool versions"
    echo ""
    echo "| Tool | Version |"
    echo "|---|---|"
    echo "| g++ | $($CXX --version 2>&1 | head -1) |"
    echo "| python3 | $($PY --version 2>&1 | head -1) |"
    echo "| cmake | $(cmake --version 2>&1 | head -1) |"
    echo "| git | $(git --version 2>&1) |"
    echo "| OS | $(uname -srm) |"
    echo ""
    if [ "$FAILED" -ne 0 ]; then
        echo "## Why it failed"
        echo ""
        echo "Stage \`$FAILED_STAGE\`. Last 30 lines of \`$(basename "${FAILED_LOG:-}")\`:"
        echo ""
        echo '```'
        [ -n "$FAILED_LOG" ] && tail -n 30 "$FAILED_LOG"
        echo '```'
        echo ""
    fi
    echo "## What this run does not prove"
    echo ""
    echo "* No MSVC, no Windows API linking, no Inno Setup compiler - that is the Windows job."
    echo "* No USB hardware and no drivers: device sharing/attachment stays unverified (docs/TESTING.md)."
    echo ""
} >"$REPORT"

if [ -n "${GITHUB_STEP_SUMMARY:-}" ]; then
    cat "$REPORT" >>"$GITHUB_STEP_SUMMARY"
fi

echo ""
echo "report: $REPORT"
if [ "$FAILED" -ne 0 ]; then
    echo "pipeline FAILED ($FAILED stage(s))"
    exit 1
fi
echo "pipeline PASSED"
exit 0
