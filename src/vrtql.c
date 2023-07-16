#include <time.h>

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <pthread.h>
#endif

#if defined(__windows__)
#include <windows.h>
#endif

#include <assert.h>
#include <string.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdarg.h>

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>

#include "vrtql.h"

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

// Function to handle the submission of an error by default
static int vrtql_error_default_submit(int code, cstr message, ...);

// Function to clear an error by default
static void vrtql_error_clear_default();

// Function to process an error by default
static int vrtql_error_default_process(int code, cstr message);

//------------------------------------------------------------------------------
// Tracing
//------------------------------------------------------------------------------

// Color codes for log message highlighting
#define ANSI_COLOR_RESET   "\x1b[0m"
#define ANSI_COLOR_RED     "\x1b[31m"
#define ANSI_COLOR_GREEN   "\x1b[32m"
#define ANSI_COLOR_YELLOW  "\x1b[33m"
#define ANSI_COLOR_BLUE    "\x1b[34m"
#define ANSI_COLOR_MAGENTA "\x1b[35m"
#define ANSI_COLOR_CYAN    "\x1b[36m"
#define ANSI_COLOR_WHITE   "\x1b[37m"

#ifdef __windows__
// Windows Mutex for thread-safe logging
HANDLE log_mutex;
#else
// POSIX Mutex for thread-safe logging
pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

typedef struct
{
    const char* color;
    const char* level;
} log_level_info;

const log_level_info log_level_infos[VL_LEVEL_COUNT] =
{
    { ANSI_COLOR_WHITE,    "DEBUG"   },
    { ANSI_COLOR_BLUE,     "INFO"    },
    { ANSI_COLOR_MAGENTA,  "WARNING" },
    { ANSI_COLOR_RED,      "ERROR"   }
};

static void unlock_mutex(void* arg)
{
    pthread_mutex_unlock((pthread_mutex_t*)arg);
}

void vrtql_trace(vrtql_log_level level, const char* format, ...)
{
    if (level < 0 || level >= VL_LEVEL_COUNT)
    {
        vrtql_error_default_submit(VE_WARN, "Invalid log level");
        return;
    }

    time_t raw_time;
    struct tm time_info;
    char stamp[20];

    time(&raw_time);

#ifdef __windows__
    // Windows implementation using localtime_s
    if (localtime_s(&time_info, &raw_time) != 0)
    {
        vrtql_error_default_submit(VE_SYS, "localtime_s failed");
        return;
    }
#else
    // Non-Windows implementation using localtime_r
    if (localtime_r(&raw_time, &time_info) == NULL)
    {
        vrtql_error_default_submit(VE_SYS, "localtime_r failed");
        return;
    }
#endif

    if (strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &time_info) == 0)
    {
        vrtql_error_default_submit(VE_SYS, "strftime returned 0");
        return;
    }

    const char* color_code = log_level_infos[level].color;
    const char* level_name = log_level_infos[level].level;

#if defined(__windows__)

    DWORD tid = GetCurrentThreadId();

    // Windows implementation using Windows API for thread synchronization
    if (WaitForSingleObject(log_mutex, INFINITE) != WAIT_OBJECT_0)
    {
        vrtql_error_default_submit(VE_SYS, "WaitForSingleObject failed");
        return;
    }

#else

    pthread_t tid = pthread_self();

    if (pthread_mutex_lock(&log_mutex) != 0)
    {
        vrtql_error_default_submit(VE_SYS, "pthread_mutex_lock failed");
        return;
    }

    pthread_cleanup_push(unlock_mutex, &log_mutex);

#endif

    fprintf(stderr, "%s[%s] [%lu] [%s]%s ",
            color_code,
            stamp,
#if defined(__windows__)
            tid,
#else
            (unsigned long)tid,
#endif
            level_name,
            ANSI_COLOR_RESET);

    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);

    fprintf(stderr, "\n");
    fflush(stderr);

#if defined(__windows__)

    // Windows implementation using Windows API for thread synchronization
    if (!ReleaseMutex(log_mutex))
    {
        vrtql_error_default_submit(VE_SYS, "ReleaseMutex failed");
    }

#else

    pthread_cleanup_pop(1);

#endif
}

//------------------------------------------------------------------------------
// Memory allocation
//------------------------------------------------------------------------------

void* vrtql_malloc(size_t size)
{
    void* ptr = malloc(size);

    if (ptr == NULL)
    {
        return vrtql.malloc_error(size);
    }

    return ptr;
}

