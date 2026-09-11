#ifdef __APPLE__
#include <libkern/OSByteOrder.h>
#include <semaphore.h>
#include <time.h>
// The unused Linux await-response function is removed by dead stripping.
int sem_timedwait(sem_t *sem, const struct timespec *deadline);
#define le16toh(x) OSSwapLittleToHostInt16(x)
#define htole16(x) OSSwapHostToLittleInt16(x)
#endif
