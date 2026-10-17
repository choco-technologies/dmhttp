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
     * Callbacks must not reenter that handle. TLS must be initialized by its owner. */
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
