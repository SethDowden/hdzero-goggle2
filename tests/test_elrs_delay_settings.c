#include <assert.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <minIni.h>
#include "core/settings.h"
#include "ui/page_common.h"

// Only hardware-dependent startup side effects are stubbed. Read and write the
// real INI format and run the entire production settings loader in a temp dir.
bool fs_file_exists(const char *filename) { return false; }
bool language_config(void) { return false; }
bool log_file_open(const char *filename) { assert(false); return false; }

static void check_load(int expected) {
    memset(&g_setting, 0xff, sizeof(g_setting));
    settings_load();
    assert(g_setting.elrs.analog_delay_ms == expected);
    assert(g_setting.elrs.enable); // Existing Backpack choice must survive.
    assert(g_setting.source.analog_channel == 35);
    assert(g_setting.fans.top_speed == 7);
}

int main(void) {
    assert(g_setting_defaults.elrs.analog_delay_ms == 1500);
    assert(settings_put_bool("elrs", "enable", true));
    assert(ini_putl("source", "analog_channel", 35, SETTING_INI));
    assert(ini_putl("fans", "top_speed", 7, SETTING_INI));
    // Each historical setting upgrades to the requested 1.5 s initial hold.
    for (int old = 0; old <= 2; old++) {
        assert(settings_put_bool("elrs", "analog_delay", old != 0));
        assert(ini_putl("elrs", "analog_delay_ms", old == 1 ? 500 : 1000, SETTING_INI));
        check_load(1500);
    }
    // Every menu value, including zero, survives a fresh settings load twice.
    for (int ms = 0; ms <= 5000; ms += 100) {
        assert(ini_putl("elrs", "analog_hold_ms", ms, SETTING_INI));
        check_load(ms);
        check_load(ms);
    }
    const long raw[] = {LONG_MIN, -1, 1, 99, 1499, 1599, 4999, 5001, LONG_MAX};
    const int normalized[] = {0, 0, 0, 0, 1400, 1500, 4900, 5000, 5000};
    for (unsigned int i = 0; i < sizeof(raw) / sizeof(raw[0]); i++) {
        assert(ini_putl("elrs", "analog_hold_ms", raw[i], SETTING_INI));
        check_load(normalized[i]);
    }
    puts("PASS: real INI migration to 1.5 s, all 51 saved choices/reloads, range normalization and preserved unrelated settings");
}
