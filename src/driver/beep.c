#include "beep.h"

#include <pthread.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

#include <log/log.h>

#include "core/defines.h"
#include "gpio.h"

static pthread_t beep_thread_handle;
static pthread_mutex_t beep_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t beep_cond = PTHREAD_COND_INITIALIZER;
static int beep_dur_ms = 0;
static uint64_t beep_requested_ms;

static void *beep_thread(void *);

static uint64_t beep_time_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    return (uint64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

void beep_init() {
    pthread_create(&beep_thread_handle, NULL, beep_thread, NULL);
}

void beep_dur(int ms) {
    if (ms <= 0)
        return;
    pthread_mutex_lock(&beep_mutex);
    beep_dur_ms = ms;
    beep_requested_ms = beep_time_ms();
    pthread_cond_signal(&beep_cond);
    pthread_mutex_unlock(&beep_mutex);
}

static void *beep_thread(void *arg) {
    while (1) {
        pthread_mutex_lock(&beep_mutex);
        // A request may arrive before the worker waits or during another tone.
        // The pending value is the predicate; a condition signal is not a queue.
        while (beep_dur_ms <= 0)
            pthread_cond_wait(&beep_cond, &beep_mutex);
        int this_beep_ms = beep_dur_ms;
        uint64_t requested_ms = beep_requested_ms;
        beep_dur_ms = 0;
        pthread_mutex_unlock(&beep_mutex);

        if (this_beep_ms > 0) {
            bool asserted = gpio_set(GPIO_BEEP, 1);
            uint64_t started_ms = beep_time_ms();
            LOGI("BEEP GPIO high write=%s at %llu ms; requested at %llu ms; duration %d ms",
                 asserted ? "ok" : "failed", (unsigned long long)started_ms,
                 (unsigned long long)requested_ms, this_beep_ms);
            usleep(this_beep_ms * 1000);
            bool cleared = gpio_set(GPIO_BEEP, 0);
            LOGI("BEEP GPIO low write=%s at %llu ms", cleared ? "ok" : "failed",
                 (unsigned long long)beep_time_ms());
        }
    }
}
