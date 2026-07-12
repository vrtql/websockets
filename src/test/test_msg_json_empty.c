// repro — empty-content JSON message cannot round-trip: serialize emits a
// 2-element root, deserialize requires 3.
//
// message.c serialize (JSON arm) appends the content element to the root
// array only when content size > 0, so a message with NO content serializes
// to [routing, headers]. Deserialize hard-requires a root array of size 3
// and rejects the bytes ("Invalid JSON: Root is not an array of size 3")
// before routing or headers are ever read. Wire-reachable: any JSON-format
// control/signal message that carries all of its information in routing and
// headers is dropped by every receiver on this codec. MessagePack is
// unaffected.
//
// Expected on buggy source: the empty-content subject fails to deserialize
// (exit 1). Fixed source always emits the third element (empty string when
// there is no content), so the subject round-trips; the non-empty control
// round-trips on both.

#include <stdio.h>
#include <string.h>
#include "message.h"

// Serialize a JSON-format message carrying routing + header (+ content when
// non-empty), deserialize the produced bytes, and verify what comes back.
// Returns 0 on success.
static int round_trip(cstr content, cstr label)
{
    vrtql_msg* m = vrtql_msg_new();
    m->format    = VM_JSON_FORMAT;

    vrtql_msg_set_routing(m, "to", "svc");
    vrtql_msg_set_header(m, "type", "sig");

    if (content != NULL && strlen(content) > 0)
    {
        vrtql_msg_set_content(m, content);
    }

    vws_buffer* b = vrtql_msg_serialize(m);

    if (b == NULL)
    {
        printf("FAIL %s: serialize returned NULL\n", label);
        vrtql_msg_free(m);
        return 1;
    }

    vrtql_msg* d = vrtql_msg_new();
    bool ok      = vrtql_msg_deserialize(d, b->data, b->size);

    int rc = 0;

    if (ok == false)
    {
        printf("FAIL %s: deserialize rejected the serialized bytes\n", label);
        rc = 1;
    }
    else
    {
        cstr to = vrtql_msg_get_routing(d, "to");

        if (to == NULL || strcmp(to, "svc") != 0)
        {
            printf("FAIL %s: routing lost in the round-trip\n", label);
            rc = 1;
        }

        size_t size = vrtql_msg_get_content_size(d);

        if (content == NULL || strlen(content) == 0)
        {
            if (size != 0)
            {
                printf("FAIL %s: expected empty content, got %zu bytes\n",
                       label, size);
                rc = 1;
            }
        }
        else
        {
            cstr got = vrtql_msg_get_content(d);

            if (size != strlen(content) || got == NULL ||
                strncmp(got, content, size) != 0)
            {
                printf("FAIL %s: content lost in the round-trip\n", label);
                rc = 1;
            }
        }
    }

    vws_buffer_free(b);
    vrtql_msg_free(m);
    vrtql_msg_free(d);

    if (rc == 0)
    {
        printf("PASS %s\n", label);
    }

    return rc;
}

int main(void)
{
    int failures = 0;

    // Subject: no content — all information in routing/headers.
    failures += round_trip("", "empty-content");

    // Control: a normal message round-trips on buggy and fixed source alike.
    failures += round_trip("payload", "non-empty-content");

    return failures;
}
