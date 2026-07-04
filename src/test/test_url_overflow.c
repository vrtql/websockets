// regression guard — heap buffer overflow in url_get_protocol.
//
// url.c allocates a fixed URL_PROTOCOL_MAX_LENGTH (32) byte buffer and filled
// it with an UNBOUNDED scanset: sscanf(url, "%[^://]", protocol). A scheme
// prefix longer than 31 chars overflowed the heap allocation (the overflow
// happened before url_is_protocol() could reject the bogus scheme). The fix
// width-bounds the scanset (%31[^://]).
//
// Build this test with -fsanitize=address and url.c compiled in-tree so the
// buffer is an instrumented allocation. On buggy source ASan reports a
// heap-buffer-overflow WRITE (non-zero exit); on fixed source the scanset is
// bounded, url_get_protocol rejects the bogus scheme and returns NULL, clean
// exit 0.

#include <stdio.h>
#include <string.h>
#include "url.h"

int main(void)
{
    // 200 non-':' non-'/' bytes: a scheme prefix far past the 32-byte buffer.
    char url[256];
    memset(url, 'a', 200);
    url[200] = '\0';

    char* proto = url_get_protocol(url);

    // Fixed source: the 200-char bogus scheme is truncated to 31 chars, fails
    // url_is_protocol(), so proto is NULL. Buggy source never reaches here
    // (ASan aborts on the overflow write).
    printf("PASS: url_get_protocol returned %s (no overflow)\n",
           proto ? proto : "(null)");

    free(proto);
    return 0;
}
