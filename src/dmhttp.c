#define DMOD_ENABLE_REGISTRATION ON
#include "dmhttp_internal.h"
#include <stdatomic.h>
#include <errno.h>
static atomic_uint live_requests;
#define HTTP_MAGIC 0x48545450u
static bool valid(dmhttp_t h)
{
    return h && h->magic == HTTP_MAGIC;
}

void http_release(dmhttp_t h)
{
    if (h->tls)
    {
        dmtls_connection_destroy(h->tls);
        h->tls = NULL;
    }
    if (h->opened)
    {
        h->options.transport.close(h->options.transport.stream.user);
        h->opened = false;
    }
}
dmhttp_result_t http_fail(dmhttp_t h, dmhttp_result_t rc)
{
    h->response.error = rc;
    h->state = HTTP_FAILED;
    http_release(h);
    return rc;
}
static dmhttp_result_t tls_result(dmhttp_t h, dmtls_result_t rc)
{
    if (rc == DMTLS_OK)
        return DMHTTP_OK;
    if (rc == DMTLS_WANT_READ)
        return DMHTTP_WANT_READ;
    if (rc == DMTLS_WANT_WRITE)
        return DMHTTP_WANT_WRITE;
    h->response.tls_error = rc;
    if (h->tls)
    {
        dmtls_status_t status;
        if (dmtls_status(h->tls, &status) == DMTLS_OK)
            h->response.verification_flags = status.verification_flags;
    }
    return rc == DMTLS_ERR_TIMEOUT ? DMHTTP_ERR_TIMEOUT : DMHTTP_ERR_TLS;
}
static dmhttp_result_t check(dmhttp_t h)
{
    if (h->state == HTTP_FAILED)
        return h->response.error;
    if (h->state == HTTP_FINISHED)
        return DMHTTP_DONE;
    if ((uint32_t)(dmosi_get_tick_count() - h->started) >= h->options.timeout_ms)
        return http_fail(h, DMHTTP_ERR_TIMEOUT);
    return DMHTTP_OK;
}
static dmhttp_result_t receive(dmhttp_t h)
{
    if (h->input_at < h->input_size)
        return DMHTTP_OK;
    h->input_at = h->input_size = 0;
    size_t n = 0;
    dmtls_result_t tr;
    if (h->tls)
        tr = dmtls_read(h->tls, h->input, sizeof(h->input), &n);
    else
        tr = h->options.transport.stream.receive(h->options.transport.stream.user, h->input,
                                                 sizeof(h->input), &n);
    if (n > sizeof(h->input) || (tr != DMTLS_OK && n))
        return DMHTTP_ERR_TRANSPORT;
    if (tr == DMTLS_WANT_READ)
        return DMHTTP_WANT_READ;
    if (tr == DMTLS_WANT_WRITE)
        return DMHTTP_WANT_WRITE;
    if (h->tls && tr == DMTLS_PEER_CLOSED)
        return DMHTTP_DONE;
    if (tr != DMTLS_OK)
        return h->tls ? tls_result(h, tr) : DMHTTP_ERR_TRANSPORT;
    if (!n)
        return DMHTTP_DONE;
    h->input_size = n;
    return DMHTTP_OK;
}
/* Consumes at most one buffered byte. Strict CRLF and bounded line storage. */
static dmhttp_result_t line_byte(dmhttp_t h, size_t *budget, size_t limit, bool *complete)
{
    *complete = false;
    dmhttp_result_t rc = receive(h);
    if (rc != DMHTTP_OK)
        return rc == DMHTTP_DONE ? DMHTTP_ERR_TRUNCATED : rc;
    unsigned char c = h->input[h->input_at++];
    if (++*budget > limit)
        return DMHTTP_ERR_LIMIT;
    if (c == '\n')
    {
        if (!h->line_size || h->line[h->line_size - 1] != '\r')
            return DMHTTP_ERR_PROTOCOL;
        h->line[--h->line_size] = 0;
        *complete = true;
        return DMHTTP_OK;
    }
    if (!c || (h->line_size && h->line[h->line_size - 1] == '\r'))
        return DMHTTP_ERR_PROTOCOL;
    if (h->line_size == HTTP_LINE_MAX)
        return DMHTTP_ERR_LIMIT;
    h->line[h->line_size++] = (char)c;
    return DMHTTP_AGAIN;
}
static dmhttp_result_t connect_stream(dmhttp_t h)
{
    dmhttp_result_t rc;

    h->opened = true;
    dmtls_result_t tr =
        h->options.transport.open(h->options.transport.stream.user, h->url.host, h->url.port);
    if (tr == DMTLS_WANT_READ)
        return DMHTTP_WANT_READ;
    if (tr == DMTLS_WANT_WRITE)
        return DMHTTP_WANT_WRITE;
    if (tr != DMTLS_OK)
        return http_fail(h, DMHTTP_ERR_TRANSPORT);
    if (h->url.secure)
    {
        rc = tls_result(h, dmtls_connection_create(
                               h->options.tls_config, h->url.host, &h->options.transport.stream,
                               h->options.timeout_ms, h->options.timeout_ms, &h->tls));
        if (rc != DMHTTP_OK)
            return http_fail(h, rc);
        h->state = HTTP_TLS;
    }
    else
        h->state = HTTP_SEND;
    return DMHTTP_AGAIN;
}
static dmhttp_result_t send_request(dmhttp_t h)
{

    size_t n = 0, left = h->request_size - h->request_sent;
    dmtls_result_t tr;
    if (h->tls)
        tr = dmtls_write(h->tls, h->request + h->request_sent, left, &n);
    else
        tr = h->options.transport.stream.send(h->options.transport.stream.user,
                                              (const uint8_t *)h->request + h->request_sent, left,
                                              &n);
    if (n > left || (tr != DMTLS_OK && n))
        return http_fail(h, DMHTTP_ERR_TRANSPORT);
    if (tr == DMTLS_WANT_READ)
        return DMHTTP_WANT_READ;
    if (tr == DMTLS_WANT_WRITE)
        return DMHTTP_WANT_WRITE;
    if (tr != DMTLS_OK)
        return http_fail(h, h->tls ? tls_result(h, tr) : DMHTTP_ERR_TRANSPORT);
    if (!n)
        return http_fail(h, DMHTTP_ERR_TRANSPORT);
    h->request_sent += n;
    if (h->request_sent == h->request_size)
        h->state = HTTP_HEADERS;
    return DMHTTP_AGAIN;
}
static dmhttp_result_t read_headers(dmhttp_t h)
{
    dmhttp_result_t rc;
    for (unsigned work = 0; work < HTTP_IO_SIZE; ++work)
    {
        bool complete;
        rc = line_byte(h, &h->header_bytes, DMHTTP_HEADER_MAX, &complete);
        if (rc < 0)
            return http_fail(h, rc);
        if (rc == DMHTTP_WANT_READ || rc == DMHTTP_WANT_WRITE)
            return rc;
        if (!complete)
            continue;
        if (h->line_size)
            rc = http_header_line(h, false);
        else
            rc = http_headers_done(h);
        h->line_size = 0;
        if (rc < 0)
            return http_fail(h, rc);
        if (h->state != HTTP_HEADERS)
            return rc;
    }
    return DMHTTP_AGAIN;
}
/* Send close_notify without waiting for a peer alert. The connector's close
 * releases the stream after these bytes have been handed to its send queue. */
