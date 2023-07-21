#ifndef VRTQL_COMMON_DECLARE
#define VRTQL_COMMON_DECLARE

#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "util/sc_map.h"

// Typedefs for brevity
typedef const char* cstr;
typedef const unsigned char* ucstr;

//------------------------------------------------------------------------------
// Error handling
//------------------------------------------------------------------------------

/**
 * @brief Enumerates error codes used in the library.
 */
typedef enum
{
    VE_SUCCESS = 0,   /**< No error */
    VE_TIMEOUT = 1,   /**< Socket timeout */
    VE_WARN    = 2,   /**< Warning */
    VE_SYS     = 10,  /**< System call error */
    VE_RT      = 11,  /**< Runtime error */
    VE_MEM     = 100, /**< Memory failure */
    VE_FATAL   = 200, /**< Fatal error */
} vrtql_error_code_t;

// Trace levels
typedef enum vrtql_tl_t
{
    VT_OFF         = 0,
    VT_APPLICATION = 1,
    VT_MODULE      = 2,
    VT_SERVICE     = 3,
    VT_PROTOCOL    = 4,
    VT_THREAD      = 5,
    VT_TCPIP       = 6,
    VT_LOCK        = 7,
    VT_MEMORY      = 8,
    VT_ALL         = 9
} vrtql_tl_t;

/**
 * @brief Defines a structure for vrtql errors.
 */
typedef struct
{
    int code;       /**< Error code */
    char* text;     /**< Error text */
} vrtql_error_value;

//------------------------------------------------------------------------------
// Tracing
//------------------------------------------------------------------------------

/**
 * @brief Enumerates the levels of logging.
 */
typedef enum
{
    VL_DEBUG,         /**< Debug level log       */
    VL_INFO,          /**< Information level log */
    VL_WARN,          /**< Warning level log     */
    VL_ERROR,         /**< Error level log       */
    VL_LEVEL_COUNT    /**< Count of log levels   */
} vrtql_log_level_t;

/**
 * @brief Logs a trace message.
 *
 * @param level The level of the log
 * @param format The format string for the message
 * @param ... The arguments for the format string
 */
void vrtql_trace(vrtql_log_level_t level, const char* format, ...);

/**
 * @brief Callback for tracing
 */
typedef void (*vrtql_trace_cb)(vrtql_log_level_t level, const char* fmt, ...);

/**
 * @brief Callback for memory allocation with malloc.
 */
typedef void* (*vrtql_malloc_cb)(size_t size);

/**
 * @brief Callback for malloc failure. This is called when vrtql.malloc() fails
 * to allocate memory. This function is meant to handle, report and/or recover
 * from the error. Whatever this function returns will be returned from the
 * vrtql.malloc().
 *
 * @param size The size argument passed to vrtql.malloc()
 */
typedef void* (*vrtql_malloc_error_cb)(size_t size);

/**
 * @brief Callback for memory allocation with calloc.
 */
typedef void* (*vrtql_calloc_cb)(size_t nmemb, size_t size);

/**
 * @brief Callback for calloc failure. This is called when vrtql.calloc() fails
 * to allocate memory. This function is meant to handle, report and/or recover
 * from the error. Whatever this function returns will be returned from the
 * vrtql.calloc().
 *
 * @param nmemb The nmemb argument passed to vrtql.calloc()
 * @param size The size argument passed to vrtql.calloc()
 */
typedef void* (*vrtql_calloc_error_cb)(size_t nmemb, size_t size);

/**
 * @brief Callback for memory allocation with realloc.
 *
 * @param ptr The ptr argument passed to vrtql.realloc()
 */
typedef void* (*vrtql_realloc_cb)(void* ptr, size_t size);

/**
 * @brief Callback for realloc failure. This is called when vrtql.realloc()
 * fails to allocate memory. This function is meant to handle, report and/or
 * recover from the error. Whatever this function returns will be returned from
 * the vrtql.realloc().
 *
 * @param ptr The ptr argument passed to vrtql.realloc()
 * @param size The size argument passed to vrtql.realloc()
 */
typedef void* (*vrtql_realloc_error_cb)(void* ptr, size_t size);

/**
 * @brief Callback for error submission. Error submission function. Error
 * submission takes care of recording the error in the vrtql.e member. The next
 * step is the process the error.
 */
