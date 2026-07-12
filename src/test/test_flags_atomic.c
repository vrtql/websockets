// test_flags_atomic.c — the vws flag helpers must be atomic: flag words are
// shared across threads (a server's reactor and its worker pool both
// set/clear/read connection-state words), and a plain read-modify-write
// loses updates under concurrent set/clear — a bit set by one thread can be
// wiped by another thread's stale write-back.
//
// Functional witness: one word carries an untouched WITNESS bit while two
// threads hammer set/clear on their own distinct bits. With atomic helpers
// the witness bit provably survives and each thread's final state is exact;
// with the old plain RMW the witness can be clobbered (probabilistic). The
// deterministic race gate is an external ThreadSanitizer build of this same
// drive (the helpers compiled with -fsanitize=thread report the data race
// on the old implementation and are clean on the atomic one).

#include <pthread.h>
#include <stdio.h>
#include <stdint.h>

#include "vws.h"

#define WITNESS (1ULL << 0)
#define BIT_A   (1ULL << 7)
#define BIT_B   (1ULL << 8)

static const int N = 500000;

static uint64_t g_flags = 0;

static void* drive_a(void* x)
{
    (void)x;

    for (int i = 0; i < N; i++)
    {
        vws_set_flag(&g_flags, BIT_A);
        vws_clear_flag(&g_flags, BIT_A);
    }

    vws_set_flag(&g_flags, BIT_A);   /* final state: SET */

    return NULL;
}

static void* drive_b(void* x)
{
    (void)x;

    for (int i = 0; i < N; i++)
    {
        vws_set_flag(&g_flags, BIT_B);
        vws_clear_flag(&g_flags, BIT_B);
    }

    return NULL;                     /* final state: CLEAR */
}

int main(void)
{
    int failures = 0;

    /* Single-threaded semantics. */
    vws_set_flag(&g_flags, WITNESS);

    if (vws_is_flag(&g_flags, WITNESS) != 1)
    {
        printf("FAIL: set/is round-trip\n");
        failures++;
    }

    /* Two threads hammer their own bits; the witness must survive. */
    pthread_t a, b;
    pthread_create(&a, NULL, drive_a, NULL);
    pthread_create(&b, NULL, drive_b, NULL);
    pthread_join(a, NULL);
    pthread_join(b, NULL);

    if (vws_is_flag(&g_flags, WITNESS) != 1)
    {
        printf("FAIL: witness bit clobbered by concurrent RMW\n");
        failures++;
    }

    if (vws_is_flag(&g_flags, BIT_A) != 1)
    {
        printf("FAIL: thread A's final set lost\n");
        failures++;
    }

    if (vws_is_flag(&g_flags, BIT_B) != 0)
    {
        printf("FAIL: thread B's final clear lost\n");
        failures++;
    }

    /* Clear semantics. */
    vws_clear_flag(&g_flags, WITNESS);

    if (vws_is_flag(&g_flags, WITNESS) != 0)
    {
        printf("FAIL: clear/is round-trip\n");
        failures++;
    }

    if (failures == 0)
    {
        printf("PASS: flag helpers atomic under concurrent set/clear\n");
        return 0;
    }

    return 1;
}
