#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#include "driver/beep.h"
#include "core/defines.h"

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cond = PTHREAD_COND_INITIALIZER;
static int on_count, off_count, release_count, sleep_count;
static unsigned int durations[4];

static struct timespec deadline(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_REALTIME, &t) == 0);
    t.tv_sec += 2;
    return t;
}

bool gpio_set(int port, bool value) {
    assert(port == GPIO_BEEP);
    pthread_mutex_lock(&mutex);
    if (value) on_count++;
    else off_count++;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
    return true;
}

int beep_test_usleep(useconds_t us) {
    pthread_mutex_lock(&mutex);
    assert(sleep_count < 4);
    durations[sleep_count++] = us;
    pthread_cond_broadcast(&cond);
    struct timespec t = deadline();
    while (release_count < sleep_count)
        assert(pthread_cond_timedwait(&cond, &mutex, &t) == 0);
    pthread_mutex_unlock(&mutex);
    return 0;
}

int log_printf(const char *file, const char *func, int line, const int level, const char *fmt, ...) { return 0; }

static void wait_for(int *counter, int expected) {
    pthread_mutex_lock(&mutex);
    struct timespec t = deadline();
    while (*counter < expected)
        assert(pthread_cond_timedwait(&cond, &mutex, &t) == 0);
    pthread_mutex_unlock(&mutex);
}

int main(void) {
    // Deliberately queue before the worker exists: a signal alone is lost.
    beep_dur(50);
    beep_init();
    wait_for(&sleep_count, 1);
    assert(on_count == 1 && off_count == 0 && durations[0] == 50000);

    // Requests while sounding must survive until the worker can play them.
    // Preserve the existing latest-request behavior instead of replaying a burst.
    beep_dur(100);
    beep_dur(200);
    pthread_mutex_lock(&mutex);
    release_count = 1;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
    wait_for(&sleep_count, 2);
    assert(on_count == 2 && off_count == 1 && durations[1] == 200000);
    pthread_mutex_lock(&mutex);
    release_count = 2;
    pthread_cond_broadcast(&cond);
    pthread_mutex_unlock(&mutex);
    wait_for(&off_count, 2);
    puts("PASS: real beeper retains requests before worker wait and during active tone; latest pending tone wins");
}