typedef int (*vrtql_error_submit_cb)(int code, cstr message, ...);

/**
 * @brief Callback for error processing. Error processing function. Error
 * processing makes policy decisions, if any, on how to handle specific classes
 * of errors, for example how to exit the process on fatal errors (VE_FATAL), or
 * how to handle memory allocation errors (VE_MEM).
 */
typedef int (*vrtql_error_process_cb)(int code, cstr message);

/**
 * @brief Callback for error clearing. The default is to set VE_SUCESS.
 */
typedef void (*vrtql_error_clear_cb)();

/**
 * @brief Callback for success conditions. The default is to set VE_SUCESS.
 */
typedef void (*vrtql_error_success_cb)();

/**
 * @brief Defines the global vrtql environment.
 */
typedef struct
{
    vrtql_malloc_cb malloc;               /**< malloc function             */
    vrtql_malloc_error_cb malloc_error;   /**< malloc error hanlding       */
    vrtql_calloc_cb calloc;               /**< calloc function             */
    vrtql_calloc_error_cb calloc_error;   /**< calloc error hanlding       */
    vrtql_realloc_cb realloc;             /**< realloc function            */
    vrtql_realloc_error_cb realloc_error; /**< calloc error hanlding       */
    vrtql_error_submit_cb error;          /**< Error submission function   */
    vrtql_error_process_cb process_error; /**< Error processing function   */
    vrtql_error_clear_cb clear_error;     /**< Error clear function        */
    vrtql_error_clear_cb success;         /**< Error clear function        */
    vrtql_error_value e;                  /**< Last error value            */
    vrtql_trace_cb trace;                 /**< Error clear function        */
    uint8_t tracelevel;                   /**< Tracing leve (0 is off)     */
    uint64_t state;                       /**< Contains global state flags */
} vrtql_env;

/**
 * @brief The global vrtql environment variable
 */
extern __thread vrtql_env vrtql;

/**
 * @brief Defines a buffer for vrtql.
 */
typedef struct vrtql_buffer
{
    unsigned char* data; /**< The data in the buffer                       */
    size_t allocated;    /**< The amount of space allocated for the buffer */
    size_t size;         /**< The current size of the data in the buffer   */
} vrtql_buffer;

//------------------------------------------------------------------------------
// Buffer
//------------------------------------------------------------------------------

/**
 * @brief Creates a new vrtql buffer.
 *
 * @return Returns a new vrtql_buffer instance
 */
vrtql_buffer* vrtql_buffer_new();

/**
 * @brief Frees a vrtql buffer.
 *
 * @param buffer The buffer to be freed
 */
void vrtql_buffer_free(vrtql_buffer* buffer);

/**
 * @brief Clears a vrtql buffer.
 *
 * @param buffer The buffer to be cleared
 */
void vrtql_buffer_clear(vrtql_buffer* buffer);

/**
 * @brief Appends formatted data to a vrtql_buffer using printf() format.
 *
 * This function appends formatted data to the specified vrtql_buffer using a
 * printf()-style format string and variable arguments. The formatted data is
 * appended to the existing content of the buffer.
 *
 * @param buffer The vrtql_buffer to append the formatted data to.
 * @param format The printf() format string specifying the format of the data to
 *        be appended.
 * @param ... Variable arguments corresponding to the format specifier in the
 *        format string.
 *
 * @note The behavior of this function is similar to the standard printf()
 *       function, where the format string specifies the expected format of the
 *       data and the variable arguments provide the values to be formatted and
 *       appended to the buffer.
 *
 * @note The vrtql_buffer must be initialized and have sufficient capacity to
 *       hold the appended data. If the buffer capacity is exceeded, the
 *       behavior is undefined.
 *
 * @warning Take care to ensure the format string and variable arguments are
 *          consistent, as mismatches can lead to undefined behavior or security
 *          vulnerabilities (e.g., format string vulnerabilities).
 *
 * @see vrtql_buffer_init
 * @see vrtql_buffer_append
 */
void vrtql_buffer_printf(vrtql_buffer* buffer, cstr format, ...);

/**
 * @brief Appends data to a vrtql buffer.
 *
 * @param buffer The buffer to append to
 * @param data The data to append
 * @param size The size of the data
 */
