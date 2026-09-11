#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "core/elrs.h"
#include "core/app_state.h"
#include "core/common.hh"
#include "core/settings.h"
#include "core/battery.h"
#include "core/dvr.h"
#include "driver/esp32.h"
#include "driver/dm5680.h"
#include "driver/hardware.h"
#include "driver/i2c.h"
#include "driver/rtc.h"
#include "driver/rtc6715.h"
#include "ui/page_common.h"
#include "ui/page_version.h"

setting_t g_setting;
source_info_t g_source_info;
hw_status_t g_hw_stat;
sys_battery_t g_battery;
rx_status_t rx_status[2];
pthread_mutex_t lvgl_mutex = PTHREAD_MUTEX_INITIALIZER;
atomic_int g_init_done = 1;
int GOGGLE_VER_2 = 1;
bool dvr_is_recording;
static int beeps, dvr_stops, source_inits, power_calls, system_calls, writes, hd_switches;
static unsigned int sleep_seconds, sleep_us;
static uint8_t fpga_regs[256], response[32];
static uint32_t tuner_regs[16];
static int response_len;
static uint64_t now_ms = 10000, last_beep_ms, last_tune_ms;
static bool clock_failed;

// Only elrs.c is compiled with clock_gettime redirected to this fixture.
int retune_test_clock_gettime(clockid_t clock_id, struct timespec *ts) {
    if (clock_id != CLOCK_MONOTONIC)
        return clock_gettime(clock_id, ts);
    if (clock_failed)
        return -1;
    ts->tv_sec = now_ms / 1000;
    ts->tv_nsec = (now_ms % 1000) * 1000000;
    return 0;
}

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) { return 0; }
void beep_dur(int ms) { beeps++; last_beep_ms = now_ms; }
void dvr_cmd(osd_dvr_cmd_t cmd) { assert(cmd == DVR_STOP); dvr_stops++; dvr_is_recording = false; }
void app_switch_to_hdzero(bool is_default) { assert(is_default); hd_switches++; }
void ht_set_center_position(void) {}
void rtc_set_clock(const struct rtc_date *rd) {}
void osd_signal_update(void) {}
int generate_current_version(sys_version_t *ver) { return -1; }
int uart_write(int fd, uint8_t *data, int len) { assert(len <= sizeof(response)); memcpy(response, data, len); response_len = len; return len; }
unsigned int sleep(unsigned int seconds) { sleep_seconds += seconds; return 0; }
int usleep(useconds_t usec) { sleep_us += usec; return 0; }
int system_exec(const char *cmd) { system_calls++; return 0; }
int system_script(const char *cmd) { system_calls++; return 0; }
void Analog_Module_Power(bool force, bool on) { assert(on); power_calls++; }
void DM5680_InternalAnalog_Power(uint8_t on) { assert(on); power_calls++; }
void gpadc_on(uint8_t on) { assert(on); }
void Source_AV(uint8_t sel) { assert(sel == 1); source_inits++; g_hw_stat.source_mode = SOURCE_MODE_AV; g_hw_stat.av_chid = sel; }
void dvr_update_vi_conf(video_resolution_t fmt) { assert(fmt == VR_720P50); }
void osd_fhd(uint8_t on) { assert(!on); }
void osd_show(bool on) { assert(on); }
int lvgl_switch_to_720p(void) { return 0; }
void osd_clear(void) {}
uint32_t lv_timer_handler(void) { return 0; }
void Display_Osd(bool on) {}
int ini_putl(const char *section, const char *key, long value, const char *file) { return 1; }
void dvr_select_audio_source(uint8_t source) {}
void dvr_enable_line_out(bool on) { assert(on); }
int i2c_write(int port, uint8_t slave, uint8_t addr, uint8_t val) {
    assert(port == 2 && slave == ADDR_FPGA);
    writes++;
    fpga_regs[addr] = val;
    if (addr == 0xb0) {
        assert(val == 1 && fpga_regs[0xb1] < 16);
        tuner_regs[fpga_regs[0xb1]] = fpga_regs[0xb2] | (uint32_t)fpga_regs[0xb3] << 8 | (uint32_t)fpga_regs[0xb4] << 16;
        if (fpga_regs[0xb1] == 1)
            last_tune_ms = now_ms;
    }
    return 1;
}