static dmhttp_result_t close_tls(dmhttp_t h)
{
    if (!h->tls)
        return DMHTTP_OK;
    dmhttp_result_t rc = tls_result(h, dmtls_close(h->tls));
    return rc < 0 ? http_fail(h, rc) : rc;
}
static dmhttp_result_t step(dmhttp_t h)
{
    dmhttp_result_t rc = check(h);
    if (rc != DMHTTP_OK)
        return rc;
    if (h->state == HTTP_BODY || h->state == HTTP_CLOSING)
        return DMHTTP_OK;
    if (h->state == HTTP_REDIRECT)
    {
        rc = close_tls(h);
        if (rc != DMHTTP_OK)
            return rc;
        rc = http_redirect_restart(h);
        return rc < 0 ? http_fail(h, rc) : rc;
    }
    if (h->state == HTTP_CONNECT)
        return connect_stream(h);
    if (h->state == HTTP_TLS)
    {
        rc = tls_result(h, dmtls_handshake(h->tls));
        if (rc < 0)
            return http_fail(h, rc);
        if (rc != DMHTTP_OK)
            return rc;
        h->state = HTTP_SEND;
        return DMHTTP_AGAIN;
    }
    if (h->state == HTTP_SEND)
        return send_request(h);
    return read_headers(h);
}
static dmhttp_result_t finish(dmhttp_t h)
{
    h->state = HTTP_CLOSING;
    dmhttp_result_t rc = close_tls(h);
    if (rc != DMHTTP_OK)
        return rc;
    h->state = HTTP_FINISHED;
    http_release(h);
    return DMHTTP_DONE;
}
static dmhttp_result_t chunk_extensions(const char *p)
{
    /* Validate extension grammar, including quoted strings and escapes. */
    while (*p)
    {
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p++ != ';')
            return DMHTTP_ERR_PROTOCOL;
        while (*p == ' ' || *p == '\t')
            ++p;
        size_t tokens = 0;
        while (http_token((unsigned char)*p))
        {
            ++p;
            ++tokens;
        }
        if (!tokens)
            return DMHTTP_ERR_PROTOCOL;
        while (*p == ' ' || *p == '\t')
            ++p;
        if (*p == '=')
        {
            ++p;
            while (*p == ' ' || *p == '\t')
                ++p;
            if (*p == '"')
            {
                ++p;
                while (*p && *p != '"')
                {
                    unsigned char c = *p++;
                    if (c == '\\')
                    {
                        if (!*p)
                            return DMHTTP_ERR_PROTOCOL;
                        c = *p++;
                    }
                    if ((c < 32 && c != '\t') || c == 127)
                        return DMHTTP_ERR_PROTOCOL;
                }
                if (*p++ != '"')
                    return DMHTTP_ERR_PROTOCOL;
            }
            else
            {
                tokens = 0;
                while (http_token((unsigned char)*p))
                {
                    ++p;
                    ++tokens;
                }
                if (!tokens)
                    return DMHTTP_ERR_PROTOCOL;
            }
        }
    }
    return DMHTTP_OK;
}
static dmhttp_result_t chunk_size(dmhttp_t h)
{
    const char *p = h->line;
    uint64_t n = 0;
    size_t digits = 0;
    while (*p && *p != ';' && *p != ' ' && *p != '\t')
    {
        unsigned char c = *p++;
        unsigned v;
        if (c >= '0' && c <= '9')
            v = c - '0';
        else if (c >= 'a' && c <= 'f')
            v = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F')
            v = c - 'A' + 10;
        else
            return DMHTTP_ERR_PROTOCOL;
        if (n > (UINT64_MAX - v) / 16)
            return DMHTTP_ERR_LIMIT;
        n = n * 16 + v;
        ++digits;
    }
    if (!digits)
        return DMHTTP_ERR_PROTOCOL;
    dmhttp_result_t rc = chunk_extensions(p);
    if (rc != DMHTTP_OK)
        return rc;
    if (h->options.max_body_bytes && n > h->options.max_body_bytes - h->response.body_bytes)
        return DMHTTP_ERR_LIMIT;
    h->remaining = n;
    h->framing = n ? HTTP_CHUNK_DATA : HTTP_TRAILERS;
    return DMHTTP_OK;
}
static dmhttp_result_t read_chunk_line(dmhttp_t h)
{
    dmhttp_result_t rc;

    bool complete;
    rc = line_byte(h, &h->trailer_bytes, DMHTTP_HEADER_MAX, &complete);
    if (rc < 0)
        return http_fail(h, rc);
    if (rc == DMHTTP_WANT_READ || rc == DMHTTP_WANT_WRITE)
        return rc;
    if (!complete)
        return DMHTTP_AGAIN;
    if (h->framing == HTTP_TRAILERS)
    {
        if (!h->line_size)
            return finish(h);
        rc = http_header_line(h, true);
    }
    else
        rc = chunk_size(h);
    h->line_size = 0;
    /* Chunk framing budget is per chunk; trailer budget is cumulative. */
    if (h->framing == HTTP_CHUNK_DATA)
        h->trailer_bytes = 0;
    if (rc < 0)
        return http_fail(h, rc);
    return DMHTTP_AGAIN;
}
static dmhttp_result_t read_chunk_crlf(dmhttp_t h)
{
    dmhttp_result_t rc;

    rc = receive(h);
    if (rc == DMHTTP_DONE)
        rc = DMHTTP_ERR_TRUNCATED;
    if (rc < 0)
        return http_fail(h, rc);
    if (rc != DMHTTP_OK)
        return rc;
    if (h->input[h->input_at++] != (h->chunk_crlf == 0 ? '\r' : '\n'))
        return http_fail(h, DMHTTP_ERR_PROTOCOL);
    if (++h->chunk_crlf == 2)
    {
        h->chunk_crlf = 0;
        h->framing = HTTP_CHUNK_SIZE;
    }
    return DMHTTP_AGAIN;
}
static dmhttp_result_t read_payload(dmhttp_t h, void *output, size_t capacity, size_t *received)
{
    dmhttp_result_t rc;
    rc = receive(h);
    if (rc == DMHTTP_DONE)
        return h->framing == HTTP_CLOSE ? finish(h) : http_fail(h, DMHTTP_ERR_TRUNCATED);
    if (rc < 0)
        return http_fail(h, rc);
    if (rc != DMHTTP_OK)
        return rc;
    size_t n = h->input_size - h->input_at;
    if (n > capacity)
        n = capacity;
    if (h->framing != HTTP_CLOSE && n > h->remaining)
        n = (size_t)h->remaining;
    if (h->response.body_bytes > UINT64_MAX - n)
        return http_fail(h, DMHTTP_ERR_LIMIT);
    if (h->options.max_body_bytes && n > h->options.max_body_bytes - h->response.body_bytes)
        return http_fail(h, DMHTTP_ERR_LIMIT);
    memcpy(output, h->input + h->input_at, n);
    h->input_at += n;
    h->response.body_bytes += n;
    *received = n;
    if (h->framing != HTTP_CLOSE)
    {
        h->remaining -= n;
        if (!h->remaining && h->framing == HTTP_CHUNK_DATA)
            h->framing = HTTP_CHUNK_CRLF;
    }
    return DMHTTP_OK;
}
static dmhttp_result_t read_body(dmhttp_t h, void *output, size_t capacity, size_t *received)
{
    dmhttp_result_t rc = check(h);
    if (rc != DMHTTP_OK)
        return rc;
    if (h->state == HTTP_CLOSING)
        return finish(h);
    if (h->state != HTTP_BODY)
        return DMHTTP_ERR_STATE;
    for (unsigned work = 0; work < HTTP_IO_SIZE; ++work)
    {
        if (h->framing == HTTP_LENGTH && !h->remaining)
            return finish(h);
        if (h->framing == HTTP_CHUNK_SIZE || h->framing == HTTP_TRAILERS)
            rc = read_chunk_line(h);
        else if (h->framing == HTTP_CHUNK_CRLF)
            rc = read_chunk_crlf(h);
        else
            return read_payload(h, output, capacity, received);
        if (rc != DMHTTP_AGAIN)
            return rc;
    }
    return DMHTTP_AGAIN;
}
dmod_dmhttp_api_declaration(1.0, dmhttp_result_t, _create,
                            (const char *url, const dmhttp_options_t *options, dmhttp_t *output))
{
    if (!output)
        return DMHTTP_ERR_ARGUMENT;
    *output = NULL;
    if (!options || !options->transport.open || !options->transport.close ||
        !options->transport.stream.send || !options->transport.stream.receive ||
        !options->timeout_ms || options->timeout_ms > INT32_MAX || options->max_redirects > 16 ||
        (options->method != DMHTTP_GET && options->method != DMHTTP_HEAD) ||
        (options->method == DMHTTP_HEAD && options->range_enabled))
        return DMHTTP_ERR_ARGUMENT;
    dmhttp_t h = Dmod_Malloc(sizeof(*h));
    if (!h)
        return DMHTTP_ERR_MEMORY;
    memset(h, 0, sizeof(*h));
    h->options = *options;
    h->magic = HTTP_MAGIC;
    dmhttp_result_t rc = http_url_parse(url, &h->url);
    if (rc == DMHTTP_OK && h->url.secure && !options->tls_config)
        rc = DMHTTP_ERR_TLS;
    if (rc == DMHTTP_OK)
        rc = http_request_build(h);
    if (rc != DMHTTP_OK)
    {
        Dmod_Free(h);
        return rc;
    }
    h->started = dmosi_get_tick_count();
    h->state = HTTP_CONNECT;
    atomic_fetch_add_explicit(&live_requests, 1, memory_order_relaxed);
    *output = h;
    return DMHTTP_OK;
}
dmod_dmhttp_api_declaration(1.0, dmhttp_result_t, _step, (dmhttp_t h))
{
    if (!valid(h))
        return DMHTTP_ERR_ARGUMENT;
    if (h->busy)
        return DMHTTP_ERR_STATE;
    h->busy = true;
    dmhttp_result_t rc = step(h);
    h->busy = false;
    return rc;
}
dmod_dmhttp_api_declaration(1.0, dmhttp_result_t, _read,
                            (dmhttp_t h, void *buffer, size_t capacity, size_t *received))
{
    if (!received)
        return DMHTTP_ERR_ARGUMENT;
    *received = 0;
    if (!valid(h) || !buffer || !capacity)
        return DMHTTP_ERR_ARGUMENT;
    if (h->busy)
        return DMHTTP_ERR_STATE;
    h->busy = true;
    dmhttp_result_t rc = read_body(h, buffer, capacity, received);
    h->busy = false;
    return rc;
}
dmod_dmhttp_api_declaration(1.0, dmhttp_result_t, _response,
                            (dmhttp_t h, dmhttp_response_t *output))
{
    if (!valid(h) || !output)
        return DMHTTP_ERR_ARGUMENT;
    *output = h->response;
    return DMHTTP_OK;
}
dmod_dmhttp_api_declaration(1.0, dmhttp_result_t, _cancel, (dmhttp_t h))
{
    if (!valid(h))
        return DMHTTP_ERR_ARGUMENT;
    if (h->busy)
        return DMHTTP_ERR_STATE;
    if (h->state == HTTP_FINISHED)
        return DMHTTP_DONE;
    if (h->state == HTTP_FAILED)
        return h->response.error;
    h->busy = true;
    dmhttp_result_t rc = http_fail(h, DMHTTP_ERR_CANCELLED);
    h->busy = false;
    return rc;
}
dmod_dmhttp_api_declaration(1.0, void, _destroy, (dmhttp_t h))
{
    if (valid(h) && !h->busy)
    {
        h->busy = true;
        http_release(h);
        h->magic = 0;
        Dmod_Free(h);
        atomic_fetch_sub_explicit(&live_requests, 1, memory_order_relaxed);
    }
}
int dmod_init(const Dmod_Config_t *config)
{
    (void)config;
    return 0;
}
int dmod_deinit(void)
{
    return atomic_load_explicit(&live_requests, memory_order_relaxed) ? -EBUSY : 0;
}
