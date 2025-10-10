#include <time.h>
#include <ctype.h>
#include <errno.h>

#if defined(__linux__) || defined(__bsd__) || defined(__sunos__)
#include <unistd.h>
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

#include "vws.h"

//------------------------------------------------------------------------------
// Utility functions
//------------------------------------------------------------------------------

// Function to handle the submission of an error by default
static int vws_error_default_submit(int code, cstr message, ...);

// Function to clear an error by default
static void vws_error_clear_default();

// Function to process an error by default
static int vws_error_default_process(int code, cstr message);

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
    { ANSI_COLOR_WHITE,   "DEBG" },
    { ANSI_COLOR_BLUE,    "INFO" },
    { ANSI_COLOR_MAGENTA, "WARN" },
    { ANSI_COLOR_RED,     "CRIT" }
};

static void unlock_mutex(void* arg)
{
    pthread_mutex_unlock((pthread_mutex_t*)arg);
}

void vws_trace_lock()
{
#if defined(__windows__)

    DWORD tid = GetCurrentThreadId();

    // Windows implementation using Windows API for thread synchronization
    if (WaitForSingleObject(log_mutex, INFINITE) != WAIT_OBJECT_0)
    {
        vws_error_default_submit(VE_SYS, "WaitForSingleObject failed");
        return;
    }

#else

    pthread_t tid = pthread_self();

    if (pthread_mutex_lock(&log_mutex) != 0)
    {
        vws_error_default_submit(VE_SYS, "pthread_mutex_lock failed");
        return;
    }

#endif
}

void vws_trace_unlock()
{
#if defined(__windows__)

    // Windows implementation using Windows API for thread synchronization
    if (!ReleaseMutex(log_mutex))
    {
        vws_error_default_submit(VE_SYS, "ReleaseMutex failed");
    }

#else

    if (pthread_mutex_unlock(&log_mutex) != 0)
    {
        vws_error_default_submit(VE_SYS, "pthread_mutex_unlock failed");
    }

#endif
}

void vws_trace(vws_log_level_t level, const char* format, ...)
{
    if (level < 0 || level >= VL_LEVEL_COUNT)
    {
        vws_error_default_submit(VE_WARN, "Invalid log level");
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
        vws_error_default_submit(VE_SYS, "localtime_s failed");
        return;
    }
#else
    // Non-Windows implementation using localtime_r
    if (localtime_r(&raw_time, &time_info) == NULL)
    {
        vws_error_default_submit(VE_SYS, "localtime_r failed");
        return;
    }
#endif

    if (strftime(stamp, sizeof(stamp), "%H:%M:%S", &time_info) == 0)
    {
        vws_error_default_submit(VE_SYS, "strftime returned 0");
        return;
    }

    const char* color_code = log_level_infos[level].color;
    const char* level_name = log_level_infos[level].level;

#if defined(__windows__)

    DWORD tid = GetCurrentThreadId();

    // Windows implementation using Windows API for thread synchronization
    if (WaitForSingleObject(log_mutex, INFINITE) != WAIT_OBJECT_0)
    {
        vws_error_default_submit(VE_SYS, "WaitForSingleObject failed");
        return;
    }

#else

    pthread_t tid = pthread_self();

    if (pthread_mutex_lock(&log_mutex) != 0)
    {
        vws_error_default_submit(VE_SYS, "pthread_mutex_lock failed");
        return;
    }

    pthread_cleanup_push(unlock_mutex, &log_mutex);

#endif

    fprintf(stderr, "%s%s.000 %#010lx [%s]%s ",
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
        vws_error_default_submit(VE_SYS, "ReleaseMutex failed");
    }

#else

    pthread_cleanup_pop(1);

#endif
}

//------------------------------------------------------------------------------
// Memory allocation
//------------------------------------------------------------------------------

void* vws_malloc(size_t size)
{
    void* ptr = malloc(size);

    if (ptr == NULL)
    {
        return vws.malloc_error(size);
    }

    return ptr;
}

void* vws_malloc_error(size_t size)
{
    // No error string since memory allocation has already failed and error
    // handler uses malloc() for copying error string.
    vws.error(VE_MEM, NULL);

    // Default does not provide any recovery attempt.
    return NULL;
}

