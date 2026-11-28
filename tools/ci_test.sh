#!/usr/bin/env bash
# CI test wrapper: runs the ctest command given as arguments, keeps the full log, and on failure
# publishes the failing tests and their failure lines (unit-test checks, CMake script errors) as
# GitHub annotations, which are visible on the run page without opening the log.
set -o pipefail
log=test_output.log
"$@" --output-on-failure 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
if [ "$status" -ne 0 ]; then
  grep -E "^\s+[0-9]+ - .*\((Failed|Timeout|Not Run|SEGFAULT|Subprocess aborted|Exception)" "$log" \
    | sed -e 's/%/%25/g' -e 's/^ */::error title=Failed test::/'
  grep -E "FAILED |\[FAIL\]|CMake Error|FATAL|error:|Error:|Assertion|Segmentation" "$log" \
    | awk '!seen[$0]++' | head -40 \
    | sed -e 's/%/%25/g' -e 's/^/::error title=Test failure::/'
  echo "::error title=Tests::ctest failed with exit code $status (failures above)"
fi
exit "$status"