void* vrtql_malloc_error(size_t size)
{
    // No error string since memory allocation has already failed and error
    // handler uses malloc() for copying error string.
    vrtql.error(VE_MEM, NULL);

    // Default does not provide any recovery attempt.
    return NULL;
}

void* vrtql_calloc(size_t nmemb, size_t size)
{
    void* ptr = calloc(nmemb, size);

    if (ptr == NULL)
    {
        return vrtql.calloc_error(nmemb, size);
    }

    return ptr;
}

void* vrtql_calloc_error(size_t nmemb, size_t size)
{
    // No error string since memory allocation has already failed and error
    // handler uses malloc() for copying error string.
    vrtql.error(VE_MEM, NULL);

    // Default does not provide any recovery attempt.
    return NULL;
}

void* vrtql_realloc(void* ptr, size_t size)
{
    ptr = realloc(ptr, size);

    if (ptr == NULL)
    {
        return vrtql.realloc_error(ptr, size);
    }

    return ptr;
}

void* vrtql_realloc_error(void* ptr, size_t size)
{
    // No error string since memory allocation has already failed and error
    // handler uses malloc() for copying error string.
    vrtql.error(VE_MEM, NULL);

    // Default does not provide any recovery attempt.
    return NULL;
}

//------------------------------------------------------------------------------
// Error handling
//------------------------------------------------------------------------------

// Sets the last error for the current thread
void vrtql_set_error(vrtql_error_code code, const char* message)
{
    if (vrtql.e.text != NULL)
    {
        free(vrtql.e.text);
        vrtql.e.text = NULL;
    }

    vrtql.e.code = code;

    if (message != NULL)
    {
        vrtql.e.text = strdup(message);
    }
}

// Get the error value for the current thread
vrtql_error_value vrtql_get_error()
{
    return vrtql.e;
}

int vrtql_error_default_submit(int code, cstr format, ...)
{
    va_list args;
    va_start(args, format);

    // Determine the length of the formatted string
    int length = vsnprintf(NULL, 0, format, args);

    // Allocate a buffer for the formatted string
    char* buffer = malloc(length + 1);

    // Format the string into the buffer
    vsnprintf(buffer, length + 1, format, args);

    // Set
    vrtql_set_error(code, buffer);

    // Process
    vrtql.process_error(code, buffer);

    // Print the formatted string to stderr
    fprintf(stderr, "%s", buffer);

    // Cleanup
    free(buffer);
    va_end(args);

    return 0;
}

int vrtql_error_default_process(int code, cstr message)
{
    if (vrtql.trace == 1)
    {
        switch (code)
        {
            case VE_TIMEOUT:
            case VE_WARN:
            {
                vrtql_trace(VL_WARN, "Error %i: %s", code, message);
                break;
            }

            case VE_SYS:
            case VE_RT:
            {
                vrtql_trace(VL_INFO, "Error %i: %s", code, message);
                break;
            }

            case VE_MEM:
            case VE_FATAL:
            {
                vrtql_trace(VL_ERROR, "Error %i: %s", code, message);
                break;
            }

            default:
            {
                vrtql_trace(VL_INFO, "No error");
            }
        }
    }

    switch (code)
    {
        case VE_MEM:
        {
            fprintf(stderr, "Out of memory error\n");
            break;
        }

        case VE_FATAL:
        {
            fprintf(stderr, "Fatal error\n");
            exit(1);
        }

        default:
        {
            if (message != NULL)
            {
                fprintf(stderr, "Error %i: %s\n", code, message);
            }
        }
    }

    return 0;
}

void vrtql_error_clear_default()
{
    vrtql_set_error(VE_SUCCESS, NULL);
}

void vrtql_error_success_default()
{
    vrtql_set_error(VE_SUCCESS, NULL);
}

// Initialization of the vrtql environment. The environment is initialized with
// default error handling functions and the trace flag is turned off
__thread vrtql_env vrtql =
{
    .malloc        = vrtql_malloc,
    .malloc_error  = vrtql_malloc_error,
    .calloc        = vrtql_calloc,
    .calloc_error  = vrtql_calloc_error,
    .realloc       = vrtql_realloc,
    .realloc_error = vrtql_realloc_error,
    .error         = vrtql_error_default_submit,
    .process_error = vrtql_error_default_process,
    .clear_error   = vrtql_error_clear_default,
    .success       = vrtql_error_success_default,
    .e             = {.code=VE_SUCCESS, .text=NULL},
    .trace         = false,
    .state         = 0
};

