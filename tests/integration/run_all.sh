#!/usr/bin/env bash
# Copyright (C) 2024 Hathor Contributors
# SPDX-License-Identifier: GPL-3.0-or-later
#
# run_all.sh — Integration test runner for Hathor Phase 1
#
# Executes all integration test scripts in sequence and reports a combined
# pass/fail summary.
#
# Usage:
#   ./run_all.sh <hathor_binary> [<samples_path>]
#   ./run_all.sh /path/to/build/hathor /path/to/samples
#
# When invoked by CTest, the first argument is set to the hathor binary path
# via COMMAND bash run_all.sh $<TARGET_FILE:hathor> in CMakeLists.txt.
#
# Exit codes:
#   0  — all tests passed
#   1  — one or more tests failed

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HATHOR_BIN="${1:-}"
SAMPLES_PATH="${2:-}"

# ---------------------------------------------------------------------------
# Validate arguments
# ---------------------------------------------------------------------------
if [[ -z "$HATHOR_BIN" ]]; then
    echo "[FAIL] Usage: $0 <hathor_binary> [<samples_path>]" >&2
    exit 1
fi

if [[ ! -x "$HATHOR_BIN" ]]; then
    echo "[FAIL] hathor binary not found or not executable: $HATHOR_BIN" >&2
    exit 1
fi

if [[ -z "$SAMPLES_PATH" ]]; then
    # Default: two levels up from tests/integration/ → repo root/samples
    SAMPLES_PATH="$(cd "$SCRIPT_DIR/../.." && pwd)/samples"
fi

if [[ ! -d "$SAMPLES_PATH" ]]; then
    echo "[FAIL] samples directory not found: $SAMPLES_PATH" >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# List of test scripts (in order)
# ---------------------------------------------------------------------------
TESTS=(
    "test_startup.sh"
    "test_latency.sh"
    "test_protocol.sh"
)

PASS_COUNT=0
FAIL_COUNT=0
declare -A RESULTS

echo "============================================="
echo "  Hathor Integration Tests"
echo "  Binary:  $HATHOR_BIN"
echo "  Samples: $SAMPLES_PATH"
echo "============================================="
echo ""

# ---------------------------------------------------------------------------
# Run each test
# ---------------------------------------------------------------------------
for test_script in "${TESTS[@]}"; do
    test_path="$SCRIPT_DIR/$test_script"

    if [[ ! -f "$test_path" ]]; then
        echo "[SKIP] $test_script (file not found)" >&2
        RESULTS["$test_script"]="SKIP"
        (( FAIL_COUNT++ )) || true
        continue
    fi

    echo "--- Running: $test_script ---"
    if bash "$test_path" "$HATHOR_BIN" "$SAMPLES_PATH"; then
        RESULTS["$test_script"]="PASS"
        (( PASS_COUNT++ )) || true
    else
        RESULTS["$test_script"]="FAIL"
        (( FAIL_COUNT++ )) || true
    fi
    echo ""
done

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
TOTAL=$(( PASS_COUNT + FAIL_COUNT ))
echo "============================================="
echo "  Summary: ${PASS_COUNT}/${TOTAL} tests passed"
echo "---------------------------------------------"
for test_script in "${TESTS[@]}"; do
    status="${RESULTS[$test_script]:-SKIP}"
    printf "  %-35s %s\n" "$test_script" "$status"
done
echo "============================================="

if [[ "$FAIL_COUNT" -eq 0 ]]; then
    echo ""
    echo "[PASS] All integration tests passed"
    exit 0
else
    echo ""
    echo "[FAIL] ${FAIL_COUNT} test(s) failed" >&2
    exit 1
fi
