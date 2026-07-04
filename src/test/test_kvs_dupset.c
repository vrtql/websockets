// Regression guard — vws_kvs_set must be idempotent.
//
// vws_kvs_set used to append a new entry on every call with no dedup, but the
// map is single-valued (get/remove use bsearch). So re-setting a key left two
// entries: get() returned an arbitrary one and remove()/clear() deleted only
// one, so a re-set key could not be cleared. The fix replaces the value in
// place when the key already exists.
//
// Three checks:
//   1. idempotent set + clear (the original defect, deterministic).
//   2. control: distinct keys still insert-sort correctly (ordering preserved).
//   3. wire path: a deserialized message with duplicate keys yields a single
//      last-wins entry.
//
// On buggy source checks 1 and 3 FAIL; on fixed source all pass (exit 0).

#include <stdio.h>
#include <string.h>
#include "message.h"

static int fails = 0;

static void check(int cond, const char* msg)
{
    if (!cond) { printf("FAIL: %s\n", msg); fails++; }
}

int main(void)
{
    // 1. Idempotent set + full clear.
    vrtql_msg* m = vrtql_msg_new();
    vrtql_msg_set_header(m, "id", "first");
    vrtql_msg_set_header(m, "id", "second");
    check(m->headers->used == 1, "re-set of a header must not duplicate (used==1)");
    const char* got = vrtql_msg_get_header(m, "id");
    check(got != NULL && strcmp(got, "second") == 0, "get must return the last write");
    vrtql_msg_clear_header(m, "id");
    check(vrtql_msg_get_header(m, "id") == NULL, "clear must fully strip the key");

    // 2. Control: distinct keys inserted out of order must stay sorted and
    //    each resolve to its own value (the fix must not break insert-sort).
    vrtql_msg_set_routing(m, "gamma", "3");
    vrtql_msg_set_routing(m, "alpha", "1");
    vrtql_msg_set_routing(m, "beta",  "2");
    check(m->routing->used == 3, "three distinct routing keys → used==3");
    check(strcmp(vrtql_msg_get_routing(m, "alpha"), "1") == 0, "alpha resolves");
    check(strcmp(vrtql_msg_get_routing(m, "beta"),  "2") == 0, "beta resolves");
    check(strcmp(vrtql_msg_get_routing(m, "gamma"), "3") == 0, "gamma resolves");
    int sorted = 1;
    for (size_t i = 1; i < m->routing->used; i++)
    {
        if (m->routing->cmp(&m->routing->array[i-1], &m->routing->array[i]) > 0)
        {
            sorted = 0;
        }
    }
    check(sorted, "routing array stays key-sorted after out-of-order inserts");
    vrtql_msg_free(m);

    // 3. Wire path: deserialize a JSON message whose routing object has a
    //    duplicate key. The map must hold ONE last-wins entry.
    vrtql_msg* w = vrtql_msg_new();
    const char* json = "[{\"k\":\"v1\",\"k\":\"v2\"},{},\"\"]";
    bool ok = vrtql_msg_deserialize(w, (ucstr)json, strlen(json));
    check(ok, "dup-key JSON message deserializes");
    check(w->routing->used == 1, "duplicate wire key collapses to one entry");
    const char* wk = vrtql_msg_get_routing(w, "k");
    check(wk != NULL && strcmp(wk, "v2") == 0, "duplicate wire key is last-wins (v2)");
    vrtql_msg_free(w);

    if (fails == 0) { printf("PASS: vws_kvs_set idempotent; ordering + wire dup-key correct\n"); return 0; }
    return 1;
}
