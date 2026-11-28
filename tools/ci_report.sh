#!/usr/bin/env bash
# CI helper: runs a command (the runtime tests), then publishes the result as GitHub Actions
# annotations, so the summary, the OpenGL renderer, every failed check and (with CI_GDB=1 on Linux)
# a crash backtrace are visible on the public run page (logs and artifacts need a signed-in account).
# Exits with the command's exit code.
#
#   tools/ci_report.sh <run-dir> <command...>
#   CI_XVFB=1      run the command inside xvfb-run (Linux runners without a display)
#   CI_GDB=1       run the command under gdb and publish the backtrace if it crashes
#   CI_NO_GL_OK=1  a runner that cannot create any OpenGL context (macOS VMs without a GPU) is reported
#                  as a warning ("runtime tests not run: no OpenGL context") instead of a failure;
#                  every other outcome, including failed checks, still fails the job
run_dir="$1"; shift
report="$run_dir/autotest_report.txt"
log="$run_dir/engine.log"
bt="$run_dir/gdb_backtrace.txt"
mkdir -p "$run_dir"
prefix=()
if [ "${CI_XVFB:-0}" = "1" ]; then prefix=(xvfb-run -a -s "-screen 0 1280x800x24"); fi
if [ "${CI_GDB:-0}" = "1" ] && command -v gdb > /dev/null; then
  "${prefix[@]}" gdb -q -batch -return-child-result -ex run -ex "thread apply all bt 25" --args "$@" > "$bt" 2>&1
  code=$?
  tail -n 40 "$bt"
else
  "${prefix[@]}" "$@"
  code=$?
fi
# GitHub shows at most 10 error annotations per step: the crash backtrace goes first.
if [ "$code" -ne 0 ] && [ -f "$bt" ]; then
  grep -E "received signal|terminate called|what\(\)|Assertion|^#[0-9]+ " "$bt" | grep -vE "^#[0-9]+ +0x[0-9a-f]+ in (raise|abort|__pthread_kill|pthread_kill|gsignal|__GI_)" | head -8 | sed 's/^/::error title=gdb::/'
fi
if [ -f "$log" ]; then
  grep -m1 -E "Platform: window" "$log" | sed 's/^/::notice title=OpenGL::/'
  grep -m1 -E "Renderer: OpenGL" "$log" | sed 's/^/::notice title=GPU::/'
fi
if [ -f "$report" ]; then
  echo "::notice title=Runtime tests (exit $code)::$(sed -n 2p "$report")"
  grep -E "^[[:space:]]+FAIL" "$report" | head -45 | sed 's/^[[:space:]]*/::error title=Runtime check::/'
elif [ "${CI_NO_GL_OK:-0}" = "1" ] && [ -f "$log" ] && grep -q "could not create an OpenGL" "$log"; then
  echo "::warning title=Runtime tests not run (UNVERIFIED)::this runner cannot create an OpenGL context: $(grep -m1 'GLFW error' "$log" | sed 's/.*GLFW error/GLFW error/')"
  exit 0
else
  echo "::error title=Runtime tests (exit $code)::no report written; last log lines follow"
  if [ -f "$log" ]; then tail -n 2 "$log" | sed 's/^/::error title=engine.log::/'; fi
fi
exit $code
