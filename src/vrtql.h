#ifndef VRTQL_COMMON_DECLARE
#define VRTQL_COMMON_DECLARE

#include <stddef.h>

#include "common.h"

// Typedefs for brevity
typedef const char* cstr;
typedef const unsigned char* ucstr;

//------------------------------------------------------------------------------
// Error handling
//------------------------------------------------------------------------------

/**
 * @brief Enumerates error codes used in the library.
 */
typedef enum vrtql_error_code
{
    VE_SUCCESS = 0,   /**< No error */
    VE_TIMEOUT = 1,   /**< Socket timeout */
    VE_WARN    = 2,   /**< Warning */
    VE_SYS     = 10,  /**< System call error */
    VE_RT      = 11,  /**< Runtime error */
    VE_MEM     = 100, /**< Memory failure */
    VE_FATAL   = 200, /**< Fatal error */
} vrtql_error_code;

/**
 * @brief Defines a structure for vrtql errors.
 */
typedef struct
{
    int code;       /**< Error code */
    char* message;  /**< Error message */
} vrtql_error;

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
 * @brief Callback for error submission.
 */
typedef int (*vrtql_error_submit_cb)(int code, cstr message);

/**
 * @brief Callback for error processing.
 */
typedef int (*vrtql_error_process_cb)(int code, cstr message);

/**
 * @brief Callback for error clearing.
 */
typedef void (*vrtql_error_clear_cb)();

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
    uint8_t trace;                        /**< Turns on tracing            */
    int state;                            /**< Contains global state flags */
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
// Tracing
//------------------------------------------------------------------------------

/**
 * @brief Enumerates the levels of logging.
 */
typedef enum
{
    VL_DEBUG,         /**< Debug level log       */
    VL_INFO,          /**< Information level log */
    VL_WARNING,       /**< Warning level log     */
    VL_ERROR,         /**< Error level log       */
    VL_LEVEL_COUNT    /**< Count of log levels   */
} vrtql_log_level;

/**
 * @brief Logs a trace message.
 *
 * @param level The level of the log
 * @param format The format string for the message
 * @param ... The arguments for the format string
 */
void vrtql_trace(vrtql_log_level level, const char* format, ...);

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