void vws_free(void* data)
{
    free(data);
}

void* vws_calloc(size_t nmemb, size_t size)
{
    void* ptr = calloc(nmemb, size);

    if (ptr == NULL)
    {
        return vws.calloc_error(nmemb, size);
    }

    return ptr;
}

void* vws_calloc_error(size_t nmemb, size_t size)
{
    // No error string since memory allocation has already failed and error
    // handler uses malloc() for copying error string.
    vws.error(VE_MEM, NULL);

    // Default does not provide any recovery attempt.
    return NULL;
}

void* vws_realloc(void* ptr, size_t size)
{
    ptr = realloc(ptr, size);

    if (ptr == NULL)
    {
        return vws.realloc_error(ptr, size);
    }

    return ptr;
}

void* vws_realloc_error(void* ptr, size_t size)
{
    // No error string since memory allocation has already failed and error
    // handler uses malloc() for copying error string.
    vws.error(VE_MEM, NULL);

    // Default does not provide any recovery attempt.
    return NULL;
}

void* vws_strdup(cstr ptr)
{
    ptr = strdup(ptr);

    if (ptr == NULL)
    {
        return vws.strdup_error(ptr);
    }

    return ptr;
}

void* vws_strdup_error(cstr ptr)
{
    // No error string since memory allocation has already failed and error
    // handler uses malloc() for copying error string.
    vws.error(VE_MEM, NULL);

    // Default does not provide any recovery attempt.
    return NULL;
}

//------------------------------------------------------------------------------
// Error handling
//------------------------------------------------------------------------------

// Sets the last error for the current thread
void vws_set_error(vws_error_code_t code, const char* message)
{
    if (vws.e.text != NULL)
    {
        vws.free(vws.e.text);
        vws.e.text = NULL;
    }

    vws.e.code = code;

    if (message != NULL)
    {
        vws.e.text = strdup(message);
    }
}

// Get the error value for the current thread
vws_error_value vws_get_error()
{
    return vws.e;
}

int vws_error_default_submit(int code, cstr format, ...)
{
    va_list args, args_copy;
    va_start(args, format);

    va_copy(args_copy, args);

    // Determine the length of the formatted string
    int length = vsnprintf(NULL, 0, format, args_copy);

    va_end(args_copy);  // End args_copy. We're done with it now.

    // Allocate a buffer for the formatted string
    char* buffer = malloc(length + 1);

    // Format the string into the buffer
    vsnprintf(buffer, length + 1, format, args);

    // Set
    vws_set_error(code, buffer);

    // Process
    vws.process_error(code, buffer);

    // Cleanup
    vws.free(buffer);
    va_end(args);

    return 0;
}

int vws_error_default_process(int code, cstr message)
{
    if (vws.tracelevel >= 1)
    {
        switch (code)
        {
            case VE_WARN:
            {
                vws.trace(VL_WARN, "%s", message);
                break;
            }

            case VE_TIMEOUT:
            {
                vws.trace(VL_WARN, "timeout: %s", message);
                break;
            }

            case VE_SOCKET:
            {
                vws.trace(VL_WARN, "disconnect: %s", message);
                break;
            }

            case VE_SYS:
            case VE_RT:
            {
                vws.trace(VL_INFO, "error %i: %s", message);
                break;
            }

            case VE_MEM:
            case VE_FATAL:
            {
                vws.trace(VL_ERROR, "fatal %i: %s", code, message);
                break;
            }

            default:
            {
                vws.trace(VL_INFO, "no error");
            }
        }
    }

    switch (code)
    {
        case VE_MEM:
        {
            break;
        }

        case VE_FATAL:
        {
            exit(1);
        }

        default:
        {
            if (message != NULL)
            {

            }
        }
    }

    return 0;
}

void vws_error_clear_default()
{
    vws_set_error(VE_SUCCESS, NULL);
}

void vws_error_success_default()
{
    vws_set_error(VE_SUCCESS, NULL);
}

// Global SSL context
SSL_CTX* vws_ssl_ctx = NULL;