void vrtql_buffer_append(vrtql_buffer* buffer, ucstr data, size_t size);

/**
 * @brief Drains a vrtql buffer by a given size.
 *
 * @param buffer The buffer to drain
 * @param size The size to drain from the buffer
 */
void vrtql_buffer_drain(vrtql_buffer* buffer, size_t size);

//------------------------------------------------------------------------------
// Map
//------------------------------------------------------------------------------

/**
 * @brief Retrieves a value from the map using a string key.
 *
 * Returns a constant string pointer to the value associated with the key.
 *
 * @param map The map from which to retrieve the value.
 * @param key The string key to use for retrieval.
 * @return A constant string pointer to the value associated with the key.
 */
cstr vrtql_map_get(struct sc_map_str* map, cstr key);

/**
 * @brief Sets a value in the map using a string key and value.
 *
 * It will create a new key-value pair or update the value if the key already exists.
 *
 * @param map The map in which to set the value.
 * @param key The string key to use for setting.
 * @param value The string value to set.
 */
void vrtql_map_set(struct sc_map_str* map, cstr key, cstr value);

/**
 * @brief Removes a key-value pair from the map using a string key.
 *
 * If the key exists in the map, it will be removed along with its associated value.
 *
 * @param map The map from which to remove the key-value pair.
 * @param key The string key to use for removal.
 */
void vrtql_map_remove(struct sc_map_str* map, cstr key);

/**
 * @brief Removes all key-value pair from the map, calling free() on them
 *
 * @param map The map to clear
 */
void vrtql_map_clear(struct sc_map_str* map);

//------------------------------------------------------------------------------
// URL
//------------------------------------------------------------------------------

/**
 * @brief Represents a parsed URL.
 */
typedef struct
{
    char* scheme;    /**< The URL scheme (http, https, etc.) */
    char* host;      /**< The host name or IP address        */
    char* port;      /**< The port number                    */
    char* path;      /**< The path on the host               */
    char* query;     /**< The query parameters               */
    char* fragment;  /**< The fragment identifier            */
} vrtql_url;

/**
 * @brief Parses a URL.
 *
 * @param url The URL to parse
 * @return Returns a vrtql_url structure with the parsed URL parts
 */
vrtql_url vrtql_url_parse(const char* url);

/**
 * @brief Builds a URL string from parts.
 *
 * @param parts The URL parts
 * @return Returns a URL string built from the parts
 */
char* vrtql_url_build(const vrtql_url* parts);

/**
 * @brief Allocates a vrtql_url structure.
 *
 * @return Returns a new vrtql_url structure
 */
vrtql_url vrtql_url_new();

/**
 * @brief Frees a vrtql_url structure.
 *
 * @param parts The vrtql_url structure to free
 */
void vrtql_url_free(vrtql_url parts);

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
void vrtql_msleep(unsigned int ms);

/**
 * @brief Checks if a specific flag is set.
 *
 * @param flags The flags to check
 * @param flag  The flag to check for
 * @return Returns true if the flag is set, false otherwise
 */
uint8_t vrtql_is_flag(const uint64_t* flags, uint64_t flag);

/**
 * @brief Sets a specific flag.
 *
 * @param flags The flags to modify
 * @param flag  The flag to set
 * @return None
 */
void vrtql_set_flag(uint64_t* flags, uint64_t flag);

/**
 * @brief Clears a specific flag.
 *
 * @param flags The flags to modify
 * @param flag  The flag to clear
 * @return None
 */
void vrtql_clear_flag(uint64_t* flags, uint64_t flag);

/**
 * @brief Generates a UUID.
 *
 * @return Returns a UUID string
 */
char* vrtql_generate_uuid();

/**
 * @brief Encodes data as Base64.
 *
 * @param data The data to encode
 * @param size The size of the data
 * @return Returns a Base64-encoded string
 */
char* vrtql_base64_encode(const unsigned char* data, size_t size);

/**
 * @brief Decodes a Base64-encoded string.
 *
 * @param data The Base64 string to decode
 * @param size Pointer to size which will be filled with the size of the decoded data
 * @return Returns a pointer to the decoded data
 */
unsigned char* vrtql_base64_decode(const char* data, size_t* size);

#endif /* VRTQL_COMMON_DECLARE */