//------------------------------------------------------------------------------
// Buffer
//------------------------------------------------------------------------------

vrtql_buffer* vrtql_buffer_new()
{
    vrtql_buffer* buffer = vrtql.malloc(sizeof(vrtql_buffer));

    buffer->data      = NULL;
    buffer->allocated = 0;
    buffer->size      = 0;

    return buffer;
}

void vrtql_buffer_clear(vrtql_buffer* buffer)
{
    if (buffer != NULL)
    {
        if (buffer->data != NULL)
        {
            free(buffer->data);
        }

        buffer->data      = NULL;
        buffer->allocated = 0;
        buffer->size      = 0;
    }
}

void vrtql_buffer_free(vrtql_buffer* buffer)
{
    if (buffer != NULL)
    {
        vrtql_buffer_clear(buffer);
        free(buffer);
    }
}

void vrtql_buffer_printf(vrtql_buffer* buffer, cstr format, ...)
{
    va_list args;
    va_start(args, format);

    // Determine the length of the formatted string
    int length = vsnprintf(NULL, 0, format, args);

    // Allocate a buffer for the formatted string
    char* data = malloc(length + 1);

    // Format the string into the buffer
    vsnprintf(data, length + 1, format, args);

    vrtql_buffer_append(buffer, data, length);

    // Cleanup
    free(data);
    va_end(args);
}

void vrtql_buffer_append(vrtql_buffer* buffer, ucstr data, size_t size)
{
    if (buffer == NULL || data == NULL)
    {
        return;
    }

    size_t total_size = buffer->size + size;

    if (total_size > buffer->allocated)
    {
        buffer->allocated = total_size * 1.5;

        ucstr mem;
        if (buffer->data == NULL)
        {
            mem = (ucstr)vrtql.malloc(buffer->allocated);
        }
        else
        {
            mem = (ucstr)vrtql.realloc(buffer->data, buffer->allocated);
        }

        buffer->data = mem;
    }

    memcpy(buffer->data + buffer->size, data, size);
    buffer->size = total_size;
}

void vrtql_buffer_drain(vrtql_buffer* buffer, size_t size)
{
    if (buffer == NULL || buffer->data == NULL)
    {
        return;
    }

    if (size >= buffer->size)
    {
        // When size >= buffer->size, clear the whole buffer
        vrtql_buffer_clear(buffer);
    }
    else
    {
        memmove(buffer->data, buffer->data + size, buffer->size - size);
        buffer->size -= size;
        buffer->data[buffer->size] = 0;
    }
}

//------------------------------------------------------------------------------
// Map
//------------------------------------------------------------------------------

cstr vrtql_map_get(struct sc_map_str* map, cstr key)
{
    // See if we have an existing entry
    cstr v = sc_map_get_str(map, key);

    if (sc_map_found(map) == false)
    {
        return NULL;
    }

    return v;
}

void vrtql_map_set(struct sc_map_str* map, cstr key, cstr value)
{
    // See if we have an existing entry
    sc_map_get_str(map, key);

    if (sc_map_found(map) == false)
    {
        // We don't. Therefore we need to allocate new key.
        key = strdup(key);
    }

    cstr v = sc_map_put_str(map, key, strdup(value));

    if (sc_map_found(map) == true)
    {
        free(v);
    }
}

void vrtql_map_remove(struct sc_map_str* map, cstr key)
{
    // See if we have an existing entry
    cstr v = sc_map_get_str(map, key);

    if (sc_map_found(map) == true)
    {
        free(v);
    }

    sc_map_del_str(map, key);
}

void vrtql_map_clear(struct sc_map_str* map)
{
    cstr key; cstr value;
    sc_map_foreach(map, key, value)
    {
        free(key);
        free(value);
    }

    sc_map_clear_str(map);
}

//------------------------------------------------------------------------------
// UUID
//------------------------------------------------------------------------------