// Initialization of the vrtql environment. The environment is initialized with
// default error handling functions and the trace flag is turned off
__thread vws_env vws =
{
    .malloc        = vws_malloc,
    .malloc_error  = vws_malloc_error,
    .calloc        = vws_calloc,
    .calloc_error  = vws_calloc_error,
    .realloc       = vws_realloc,
    .realloc_error = vws_realloc_error,
    .strdup        = vws_strdup,
    .strdup_error  = vws_strdup_error,
    .free          = vws_free,
    .error         = vws_error_default_submit,
    .process_error = vws_error_default_process,
    .clear_error   = vws_error_clear_default,
    .success       = vws_error_success_default,
    .e             = {.code=VE_SUCCESS, .text=NULL},
    .trace         = vws_trace,
    .tracelevel    = 0,
    .state         = 0
};

void vws_cleanup()
{
    if (vws.e.text != NULL)
    {
        free(vws.e.text);
    }
}

//------------------------------------------------------------------------------
// Buffer
//------------------------------------------------------------------------------

vws_buffer* vws_buffer_new()
{
    vws_buffer* buffer = vws.malloc(sizeof(vws_buffer));

    buffer->data      = NULL;
    buffer->allocated = 0;
    buffer->size      = 0;

    return buffer;
}

void vws_buffer_clear(vws_buffer* buffer)
{
    if (buffer != NULL)
    {
        if (buffer->data != NULL)
        {
            vws.free(buffer->data);
        }

        buffer->data      = NULL;
        buffer->allocated = 0;
        buffer->size      = 0;
    }
}

void vws_buffer_free(vws_buffer* buffer)
{
    if (buffer != NULL)
    {
        vws_buffer_clear(buffer);
        vws.free(buffer);
    }
}

void vws_buffer_printf(vws_buffer* buffer, cstr format, ...)
{
    va_list args, args_copy;
    va_start(args, format);

    va_copy(args_copy, args);

    // Determine the length of the formatted string
    int length = vsnprintf(NULL, 0, format, args_copy);

    va_end(args_copy);

    // Allocate a buffer for the formatted string
    char* data = malloc(length + 1);

    // Format the string into the buffer
    vsnprintf(data, length + 1, format, args);

    vws_buffer_append(buffer, (ucstr)data, length);

    // Cleanup
    vws.free(data);
    va_end(args);
}

void vws_buffer_append(vws_buffer* buffer, ucstr data, size_t size)
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
            mem = (ucstr)vws.malloc(buffer->allocated);
        }
        else
        {
            mem = (ucstr)vws.realloc(buffer->data, buffer->allocated);
        }

        buffer->data = mem;
    }

    memcpy(buffer->data + buffer->size, data, size);
    buffer->size = total_size;
}

void vws_buffer_drain(vws_buffer* buffer, size_t size)
{
    if (buffer == NULL || buffer->data == NULL)
    {
        return;
    }

    if (size >= buffer->size)
    {
        // When size >= buffer->size, clear the whole buffer
        vws_buffer_clear(buffer);
    }
    else
    {
        memmove(buffer->data, buffer->data + size, buffer->size - size);
        buffer->size -= size;
        buffer->data[buffer->size] = 0;
    }
}

//------------------------------------------------------------------------------
// Hashtable
//------------------------------------------------------------------------------

cstr vws_map_get(struct sc_map_str* map, cstr key)
{
    // See if we have an existing entry
    cstr v = sc_map_get_str(map, key);

    if (sc_map_found(map) == false)
    {
        return NULL;
    }

    return v;
}

void vws_map_set(struct sc_map_str* map, cstr key, cstr value)
{
    sc_map_get_str(map, key);

    if (sc_map_found(map) == true)
    {
        // We have an existing entry
        vws_map_remove(map, key);
    }

    sc_map_put_str(map, strdup(key), strdup(value));
}

void vws_map_remove(struct sc_map_str* map, cstr key)
{
    // See if we have an existing entry
    cstr v = sc_map_get_str(map, key);

    if (sc_map_found(map) == true)
    {
        vws.free(v);
    }

    sc_map_del_str(map, key);
}

