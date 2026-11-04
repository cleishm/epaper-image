# ESP-IDF test app

This app runs the host Unity suite on ESP32 targets, normally under QEMU, to
check that results are byte-identical across architectures (docs/design.md,
"Fixed-point arithmetic and determinism"). It compiles the host test files
where they are (`../test_*.c`, with the list in `../test_list.c`, shared with
`../test_runner.c`). It uses the repository root as the `epaper-image` component
(`EXTRA_COMPONENT_DIRS`), so it also tests the root CMakeLists.txt's
`ESP_PLATFORM` branch.

Differences from the host build:

- `EPI_TEST_EXHAUSTIVE=0`: the 2^31-input cbrt monotonicity test is ignored.
- Assertions are on, and there is no `fork()`, so
  `test_converter_drain_violation` and `test_preview_drain_violation` are
  ignored (their death-test branches need fork, and their NDEBUG branches need
  a release build of the library).
- `-O2` (`CONFIG_COMPILER_OPTIMIZATION_PERF`), full newlib printf, 64-bit
  Unity assertions, no task watchdog, 32 KB main task stack.

Expect the same output as the host for every determinism check: the nine
`fingerprint <name> 0x...` lines (test_math), the `fingerprint colour tables` line
(test_colour_panel), the `intent fingerprint` line,
and PASS for `test_converter_goldens`, `test_grey_goldens` and
`test_resampler_goldens` (these print hashes only on mismatch). The
`differs from libm` / `differ` counts in the cbrt_d, sqrt_d, exp_d and sin tests
compare against the target's own libm, so they may differ from the host, and
that is fine.

## Build and run

This uses ESP-IDF v6.1. After activation, `idf.py` is a shell alias, so scripts
call `python "$IDF_PATH/tools/idf.py"`. From this directory:

```sh
source ~/.espressif/tools/activate_idf_v6.1.sh
idf() { python "$IDF_PATH/tools/idf.py" "$@"; }

for t in esp32 esp32c3; do
    idf -B build_$t -D SDKCONFIG=build_$t/sdkconfig set-target $t
    idf -B build_$t -D SDKCONFIG=build_$t/sdkconfig build
done

./run_qemu.sh esp32      # Xtensa; log in build_esp32/qemu.log
./run_qemu.sh esp32c3    # RISC-V; log in build_esp32c3/qemu.log
```

`run_qemu.sh <target> [timeout_s]` runs `idf.py qemu` (which also rebuilds
if sources changed) and captures the serial console to
`build_<target>/qemu.log`. It waits for the app's `EPI_TESTS_DONE` line, a
panic or the timeout (default 3 h), then stops QEMU. It exits 0 when no
test failed. If the activation script did not put `qemu-system-xtensa` /
`qemu-system-riscv32` on PATH (it only does so for the exact version
tools.json recommends), the script uses the newest build installed under
`$IDF_TOOLS_PATH/qemu-*`. The two targets can run in parallel.

To run interactively, use
`idf -B build_esp32 -D SDKCONFIG=build_esp32/sdkconfig qemu monitor`
(exit with Ctrl-]).

These summary lines are worth comparing with the host run
(`./build/tests/epaper_image_tests` from the repository root):

```sh
grep -aE 'fingerprint|golden|:FAIL|:IGNORE|Tests|EPI_TESTS_DONE' build_esp32/qemu.log
```

## On hardware (ESP32-S3)

`sdkconfig.defaults.esp32s3` adds the USB-Serial-JTAG console, so the suite
runs on a board whose only port is the native USB (such as the XIAO EE05):

```sh
idf -B build_esp32s3 -D SDKCONFIG=build_esp32s3/sdkconfig set-target esp32s3
idf -B build_esp32s3 -D SDKCONFIG=build_esp32s3/sdkconfig build
idf -B build_esp32s3 -D SDKCONFIG=build_esp32s3/sdkconfig -p PORT flash monitor
```

With the USB-Serial-JTAG console, the app waits 2 s before the first test, so
the host can reopen the port after the reset. Compare the summary lines
with the host as above.

## Sizes

```sh
idf -B build_esp32 -D SDKCONFIG=build_esp32/sdkconfig size
idf -B build_esp32 -D SDKCONFIG=build_esp32/sdkconfig size-components   # libepaper-image.a line
```