char* vrtql_generate_uuid()
{
    unsigned char uuid[16];

    if (RAND_bytes(uuid, sizeof(uuid)) != 1)
    {
        return NULL;
    }

    // Set the version (4) and variant bits
    uuid[6] = (uuid[6] & 0x0F) | 0x40;
    uuid[8] = (uuid[8] & 0x3F) | 0x80;

    // Format the UUID as a string
    char* encoded_uuid = vrtql_base64_encode(uuid, sizeof(uuid));

    if (encoded_uuid == NULL)
    {
        return NULL;
    }

    // Remove padding characters and dashes
    size_t encoded_uuid_length = strlen(encoded_uuid);
    for (size_t i = 0; i < encoded_uuid_length; i++)
    {
        if ( encoded_uuid[i] == '='  ||
             encoded_uuid[i] == '\n' ||
             encoded_uuid[i] == '\r' ||
             encoded_uuid[i] == '-' )
        {
            encoded_uuid[i] = '_';
        }
    }

    return encoded_uuid;
}

//------------------------------------------------------------------------------
// Base 64
//------------------------------------------------------------------------------

char* vrtql_base64_encode(const unsigned char* data, size_t length)
{
    BIO* bio    = BIO_new(BIO_s_mem());
    BIO* base64 = BIO_new(BIO_f_base64());

    BIO_set_flags(base64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(base64, bio);
    BIO_write(base64, data, length);
    BIO_flush(base64);

    BUF_MEM* ptr;
    BIO_get_mem_ptr(base64, &ptr);
    size_t output_length = ptr->length;
    char* encoded_data   = (char*)vrtql.malloc(output_length + 1);

    memcpy(encoded_data, ptr->data, output_length);
    encoded_data[output_length] = '\0';

    BIO_free_all(base64);

    return encoded_data;
}

unsigned char* vrtql_base64_decode(const char* data, size_t* size)
{
    BIO* bio    = BIO_new_mem_buf(data, -1);
    BIO* base64 = BIO_new(BIO_f_base64());

    BIO_set_flags(base64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(base64, bio);

    // Determine the size of the decoded data
    size_t encoded_length = strlen(data);
    size_t decoded_length = (encoded_length * 3) / 4;  // Rough estimate
    unsigned char* decoded_data = (unsigned char*)vrtql.malloc(decoded_length);

    // Decode the base64 data
    *size = BIO_read(base64, decoded_data, encoded_length);

    BIO_free_all(base64);

    return decoded_data;
}

//------------------------------------------------------------------------------
// Utilities
//------------------------------------------------------------------------------

uint8_t vrtql_is_flag(const uint64_t* flags, uint64_t flag)
{
    return (*flags & flag) == flag;
}

void vrtql_set_flag(uint64_t* flags, uint64_t flag)
{
    *flags |= flag;
}

void vrtql_clear_flag(uint64_t* flags, uint64_t flag)
{
    *flags &= ~flag;
}

//------------------------------------------------------------------------------
// URL Parsing
//------------------------------------------------------------------------------

static char* url_extract_part(const char* url, const char* sep, char** rest)
{
    const char* part_end = strstr(url, sep);
    if (part_end == NULL)
    {
        return NULL;
    }

    size_t part_length = part_end - url;
    char* part = vrtql.malloc(part_length + 1);

    strncpy(part, url, part_length);
    part[part_length] = '\0';

    *rest = strdup(part_end + strlen(sep));
    if (*rest == NULL)
    {
        vrtql.error(VE_MEM, "strdup()");
        free(part);
        return NULL;
    }

    return part;
}

#if defined(__windows__)
char* strndup(const char* s, size_t n)
{
    size_t len = strlen(s);
    if (n < len)
    {
        len = n;
    }

    char* new_str = (char*)vrtql.malloc(len + 1);

    if (new_str != NULL)
    {
        memcpy(new_str, s, len);
        new_str[len] = '\0';
    }

    return new_str;
}
#endif

static char* vrtql_url_extract_port(char** host)
{
    char* port_start = strchr(*host, ':');
    if (port_start == NULL)
    {
        return NULL;
    }

    char* port = strdup(port_start + 1);
    if (port == NULL)
    {
        vrtql.error(VE_MEM, "strdup()");
        return NULL;
    }

    char* truncated_host = strndup(*host, port_start - *host);
    if (truncated_host == NULL)
    {
        vrtql.error(VE_MEM, "strndup()");
        free(port);
        return NULL;
    }

    free(*host);
    *host = truncated_host;

    return port;
}

vrtql_url vrtql_url_parse(const char* url)
{
    vrtql_url parts = { NULL, NULL, NULL, NULL, NULL, NULL };

    // Extract the scheme

    parts.scheme = url_extract_part(url, "://", (char**)&url);

    if (parts.scheme == NULL)
    {
        url = strdup(url);

        if (url == NULL)
        {
            vrtql.error(VE_MEM, "strdup()");
        }
    }

    // Extract the fragment

    char* rest = NULL;
    parts.fragment = url_extract_part(url, "#", &rest);

    if (parts.fragment != NULL)
    {
        free(url);
        url = rest;
    }
    else if (rest != NULL)
    {
        free(rest);
    }

    // Extract the query

    rest = NULL;
    parts.query = url_extract_part(url, "?", &rest);

    if (parts.query != NULL)
    {
        free(url);
        url = rest;
    }
    else if (rest != NULL)
    {
        free(rest);
    }

    // The remaining URL is the host and path

    rest = NULL;
    parts.host = url_extract_part(url, "/", &rest);

    if (parts.host != NULL)
    {
        free(url);
        url  = rest;
        rest = NULL;
    }
    else
    {
        if (parts.host == NULL)
        {
            vrtql.error(VE_MEM, "strdup()");
        }

        parts.host = strdup(url);
        free(url);

        if (rest != NULL)
        {
            free(rest);
        }
    }

    // The rest of the URL is the path. Add leading slash manually.
    size_t path_len = strlen(url);

    // Allocate memory for '/' + url + '\0'
    parts.path = (char*)vrtql.malloc((path_len + 2) * sizeof(char));

    if (url[0] != '/')
    {
        parts.path[0] = '/';
        strcpy(parts.path + 1, url);
    }
    else
    {
        strcpy(parts.path, url);
    }

    free(url);

    // Extract the port from the host
    parts.port = vrtql_url_extract_port(&parts.host);

    return parts;
}

vrtql_url vrtql_url_new()
{
    vrtql_url url;

    url.scheme   = NULL;
    url.host     = NULL;
    url.port     = NULL;
    url.path     = NULL;
    url.query    = NULL;
    url.fragment = NULL;

    return url;
}

void vrtql_url_free(vrtql_url url)
{
    if (url.scheme != NULL)
    {
        free(url.scheme);
        url.scheme = NULL;
    }

    if (url.host != NULL)
    {
        free(url.host);
        url.host = NULL;
    }

    if (url.port != NULL)
    {
        free(url.port);
        url.port = NULL;
    }

    if (url.path != NULL)
    {
        free(url.path);
        url.path = NULL;
    }

    if (url.query != NULL)
    {
        free(url.query);
        url.query = NULL;
    }

    if (url.fragment != NULL)
    {
        free(url.fragment);
        url.fragment = NULL;
    }
}

char* vrtql_url_build(const vrtql_url* parts)
{
    // Calculate the total length needed for the URL
    size_t total_length = 1; // for null-terminating character '\0'

    if (parts->scheme != NULL)
    {
        total_length += strlen(parts->scheme) + 3; // "://"
    }

    if (parts->host != NULL)
    {
        total_length += strlen(parts->host);
    }

    if (parts->port != NULL)
    {
        total_length += strlen(parts->port) + 1; // ":"
    }

    if (parts->path != NULL)
    {
        total_length += strlen(parts->path);
    }

    if (parts->query != NULL)
    {
        total_length += strlen(parts->query) + 1; // "?"
    }

    if (parts->fragment != NULL)
    {
        total_length += strlen(parts->fragment) + 1; // "#"
    }

    // Allocate memory for the URL string
    char* url = (char*)vrtql.malloc(total_length);

    url[0] = '\0'; // Start with an empty string

    // Use snprintf to append to the string, which is safer and potentially
    // faster than strcat

    if (parts->scheme != NULL)
    {
        snprintf(url + strlen(url), total_length, "%s://", parts->scheme);
    }

    if (parts->host != NULL)
    {
        snprintf(url + strlen(url), total_length, "%s", parts->host);
    }

    if (parts->port != NULL)
    {
        snprintf(url + strlen(url), total_length, ":%s", parts->port);
    }

    if (parts->path != NULL)
    {
        snprintf(url + strlen(url), total_length, "%s", parts->path);
    }

    if (parts->query != NULL)
    {
        snprintf(url + strlen(url), total_length, "?%s", parts->query);
    }

    if (parts->fragment != NULL)
    {
        snprintf(url + strlen(url), total_length, "#%s", parts->fragment);
    }

    return url;
}