void vws_map_clear(struct sc_map_str* map)
{
    cstr key; cstr value;
    sc_map_foreach(map, key, value)
    {
        vws.free(key);
        vws.free(value);
    }

    sc_map_clear_str(map);
}

//------------------------------------------------------------------------------
// Key/value store
//------------------------------------------------------------------------------

int vws_kvs_cs_comp(const void* a, const void* b)
{
    return strcmp(((vws_kvp*)a)->key, ((vws_kvp*)b)->key);
}

int vws_kvs_ci_comp(const void* a, const void* b)
{
    return strcasecmp(((vws_kvp*)a)->key, ((vws_kvp*)b)->key);
}

vws_kvs* vws_kvs_new(size_t size, bool case_sensitive)
{
    vws_kvs* m = (vws_kvs*)malloc(sizeof(vws_kvs));

    if (m)
    {
        m->array = (vws_kvp*)malloc(size * sizeof(vws_kvp));
        m->used  = 0;
        m->size  = size;

        if (case_sensitive == true)
        {
            m->cmp = vws_kvs_cs_comp;
        }
        else
        {
            m->cmp = vws_kvs_ci_comp;
        }
    }

    return m;
}

void vws_kvs_clear(vws_kvs* m)
{
    for (size_t i = 0; i < m->used; i++)
    {
        free((void*)m->array[i].key); // Free copied key
        free(m->array[i].value.data);  // Free copied data
    }

    m->used = 0;
}

void vws_kvs_free(vws_kvs* m)
{
    vws_kvs_clear(m);

    free(m->array);
    free(m);
}

size_t vws_kvs_size(vws_kvs* m)
{
    return m->used;
}

void vws_kvs_set(vws_kvs* m, const char* key, void* data, size_t size)
{
    if (m->used == m->size)
    {
        m->size *= 2;
        m->array = (vws_kvp*)realloc(m->array, m->size * sizeof(vws_kvp));
    }

    // Copy the key
    char* key_copy = strdup(key);

    // Copy the data
    void* data_copy = malloc(size);
    memcpy(data_copy, data, size);

    vws_kvp kvp = {key_copy, {data_copy, size}};

    size_t i;
    for (i = m->used; i > 0 && m->cmp(&kvp, &m->array[i-1]) < 0; i--)
    {
        m->array[i] = m->array[i - 1];
    }

    m->array[i] = kvp;
    m->used++;
}

vws_value* vws_kvs_get(vws_kvs* m, const char* key)
{
    vws_kvp kvp = {key, {NULL, 0}};
    vws_kvp* result  = bsearch( &kvp,
                                m->array,
                                m->used,
                                sizeof(m->array[0]),
                                m->cmp );

    if (result != NULL)
    {
        return &result->value;
    }

    return NULL;
}

void vws_kvs_set_cstring(vws_kvs* m, const char* key, const char* value)
{
    vws_kvs_set(m, key, (void*)value, strlen(value) + 1);
}

const char* vws_kvs_get_cstring(vws_kvs* m, const char* key)
{
    vws_value* value = vws_kvs_get(m, key);

    if (value != NULL)
    {
        return (const char*)value->data;
    }

    return NULL;
}

int vws_kvs_remove(vws_kvs* m, const char* key)
{
    vws_kvp kvp     = {key, {NULL, 0}};
    vws_kvp* result = bsearch( &kvp,
                               m->array,
                               m->used,
                               sizeof(m->array[0]),
                               m->cmp );

    if (result != NULL)
    {
        size_t index = result - m->array;

        // Free the copied key and data
        free((void*)m->array[index].key);
        free(m->array[index].value.data);

        // Shift elements to maintain order
        if (index < m->used - 1)
        {
            memmove( &m->array[index],
                     &m->array[index + 1],
                     (m->used - index - 1) * sizeof(vws_kvp) );
        }

        m->used--;

        // Key was found and removed
        return 1;
    }

    // Key not found
    return 0;
}

//------------------------------------------------------------------------------
// UUID
//------------------------------------------------------------------------------

