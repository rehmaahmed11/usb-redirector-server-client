#!/usr/bin/env bash
# Builds and runs the portable core tests plus the relay protocol tests.
# Works on Linux and inside CI; no Windows needed.
#   ./tools/test_core.sh
# SPDX-License-Identifier: MIT
set -euo pipefail
cd "$(dirname "$0")/.."

CXX=${CXX:-g++}
echo "== core tests ($($CXX --version | head -1))"
$CXX -std=c++17 -O2 -Wall -Wextra -Isrc \
    tests/test_core.cpp \
    src/core/base32.cpp src/core/logger.cpp src/core/pairing.cpp src/core/protocol.cpp \
    src/core/session_auth.cpp src/core/settings.cpp src/core/sha256.cpp src/core/strings.cpp \
    src/core/usbip_wire.cpp \
    -o /tmp/mub_test_core
/tmp/mub_test_core

echo
echo "== relay protocol tests"
$CXX -std=c++17 -O2 -pthread relay/relay.cpp -o /tmp/mub_relay_test
python3 tests/test_relay.py /tmp/mub_relay_test
