#!/usr/bin/env bash
# CI build wrapper: runs the build command given as arguments, keeps the full log, and on failure
# publishes the compiler / linker errors as GitHub annotations (visible without opening the log).
set -o pipefail
log=build_output.log
"$@" 2>&1 | tee "$log"
status=${PIPESTATUS[0]}
if [ "$status" -ne 0 ]; then
  grep -E "error[ :]|: fatal error|undefined reference|unresolved external|ld: |Error [0-9]+" "$log" \
    | grep -v "warning" | awk '!seen[$0]++' | head -40 \
    | sed -e 's/%/%25/g' -e 's/^/::error title=Build error::/'
  echo "::error title=Build::the build failed with exit code $status (first errors above)"
fi
exit "$status"