char* vws_generate_uuid()
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
    char* encoded_uuid = vws_base64_encode(uuid, sizeof(uuid));

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
             encoded_uuid[i] == '-'   )
        {
            encoded_uuid[i] = '_';
        }
    }

    return encoded_uuid;
}

//------------------------------------------------------------------------------
// Base 64
//------------------------------------------------------------------------------

char* vws_base64_encode(const unsigned char* data, size_t length)
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
    char* encoded_data   = (char*)vws.malloc(output_length + 1);

    memcpy(encoded_data, ptr->data, output_length);
    encoded_data[output_length] = '\0';

    BIO_free_all(base64);

    return encoded_data;
}

unsigned char* vws_base64_decode(const char* data, size_t* size)
{
    BIO* bio    = BIO_new_mem_buf(data, -1);
    BIO* base64 = BIO_new(BIO_f_base64());

    BIO_set_flags(base64, BIO_FLAGS_BASE64_NO_NL);
    BIO_push(base64, bio);

    // Determine the size of the decoded data
    size_t encoded_length = strlen(data);
    size_t decoded_length = (encoded_length * 3) / 4;  // Rough estimate
    unsigned char* decoded_data = (unsigned char*)vws.malloc(decoded_length);

    // Decode the base64 data
    *size = BIO_read(base64, decoded_data, encoded_length);

    BIO_free_all(base64);

    return decoded_data;
}

//------------------------------------------------------------------------------
// Utilities
//------------------------------------------------------------------------------

/**
 * @brief Sleeps for the specified number of milliseconds.
 *
 * This function provides a platform-independent way to sleep for a specific
 * duration in milliseconds. The behavior is similar to the standard `sleep`
 * function, but with millisecond precision.
 *
 * @param ms The number of milliseconds to sleep.
 *
 * @note This function may not be available on all platforms. Make sure to
 *       include the necessary headers and handle any compilation errors or
 *       warnings specific to your environment.
 */
void vws_msleep(unsigned int ms)
{
#if defined(__windows__) || defined(_WIN64)

    Sleep(ms);

#elif defined(__linux__)

    usleep(ms * 1000);

#elif defined(__bsd__)

    usleep(ms * 1000);

#elif defined(__sunos__)

    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000;
    nanosleep(&ts, NULL);

#else

#error "Unsupported platform. msleep is not implemented for this platform."

#endif
}

uint8_t vws_is_flag(const uint64_t* flags, uint64_t flag)
{
    return (*flags & flag) == flag;
}

void vws_set_flag(uint64_t* flags, uint64_t flag)
{
    *flags |= flag;
}

void vws_clear_flag(uint64_t* flags, uint64_t flag)
{
    *flags &= ~flag;
}

char* vws_file_path(const char* root, const char* filename)
{
    // Initial guess for the required buffer size

    // +2 for slash and null terminator
    size_t size = strlen(root) + strlen(filename) + 2;
    char* path  = vws.malloc(size);

    if (path == NULL)
    {
        fprintf(stderr, "Memory allocation failed\n");
        return NULL;
    }

    // Attempt to write to the buffer
    size_t written = snprintf(path, size, "%s/%s", root, filename);

    // Check if the buffer was large enough
    if (written >= size)
    {
        // Buffer was too small, allocate again with the correct size
        size = written + 1; // +1 for the null terminator
        char* temp = vws.realloc(path, size);

        if (temp == NULL)
        {
            fprintf(stderr, "Memory reallocation failed\n");
            free(path);
            return NULL;
        }

        path = temp;

        // Write again
        snprintf(path, size, "%s/%s", root, filename);
    }

    return path;
}

bool vws_cstr_to_long(cstr str, long* value)
{
    char* endptr;

    // To distinguish success/failure after call
    errno = 0;

    if (str == NULL || *str == '\0' || isspace(*str))
    {
        // String is empty or whitespace only
        return false;
    }

    *value = strtol(str, &endptr, 10);  // Base 10 conversion

    // Check for conversion errors or incomplete conversion
    if (errno == ERANGE)
    {
        // Value out of range for long
        return false;
    }
    if (*endptr != '\0' || endptr == str)
    {
        // Whole string wasn’t converted
        return false;
    }

    // Successful conversion
    return true;
}

