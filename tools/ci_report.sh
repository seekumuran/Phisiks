#!/usr/bin/env bash
# CI helper: runs a command (the runtime tests), then publishes the result as GitHub Actions
# annotations, so the summary, the OpenGL renderer and every failed check are visible on the public
# run page (logs and artifacts need a signed-in account). Exits with the command's exit code.
#   tools/ci_report.sh <run-dir> <command...>
run_dir="$1"; shift
"$@"
code=$?
report="$run_dir/autotest_report.txt"
log="$run_dir/engine.log"
if [ -f "$log" ]; then
  grep -m1 -E "OpenGL .* context|Platform: window" "$log" | sed 's/^/::notice title=OpenGL::/'
  grep -m1 -E "Renderer: OpenGL" "$log" | sed 's/^/::notice title=GPU::/'
fi
if [ -f "$report" ]; then
  echo "::notice title=Runtime tests (exit $code)::$(sed -n 2p "$report")"
  grep -E "^[[:space:]]+FAIL" "$report" | head -45 | sed 's/^[[:space:]]*/::error title=Runtime check::/'
else
  echo "::error title=Runtime tests (exit $code)::no report written; last log lines follow"
  if [ -f "$log" ]; then tail -n 15 "$log" | sed 's/^/::error title=engine.log::/'; fi
fi
exit $code
