#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
test_build=$(mktemp -d)
trap 'rm -rf "$test_build"' EXIT HUP INT TERM
cc=${CC:-cc}
flags='-D_GNU_SOURCE -std=gnu11 -g -O1 -ffunction-sections -fdata-sections -fsanitize=undefined -fno-omit-frame-pointer'
includes='-Ilib/esp-loader/include -Isrc -Isrc/core -Ilib/lvgl -Ilib/log/include -Ilib/minIni/src -include tests/host_compat.h'
# Keep the real analog initialization; only digital entry is stubbed.
$cc $flags $includes -Dapp_switch_to_hdzero=unused_app_switch_to_hdzero -c src/core/app_state.c -o "$test_build/app_state.o"
$cc $flags $includes -Dclock_gettime=retune_test_clock_gettime -c src/core/elrs.c -o "$test_build/elrs.o"
$cc $flags $includes -c src/driver/rtc6715.c -o "$test_build/rtc6715.o"
case $(uname -s) in
    Darwin) gc='-Wl,-dead_strip' ;;
    *) gc='-Wl,--gc-sections' ;;
esac
$cc $flags $includes tests/test_elrs_retune.c "$test_build/app_state.o" "$test_build/elrs.o" "$test_build/rtc6715.o" $gc -pthread -o "$test_build/test_elrs_retune"
UBSAN_OPTIONS=halt_on_error=1 "$test_build/test_elrs_retune"