static void reset(void) {
    elrs_cancel_analog_retune();
    memset(&g_setting, 0, sizeof(g_setting));
    memset(&g_hw_stat, 0, sizeof(g_hw_stat));
    memset(tuner_regs, 0, sizeof(tuner_regs));
    GOGGLE_VER_2 = 1;
    g_init_done = 1;
    g_app_state = APP_STATE_VIDEO;
    g_source_info.source = SOURCE_ANALOG;
    g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_INTERNAL;
    g_setting.source.analog_channel = 33;
    g_setting.scan.channel = 7;
    g_setting.elrs.enable = true;
    g_hw_stat.source_mode = SOURCE_MODE_AV;
    g_hw_stat.av_chid = 1;
    beeps = dvr_stops = source_inits = power_calls = system_calls = writes = hd_switches = 0;
    sleep_seconds = sleep_us = 0;
    dvr_is_recording = true;
    now_ms = 10000;
    last_beep_ms = last_tune_ms = 0;
    clock_failed = false;
}

static void send_command(uint16_t function, uint16_t value, int len) {
    uint8_t frame[] = {'$', 'X', '<', 0, function & 255, function >> 8, len, 0, value & 255, value >> 8, 0};
    uint8_t crc = 0;
    for (int i = 3; i < 8 + len; i++) {
        crc ^= frame[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc & 128) ? (crc << 1) ^ 0xd5 : crc << 1;
    }
    frame[8 + len] = crc;
    for (int i = 0; i < 9 + len; i++) esp32_handler_process_byte(frame[i]);
}
static void quiet_live(int expected_channel, int expected_writes) {
    assert(g_setting.source.analog_channel == expected_channel);
    assert(g_setting.scan.channel == 7);
    assert(beeps == 0 && dvr_stops == 0 && source_inits == 0 && power_calls == 0 && system_calls == 0);
    assert(writes == expected_writes && sleep_seconds == 0 && sleep_us == (expected_writes / 10) * 20000);
    assert(dvr_is_recording && g_app_state == APP_STATE_VIDEO);
}
static void slow_path(void) {
    assert(source_inits == 1 && power_calls == 2 && dvr_stops == 1);
    assert(sleep_seconds == 1 && sleep_us == 230000);
    assert(beeps == 0 && g_app_state == APP_STATE_VIDEO);
    assert(g_setting.source.analog_channel == 34 && tuner_regs[1] == 0x2890);
}

static pthread_mutex_t command_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t command_cond = PTHREAD_COND_INITIALIZER;
static bool command_started;
static void *concurrent_command(void *unused) {
    pthread_mutex_lock(&command_mutex);
    command_started = true;
    pthread_cond_signal(&command_cond);
    pthread_mutex_unlock(&command_mutex);
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    return NULL;
}

static void poll_at(uint64_t ms) {
    now_ms = ms;
    pthread_mutex_lock(&lvgl_mutex);
    elrs_poll_analog_retune();
    pthread_mutex_unlock(&lvgl_mutex);
}

static void delayed_live(int channel, int expected_writes, int expected_beeps) {
    assert(beeps == expected_beeps);
    int saved_beeps = beeps;
    beeps = 0;
    quiet_live(channel, expected_writes);
    beeps = saved_beeps;
}

