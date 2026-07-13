// test_address_pool_generation.c — a reused address-pool slot must not be
// reachable through a stale key.
//
// cid.key is the slot index the pool hands out, and the slot is released at
// svr_on_close while consumers (the broker's deferred connection teardown)
// may still hold the old key. address_pool_set is a cyclic first-fit scan,
// so the index is reissued to a new connection and the stale key then
// resolves the NEW resident: a lookup aliases a different connection, and a
// stale remove frees the new connection's slot.
//
// With per-slot generations the key is index | (generation << 32): a reused
// slot issues a DIFFERENT key, a stale get resolves nothing, and a stale
// remove is a no-op.
//
// Drive: capacity-4 pool, no resize (count stays < capacity). Fill A,B,C,D;
// remove A; the cyclic scan wraps and the next set (E) lands on A's slot.
//
//   1. E's key must differ from A's stale key
//   2. a get through A's stale key must resolve nothing (not E)
//   3. a remove through A's stale key must be a no-op — E must survive

#include <stdio.h>
#include <stdint.h>

#include "vws.h"
#include "server.h"

static int failures = 0;

static void check(int ok, const char* what)
{
    if (ok == 0)
    {
        printf("FAIL: %s\n", what);
        failures++;
    }
}

int main(int argc, const char* argv[])
{
    (void)argc;
    (void)argv;

    address_pool* pool = address_pool_new(4, 2);

    uintptr_t obj_a = (uintptr_t)0xA0;
    uintptr_t obj_b = (uintptr_t)0xB0;
    uintptr_t obj_c = (uintptr_t)0xC0;
    uintptr_t obj_d = (uintptr_t)0xD0;
    uintptr_t obj_e = (uintptr_t)0xE0;

    int64_t key_a = address_pool_set(pool, obj_a);
    int64_t key_b = address_pool_set(pool, obj_b);
    int64_t key_c = address_pool_set(pool, obj_c);
    int64_t key_d = address_pool_set(pool, obj_d);

    check(address_pool_get(pool, key_a) == obj_a, "A resolves before free");

    // A's transport slot is released; A's key lives on in a deferred
    // teardown. The cyclic scan reissues the slot to E.
    address_pool_remove(pool, key_a);

    int64_t key_e = address_pool_set(pool, obj_e);

    check((uint32_t)(key_e & 0xFFFFFFFF) == (uint32_t)(key_a & 0xFFFFFFFF),
          "drive is on-point: E reuses A's slot index");

    // 1. The reissued slot must carry a NEW identity.
    check(key_e != key_a, "reused slot issues a different key");

    // 2. The stale key must resolve NOTHING — not the new resident.
    check(address_pool_get(pool, key_a) == 0,
          "stale get resolves nothing (must not alias the new resident)");

    // 3. A stale remove must be a no-op: the new resident survives.
    address_pool_remove(pool, key_a);

    check(address_pool_get(pool, key_e) == obj_e,
          "stale remove is a no-op (new resident survives)");

    // Untouched keys keep resolving throughout.
    check(address_pool_get(pool, key_b) == obj_b, "B unaffected");
    check(address_pool_get(pool, key_c) == obj_c, "C unaffected");
    check(address_pool_get(pool, key_d) == obj_d, "D unaffected");

    // Keys stay non-negative (vws_cid_valid contract: key >= 0) across many
    // reuse cycles of one slot.
    int64_t key = key_e;
    for (int i = 0; i < 4096; i++)
    {
        address_pool_remove(pool, key);
        key = address_pool_set(pool, obj_e);

        if (key < 0)
        {
            break;
        }
    }

    check(key >= 0, "keys stay non-negative across reuse cycles");

    address_pool_free(&pool);

    if (failures == 0)
    {
        printf("PASS: reused slot is unreachable through a stale key\n");
        return 0;
    }

    return 1;
}
