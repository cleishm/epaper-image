#!/usr/bin/env bash
# Run the built test app under QEMU non-interactively and stop QEMU when the
# suite finishes (or panics, or times out). Requires an activated ESP-IDF
# environment (IDF_PATH set) and a prior build into build_<target>.
#
#   usage: run_qemu.sh <esp32|esp32s3|esp32c3> [timeout_seconds]
#
# The serial log goes to build_<target>/qemu.log. Exit status: 0 when the
# suite reports no failures, 1 on test failures, 2 on panic/timeout.
set -u
usage="usage: run_qemu.sh <esp32|esp32s3|esp32c3> [timeout_seconds]"
target=${1:?$usage}
timeout_s=${2:-7200}
dir=$(cd "$(dirname "$0")" && pwd)
build="$dir/build_$target"
log="$build/qemu.log"
: "${IDF_PATH:?activate ESP-IDF first}"

# The activation script puts QEMU on PATH only when the installed build is
# the exact version tools.json recommends; otherwise use the newest installed.
case $target in
esp32 | esp32s3) qemu_tool=qemu-xtensa qemu_prog=qemu-system-xtensa ;;
esp32c3) qemu_tool=qemu-riscv32 qemu_prog=qemu-system-riscv32 ;;
*)
    echo "$usage" >&2
    exit 2
    ;;
esac
if ! command -v "$qemu_prog" >/dev/null; then
    qemu_bin=$(ls -d "${IDF_TOOLS_PATH:-$HOME/.espressif/tools}/$qemu_tool"/*/qemu/bin 2>/dev/null | sort | tail -1)
    [ -n "$qemu_bin" ] || { echo "run_qemu: $qemu_prog not found" >&2; exit 2; }
    PATH="$qemu_bin:$PATH"
fi
echo "run_qemu: using $(command -v "$qemu_prog")"

set -m # own process group for QEMU, so it can be stopped as a whole
python "$IDF_PATH/tools/idf.py" -C "$dir" -B "$build" -D SDKCONFIG="$build/sdkconfig" qemu \
    >"$log" 2>&1 </dev/null &
pgid=$!
set +m

start=$(date +%s)
status=2
while :; do
    if grep -q '^EPI_TESTS_DONE fails=0 ' "$log" 2>/dev/null; then
        status=0
        break
    fi
    if grep -q '^EPI_TESTS_DONE ' "$log" 2>/dev/null; then
        status=1
        break
    fi
    if grep -qE 'Guru Meditation|abort\(\) was called|Rebooting\.\.\.' "$log" 2>/dev/null; then
        echo "run_qemu: panic (see $log)" >&2
        break
    fi
    if ! kill -0 "$pgid" 2>/dev/null; then
        echo "run_qemu: QEMU exited early (see $log)" >&2
        break
    fi
    if [ $(($(date +%s) - start)) -ge "$timeout_s" ]; then
        echo "run_qemu: timed out after ${timeout_s}s (see $log)" >&2
        break
    fi
    sleep 5
done
kill -TERM -- "-$pgid" 2>/dev/null
wait "$pgid" 2>/dev/null
echo "run_qemu: $target finished in $(($(date +%s) - start))s (wall), status $status; log $log"
grep -E '^EPI_TESTS_DONE|^[0-9]+ Tests ' "$log"
exit "$status"