static void test_delayed_retune(const uint16_t *frequencies) {
    // Run every channel and frequency through the actual MSP parser and tuner.
    for (int kind = 0; kind < 2; kind++) {
        for (int ch = 0; ch < 48; ch++) {
            reset();
            g_setting.elrs.analog_delay = true;
            int target = kind && ch == 38 ? 31 : ch; // 5880 -> first match F8
            int function = kind ? MSP_SET_FREQ : MSP_SET_BAND_CHAN;
            int value = kind ? frequencies[ch] : ch;
            int len = kind ? 2 : 1;
            bool changed = target != 32;
            send_command(function, value, len);
            delayed_live(33, 0, changed);
            if (changed)
                assert(last_beep_ms == 10000);
            const int retries[] = {1, 100, 250, 500, 750, 999};
            for (unsigned int r = 0; r < sizeof(retries) / sizeof(retries[0]); r++) {
                poll_at(10000 + retries[r]);
                send_command(function, value, len);
                delayed_live(33, 0, changed);
                send_command(MSP_GET_BAND_CHAN, 0, 0);
                assert(response[8] == 32); // Active channel, not staged target.
                send_command(MSP_GET_FREQ, 0, 0);
                assert((response[8] | response[9] << 8) == 5658);
            }
            poll_at(11000);
            delayed_live(target + 1, changed ? 10 : 0, changed);
            if (changed)
                assert(last_tune_ms - last_beep_ms == 1000);
            send_command(MSP_GET_BAND_CHAN, 0, 0);
            assert(response[8] == target);
            send_command(function, value, len);
            poll_at(12000);
            delayed_live(target + 1, changed ? 10 : 0, changed);
        }
    }

    // A genuinely different target replaces the pending selection and deadline.
    reset();
    g_setting.elrs.analog_delay = true;
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    poll_at(10600);
    send_command(MSP_SET_BAND_CHAN, 34, 1);
    poll_at(11000);
    delayed_live(33, 0, 2);
    poll_at(11599);
    delayed_live(33, 0, 2);
    poll_at(11600);
    delayed_live(35, 10, 2);

    // Returning to the currently active channel cancels a queued transition.
    reset();
    g_setting.elrs.analog_delay = true;
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    poll_at(10500);
    send_command(MSP_SET_BAND_CHAN, 32, 1);
    poll_at(12000);
    delayed_live(33, 0, 2);

    // Lifecycle transitions cancel even if analog video is restored before poll.
    for (int kind = 0; kind < 8; kind++) {
        reset();
        g_setting.elrs.analog_delay = true;
        send_command(MSP_SET_BAND_CHAN, 33, 1);
        if (kind == 0) { app_state_push(APP_STATE_MAINMENU); app_state_push(APP_STATE_VIDEO); }
        if (kind == 1) { app_state_push(APP_STATE_SLEEP); app_state_push(APP_STATE_VIDEO); }
        if (kind == 2) g_source_info.source = SOURCE_HDMI_IN;
        if (kind == 3) g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
        if (kind == 4) g_setting.elrs.enable = false;
        if (kind == 5) g_setting.elrs.analog_delay = false;
        if (kind == 6) g_init_done = 0;
        if (kind == 7) g_hw_stat.av_chid = 0;
        poll_at(11000);
        assert(writes == 0 && beeps == 1 && g_setting.source.analog_channel == 33);
    }

    // Manual tuning away and back also invalidates a pending request.
    reset();
    g_setting.elrs.analog_delay = true;
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    RTC6715_SetCH(34);
    RTC6715_SetCH(32);
    poll_at(12000);
    delayed_live(33, 20, 1);

    // Power reinitialization invalidates old work.
    reset();
    g_setting.elrs.analog_delay = true;
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    RTC6715_Open(1, 0);
    int prior_writes = writes;
    poll_at(12000);
    assert(writes == prior_writes && g_setting.source.analog_channel == 33);

    // No clock, no premature tuning; RTC/wall-clock time is never consulted.
    reset();
    g_setting.elrs.analog_delay = true;
    clock_failed = true;
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    poll_at(12000);
    delayed_live(33, 0, 0);
    clock_failed = false;
    now_ms = UINT64_C(4294967290); // Cross a 32-bit millisecond wrap.
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    poll_at(UINT64_C(4294968289));
    delayed_live(33, 0, 1);
    poll_at(UINT64_C(4294968290));
    delayed_live(34, 10, 1);

    // Invalid/short commands do not beep or disturb valid pending work.
    reset();
    g_setting.elrs.analog_delay = true;
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    send_command(MSP_SET_BAND_CHAN, 48, 1);
    send_command(MSP_SET_BAND_CHAN, 255, 1);
    send_command(MSP_SET_FREQ, 5695, 1);
    send_command(MSP_SET_FREQ, 5500, 2);
    poll_at(11000);
    delayed_live(34, 10, 1);

    // Startup/menu requests retain the established full initialization path.
    reset();
    g_setting.elrs.analog_delay = true;
    app_state_push(APP_STATE_MAINMENU);
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    assert(beeps == 1);
    beeps = 0;
    slow_path();
    prior_writes = writes;
    poll_at(12000);
    assert(writes == prior_writes);

    puts("PASS: delayed 48-channel/48-frequency transitions, receipt beeps, 1 s deadlines, retries, truthful readbacks, replacement/cancellation, lifecycle/manual tuning, clock failure/wrap and initialization");
}

