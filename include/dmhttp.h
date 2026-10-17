#ifndef DMHTTP_H
#define DMHTTP_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "dmtls.h"
#include "dmhttp_defs.h"
#ifdef __cplusplus
extern "C"
{
#endif
#define DMHTTP_URL_MAX 2048u
#define DMHTTP_HEADER_MAX 8192u
#define DMHTTP_HOST_MAX 253u

    typedef struct dmhttp *dmhttp_t;
    /* Local operation results, not HTTP response status codes.
     * Read response.status_code for the server response (e.g. 404). */
    typedef enum
    {
        DMHTTP_OK = 0,
        DMHTTP_WANT_READ = 1,
        DMHTTP_WANT_WRITE = 2,
        DMHTTP_AGAIN = 3,
        DMHTTP_DONE = 4,
        DMHTTP_ERR_ARGUMENT = -1,
        DMHTTP_ERR_MEMORY = -2,
        DMHTTP_ERR_STATE = -3,
        DMHTTP_ERR_URL = -4,
        DMHTTP_ERR_TRANSPORT = -5,
        DMHTTP_ERR_TLS = -6,
        DMHTTP_ERR_PROTOCOL = -7,
        DMHTTP_ERR_LIMIT = -8,
        DMHTTP_ERR_TRUNCATED = -9,
        DMHTTP_ERR_TIMEOUT = -10,
        DMHTTP_ERR_CANCELLED = -11,
        DMHTTP_ERR_REDIRECT = -12
    } dmhttp_result_t;
    /* HTTP response statuses use their standard wire values. This lists the
     * permanent assigned statuses; status_code also preserves extension codes.
     * https://www.iana.org/assignments/http-status-codes/ */
    typedef enum
    {
        DMHTTP_STATUS_CONTINUE = 100,
        DMHTTP_STATUS_SWITCHING_PROTOCOLS = 101,
        DMHTTP_STATUS_PROCESSING = 102,
        DMHTTP_STATUS_EARLY_HINTS = 103,
        DMHTTP_STATUS_OK = 200,
        DMHTTP_STATUS_CREATED = 201,
        DMHTTP_STATUS_ACCEPTED = 202,
        DMHTTP_STATUS_NON_AUTHORITATIVE_INFORMATION = 203,
        DMHTTP_STATUS_NO_CONTENT = 204,
        DMHTTP_STATUS_RESET_CONTENT = 205,
        DMHTTP_STATUS_PARTIAL_CONTENT = 206,
        DMHTTP_STATUS_MULTI_STATUS = 207,
        DMHTTP_STATUS_ALREADY_REPORTED = 208,
        DMHTTP_STATUS_IM_USED = 226,
        DMHTTP_STATUS_MULTIPLE_CHOICES = 300,
        DMHTTP_STATUS_MOVED_PERMANENTLY = 301,
        DMHTTP_STATUS_FOUND = 302,
        DMHTTP_STATUS_SEE_OTHER = 303,
        DMHTTP_STATUS_NOT_MODIFIED = 304,
        DMHTTP_STATUS_USE_PROXY = 305,
        DMHTTP_STATUS_TEMPORARY_REDIRECT = 307,
        DMHTTP_STATUS_PERMANENT_REDIRECT = 308,
        DMHTTP_STATUS_BAD_REQUEST = 400,
        DMHTTP_STATUS_UNAUTHORIZED = 401,
        DMHTTP_STATUS_PAYMENT_REQUIRED = 402,
        DMHTTP_STATUS_FORBIDDEN = 403,
        DMHTTP_STATUS_NOT_FOUND = 404,
        DMHTTP_STATUS_METHOD_NOT_ALLOWED = 405,
        DMHTTP_STATUS_NOT_ACCEPTABLE = 406,
        DMHTTP_STATUS_PROXY_AUTHENTICATION_REQUIRED = 407,
        DMHTTP_STATUS_REQUEST_TIMEOUT = 408,
        DMHTTP_STATUS_CONFLICT = 409,
        DMHTTP_STATUS_GONE = 410,
        DMHTTP_STATUS_LENGTH_REQUIRED = 411,
        DMHTTP_STATUS_PRECONDITION_FAILED = 412,
        DMHTTP_STATUS_CONTENT_TOO_LARGE = 413,
        DMHTTP_STATUS_URI_TOO_LONG = 414,
        DMHTTP_STATUS_UNSUPPORTED_MEDIA_TYPE = 415,
        DMHTTP_STATUS_RANGE_NOT_SATISFIABLE = 416,
        DMHTTP_STATUS_EXPECTATION_FAILED = 417,
        DMHTTP_STATUS_MISDIRECTED_REQUEST = 421,
        DMHTTP_STATUS_UNPROCESSABLE_CONTENT = 422,
        DMHTTP_STATUS_LOCKED = 423,
        DMHTTP_STATUS_FAILED_DEPENDENCY = 424,
        DMHTTP_STATUS_TOO_EARLY = 425,
        DMHTTP_STATUS_UPGRADE_REQUIRED = 426,
        DMHTTP_STATUS_PRECONDITION_REQUIRED = 428,
        DMHTTP_STATUS_TOO_MANY_REQUESTS = 429,
        DMHTTP_STATUS_REQUEST_HEADER_FIELDS_TOO_LARGE = 431,
        DMHTTP_STATUS_UNAVAILABLE_FOR_LEGAL_REASONS = 451,
        DMHTTP_STATUS_INTERNAL_SERVER_ERROR = 500,
        DMHTTP_STATUS_NOT_IMPLEMENTED = 501,
        DMHTTP_STATUS_BAD_GATEWAY = 502,
        DMHTTP_STATUS_SERVICE_UNAVAILABLE = 503,
        DMHTTP_STATUS_GATEWAY_TIMEOUT = 504,
        DMHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED = 505,
        DMHTTP_STATUS_VARIANT_ALSO_NEGOTIATES = 506,
        DMHTTP_STATUS_INSUFFICIENT_STORAGE = 507,
        DMHTTP_STATUS_LOOP_DETECTED = 508,
        DMHTTP_STATUS_NETWORK_AUTHENTICATION_REQUIRED = 511
    } dmhttp_status_t;
    typedef enum
    {
        DMHTTP_GET = 0,
        DMHTTP_HEAD = 1
    } dmhttp_method_t;
    /* Each handle owns one connector. open starts or continues asynchronous
     * DNS/connect; retry it with identical host/port after WANT_*. close cancels
     * pending DNS/connect, releases the stream and detaches all callbacks before
     * returning. It is called once after every attempted open, even on failure.
     * send/receive obey dmtls's nonblocking byte-count contract (OK/0 = read EOF).
     * The adapter must arrange external readiness notifications; no polling thread
     * is created by dmhttp. Separate handles require separate adapter user data. */
    typedef dmtls_result_t (*dmhttp_open_fn)(void *, const char *, uint16_t);
    typedef void (*dmhttp_close_fn)(void *);
    typedef struct
    {
        dmhttp_open_fn open;
        dmhttp_close_fn close;
        dmtls_transport_t stream;
    } dmhttp_transport_t;
    typedef struct
    {
        dmhttp_transport_t transport;
        dmtls_config_t tls_config; /* Borrowed until destroy. Required for HTTPS. */
        dmhttp_method_t method;
        uint32_t timeout_ms;     /* Entire transaction, including DNS and redirects. */
        uint32_t max_redirects;  /* 0 disables following, at most 16. */
        uint64_t max_body_bytes; /* 0 explicitly permits unlimited streamed bytes. */
        bool range_enabled;
        uint64_t range_start; /* GET: Range: bytes=N-; unchanged across redirects. */
    } dmhttp_options_t;
    typedef struct
    {
        /* Standard HTTP wire value; 0 until a status line is received.
         * Inspect after step returns OK for the final response headers.
         * HTTP 4xx/5xx do not become DMHTTP_ERR_* operation failures. */
        unsigned status_code;
        unsigned redirects;
        bool content_length_known;
        uint64_t content_length;
        uint64_t body_bytes;
        /* A 206 response is never treated as proof that the requested range was
         * accepted: the application must validate this field before appending. */
        char content_range[128];
        char content_type[128];
        dmhttp_result_t error;
        dmtls_result_t tls_error;
        uint32_t verification_flags;
    } dmhttp_response_t;
    /* URL/options are copied. Only http/https, ASCII DNS/IPv4 hosts and escaped
     * origin-form targets are accepted (no credentials or IPv6 literals).
     * Requests use Connection: close and Accept-Encoding: identity. No uploads,
     * authentication, decompression, pooling or proxy tunnelling are performed.
     * Calls on one handle must be externally serialized, including cancel/destroy.
     * Callbacks must not reenter that handle. TLS must be initialized by its owner.
     */
    dmod_dmhttp_api(1.0, dmhttp_result_t, _create,
                    (const char *url, const dmhttp_options_t *options, dmhttp_t *output));
    /* Advance to final response headers. OK means headers are available. AGAIN
     * means bounded local progress; WANT_* means wait for stream readiness.
     * Also schedule the remaining transaction deadline: inactivity cannot disable
     * timeouts. No call waits for network events. */
    dmod_dmhttp_api(1.0, dmhttp_result_t, _step, (dmhttp_t handle));
    /* Call after step returns OK. Streams decoded body bytes into caller memory.
     * May return OK with bytes and report a framing error on the next call; commit
     * downloaded files only after DONE. DONE is repeated on subsequent reads. */
    dmod_dmhttp_api(1.0, dmhttp_result_t, _read,
                    (dmhttp_t handle, void *buffer, size_t capacity, size_t *received));
    dmod_dmhttp_api(1.0, dmhttp_result_t, _response, (dmhttp_t handle, dmhttp_response_t *output));
    dmod_dmhttp_api(1.0, dmhttp_result_t, _cancel, (dmhttp_t handle));
    dmod_dmhttp_api(1.0, void, _destroy, (dmhttp_t handle));
#ifdef __cplusplus
}
#endif
#endif
