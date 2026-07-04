// repro — crash: vrtql_msg_deserialize (JSON path) on a non-string
// routing/header value.
//
// Wire-driven. The broker accepts JSON vrtql messages (auto-detected when the
// first byte is not the MessagePack magic 0x93). message.c parses the routing
// and header objects but never verifies that each *value* is a JSON string:
//
//   value = yyjson_obj_iter_get_val(key);
//   vws_kvs_set_cstring(msg->routing, yyjson_get_str(key), yyjson_get_str(value));
//
// For a non-string value (number/bool/null/obj/arr) yyjson_get_str(value)
// returns NULL, and vws_kvs_set_cstring -> vws_kvs_set -> strlen(NULL) faults.
// Contrast: the `content` element IS guarded by yyjson_is_str() (message.c:411).
//
// Expected on buggy source: SIGSEGV (exit via signal). A fixed source should
// reject the message and return false (clean exit 0).

#include <stdio.h>
#include "message.h"

int main(void)
{
    // Valid JSON, root array of size 3: [routing, headers, content].
    // routing has one entry whose value is a NUMBER, not a string.
    const char* payload = "[{\"key\":5},{},\"\"]";

    vrtql_msg* m = vrtql_msg_new();

    // On buggy source this crashes (strlen(NULL)); a fixed parser rejects the
    // message and returns false.
    bool ok = vrtql_msg_deserialize(m, (ucstr)payload, strlen(payload));

    vrtql_msg_free(m);

    if (ok)
    {
        // Did not crash, but accepted a message with a non-string routing
        // value — the guard is missing/wrong.
        printf("FAIL: deserialize accepted a non-string routing value\n");
        return 1;
    }

    printf("PASS: deserialize rejected the non-string routing value\n");
    return 0;
}