int main(void) {
    elrs_init();
    for (int ch = 0; ch < 48; ch++) {
        reset();
        send_command(MSP_SET_BAND_CHAN, ch, 1);
        quiet_live(ch + 1, ch == 32 ? 0 : 10);
        send_command(MSP_SET_BAND_CHAN, ch, 1);
        quiet_live(ch + 1, ch == 32 ? 0 : 10);
        send_command(MSP_GET_BAND_CHAN, 0, 0);
        assert(response_len == 10 && response[8] == ch);
    }
    const uint16_t frequencies[] = {
        5865,5845,5825,5805,5785,5765,5745,5725,5733,5752,5771,5790,5809,5828,5847,5866,
        5705,5685,5665,5645,5885,5905,5925,5945,5740,5760,5780,5800,5820,5840,5860,5880,
        5658,5695,5732,5769,5806,5843,5880,5917,5362,5399,5436,5473,5510,5547,5584,5621
    };
    for (int i = 0; i < 48; i++) {
        reset();
        int expected = i == 38 ? 32 : i + 1;
        send_command(MSP_SET_FREQ, frequencies[i], 2);
        quiet_live(expected, expected == 33 ? 0 : 10);
        send_command(MSP_SET_FREQ, frequencies[i], 2);
        quiet_live(expected, expected == 33 ? 0 : 10);
        send_command(MSP_GET_FREQ, 0, 0);
        assert(response_len == 11 && (response[8] | response[9] << 8) == frequencies[i]);
    }
    reset();
    send_command(MSP_SET_BAND_CHAN, 33, 1);
    assert(tuner_regs[0] == 8 && tuner_regs[1] == 0x2890);
    send_command(MSP_SET_FREQ, 5658, 2);
    assert(tuner_regs[1] == 0x281d);
    quiet_live(33, 20);
    reset();
    send_command(MSP_SET_BAND_CHAN, 48, 1);
    send_command(MSP_SET_BAND_CHAN, 255, 1);
    send_command(MSP_SET_BAND_CHAN, 1, 0);
    send_command(MSP_SET_FREQ, 5695, 1);
    send_command(MSP_SET_FREQ, 5500, 2);
    quiet_live(33, 0);
    for (int cmd = 0; cmd < 2; cmd++) {
        for (int state = 0; state < 6; state++) {
            reset();
            if (state == 0) g_init_done = 0;
            if (state == 1) g_app_state = APP_STATE_MAINMENU;
            if (state == 2) g_hw_stat.source_mode = SOURCE_MODE_UI;
            if (state == 3) g_hw_stat.av_chid = 0;
            if (state == 4) g_app_state = APP_STATE_SLEEP;
            if (state == 5) g_setting.source.analog_channel = 34; // same channel still needs initialization
            if (state == 5) g_app_state = APP_STATE_MAINMENU;
            send_command(cmd ? MSP_SET_FREQ : MSP_SET_BAND_CHAN, cmd ? 5695 : 33, cmd ? 2 : 1);
            slow_path();
        }
    }
    for (int inactive = 0; inactive < 4; inactive++) {
        reset();
        if (inactive == 0) GOGGLE_VER_2 = 0;
        if (inactive == 1) g_setting.source.analog_module = SETTING_SOURCES_ANALOG_MODULE_EXTERNAL;
        if (inactive == 2) g_source_info.source = SOURCE_HDMI_IN;
        if (inactive == 3) g_source_info.source = SOURCE_AV_IN;
        send_command(MSP_SET_BAND_CHAN, 33, 1);
        send_command(MSP_SET_FREQ, 5695, 2);
        quiet_live(33, 0);
    }
    reset();
    send_command(MSP_SET_BUZZER, 100, 2);
    assert(beeps == 1);
    reset();
    g_source_info.source = SOURCE_HDZERO;
    send_command(MSP_SET_BAND_CHAN, 32, 1);
    assert(hd_switches == 1 && dvr_stops == 1 && beeps == 0 && writes == 0);
    send_command(MSP_SET_FREQ, 5865, 2);
    assert(hd_switches == 2 && beeps == 0);
    reset();
    pthread_mutex_lock(&lvgl_mutex);
    pthread_t thread;
    assert(pthread_create(&thread, NULL, concurrent_command, NULL) == 0);
    pthread_mutex_lock(&command_mutex);
    while (!command_started)
        pthread_cond_wait(&command_cond, &command_mutex);
    pthread_mutex_unlock(&command_mutex);
    struct timespec delay = {0, 20000000};
    nanosleep(&delay, NULL);
    assert(g_setting.source.analog_channel == 33 && writes == 0);
    g_source_info.source = SOURCE_HDMI_IN;
    pthread_mutex_unlock(&lvgl_mutex);
    pthread_join(thread, NULL);
    quiet_live(33, 0);
    // Actual source-entry function remains unconditional, including from another source.
    reset();
    g_source_info.source = SOURCE_HDMI_IN;
    g_hw_stat.source_mode = SOURCE_MODE_HDMIIN;
    g_setting.source.analog_channel = 34;
    app_switch_to_analog();
    assert(source_inits == 1 && power_calls == 2 && sleep_seconds == 1 && sleep_us == 230000);
    assert(tuner_regs[1] == 0x2890);
    puts("PASS: 48 channels, 48 frequencies, repeats/readback, invalid commands, startup/menu/sleep fallback, inactive sources, buzzer, digital dispatch, concurrent source change, actual analog initialization and tuner writes");
    test_delayed_retune(frequencies);
}
