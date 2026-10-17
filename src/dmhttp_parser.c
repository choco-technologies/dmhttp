#include "dmhttp_internal.h"

bool http_equal(const char *a, const char *b)
{
    while (*a && *b)
    {
        unsigned char x = *a++, y = *b++;
        if (x >= 'A' && x <= 'Z')
            x += 32;
        if (y >= 'A' && y <= 'Z')
            y += 32;
        if (x != y)
            return false;
    }
    return *a == *b;
}
bool http_token(unsigned char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '!' ||
           c == '#' || c == '$' || c == '%' || c == '&' || c == '\'' || c == '*' || c == '+' ||
           c == '-' || c == '.' || c == '^' || c == '_' || c == '`' || c == '|' || c == '~';
}
static bool decimal(const char *text, uint64_t *out)
{
    uint64_t v = 0;
    if (!*text)
        return false;
    while (*text)
    {
        unsigned c = (unsigned char)*text++;
        if (c < '0' || c > '9' || v > (UINT64_MAX - (c - '0')) / 10)
            return false;
        v = v * 10 + c - '0';
    }
    *out = v;
    return true;
}
static dmhttp_result_t field_copy(char *out, size_t capacity, const char *value)
{
    size_t n = strlen(value);
    if (n >= capacity)
        return DMHTTP_ERR_LIMIT;
    memcpy(out, value, n + 1);
    return DMHTTP_OK;
}
static dmhttp_result_t status_line(dmhttp_t h)
{
    char *line = h->line;

    size_t n = strlen(line);
    if (n < 13 || (memcmp(line, "HTTP/1.1 ", 9) != 0 && memcmp(line, "HTTP/1.0 ", 9) != 0) ||
        line[9] < '1' || line[9] > '5' || line[10] < '0' || line[10] > '9' || line[11] < '0' ||
        line[11] > '9' || line[12] != ' ')
        return DMHTTP_ERR_PROTOCOL;
    for (size_t i = 13; i < n; ++i)
        if (((unsigned char)line[i] < 32 && line[i] != '\t') || (unsigned char)line[i] == 127)
            return DMHTTP_ERR_PROTOCOL;
    h->response.status_code = (line[9] - '0') * 100 + (line[10] - '0') * 10 + line[11] - '0';
    h->status_seen = true;
    return DMHTTP_OK;
}
dmhttp_result_t http_header_line(dmhttp_t h, bool trailer)
{
    char *line = h->line;
    if (!trailer && !h->status_seen)
        return status_line(h);
    char *colon = line;
    while (*colon && *colon != ':')
    {
        if (!http_token((unsigned char)*colon))
            return DMHTTP_ERR_PROTOCOL;
        ++colon;
    }
    if (colon == line || *colon != ':')
        return DMHTTP_ERR_PROTOCOL;
    *colon++ = 0;
    while (*colon == ' ' || *colon == '\t')
        ++colon;
    size_t n = strlen(colon);
    while (n && (colon[n - 1] == ' ' || colon[n - 1] == '\t'))
        colon[--n] = 0;
    for (size_t i = 0; i < n; ++i)
        if (((unsigned char)colon[i] < 32 && colon[i] != '\t') || (unsigned char)colon[i] == 127)
            return DMHTTP_ERR_PROTOCOL;
    if (trailer)
    {
        if (http_equal(line, "Content-Length") || http_equal(line, "Transfer-Encoding") ||
            http_equal(line, "Host") || http_equal(line, "Content-Encoding") ||
            http_equal(line, "Content-Range") || http_equal(line, "Trailer"))
            return DMHTTP_ERR_PROTOCOL;
        return DMHTTP_OK;
    }
    if (http_equal(line, "Content-Length"))
    {
        uint64_t value;
        if (!decimal(colon, &value))
            return DMHTTP_ERR_PROTOCOL;
        if (h->length_seen && value != h->response.content_length)
            return DMHTTP_ERR_PROTOCOL;
        h->length_seen = true;
        h->response.content_length_known = true;
        h->response.content_length = value;
    }
    else if (http_equal(line, "Transfer-Encoding"))
    {
        if (h->transfer_seen || !http_equal(colon, "chunked"))
            return DMHTTP_ERR_PROTOCOL;
        h->transfer_seen = true;
    }
    else if (http_equal(line, "Content-Encoding"))
    {
        if (!http_equal(colon, "identity"))
            return DMHTTP_ERR_PROTOCOL;
    }
    else if (http_equal(line, "Location"))
    {
        if (h->location_seen)
            return DMHTTP_ERR_PROTOCOL;
        h->location_seen = true;
        return field_copy(h->location, sizeof(h->location), colon);
    }
    else if (http_equal(line, "Content-Type"))
        return field_copy(h->response.content_type, sizeof(h->response.content_type), colon);
    else if (http_equal(line, "Content-Range"))
    {
        if (h->response.content_range[0])
            return DMHTTP_ERR_PROTOCOL;
        return field_copy(h->response.content_range, sizeof(h->response.content_range), colon);
    }
    return DMHTTP_OK;
}
static dmhttp_result_t redirect_request(dmhttp_t h)
{

    if (h->response.redirects >= h->options.max_redirects)
        return DMHTTP_ERR_REDIRECT;
    struct http_url *next = Dmod_Malloc(sizeof(*next));
    if (!next)
        return DMHTTP_ERR_MEMORY;
    dmhttp_result_t rc = http_url_resolve(&h->url, h->location, next);
    if (rc == DMHTTP_OK && h->url.secure && !next->secure)
        rc = DMHTTP_ERR_REDIRECT;
    if (rc == DMHTTP_OK && next->secure && !h->options.tls_config)
        rc = DMHTTP_ERR_TLS;
    if (rc == DMHTTP_OK)
        h->url = *next;
    Dmod_Free(next);
    if (rc != DMHTTP_OK)
        return rc;
    h->state = HTTP_REDIRECT;
    return DMHTTP_AGAIN;
}
dmhttp_result_t http_redirect_restart(dmhttp_t h)
{
    dmhttp_result_t rc;
    unsigned redirects = h->response.redirects + 1;
    http_release(h);
    memset(&h->response, 0, sizeof(h->response));
    h->response.redirects = redirects;
    h->header_bytes = 0;
    h->status_seen = h->length_seen = h->transfer_seen = h->location_seen = false;
    h->location[0] = 0;
    h->input_at = h->input_size = 0;
    h->informational = 0;
    rc = http_request_build(h);
    if (rc != DMHTTP_OK)
        return rc;
    h->state = HTTP_CONNECT;
    return DMHTTP_AGAIN;
}
dmhttp_result_t http_headers_done(dmhttp_t h)
{
    if (!h->status_seen || (h->length_seen && h->transfer_seen))
        return DMHTTP_ERR_PROTOCOL;
    unsigned code = h->response.status_code;
    if (code < 200)
    {
        if (code == 101 || h->length_seen || h->transfer_seen || ++h->informational > 8)
            return DMHTTP_ERR_PROTOCOL;
        unsigned redirects = h->response.redirects;
        memset(&h->response, 0, sizeof(h->response));
        h->response.redirects = redirects;
        h->status_seen = false;
        h->location_seen = false;
        h->location[0] = 0;
        return DMHTTP_AGAIN;
    }
    if ((code == 301 || code == 302 || code == 303 || code == 307 || code == 308) &&
        h->location_seen && h->options.max_redirects)
        return redirect_request(h);
    if (code == 204 && (h->length_seen || h->transfer_seen))
        return DMHTTP_ERR_PROTOCOL;
    if (h->options.method == DMHTTP_HEAD || code == 204 || code == 304)
    {
        h->remaining = 0;
        h->framing = HTTP_LENGTH;
    }
    else if (h->transfer_seen)
        h->framing = HTTP_CHUNK_SIZE;
    else if (h->length_seen)
    {
        if (h->options.max_body_bytes && h->response.content_length > h->options.max_body_bytes)
            return DMHTTP_ERR_LIMIT;
        h->remaining = h->response.content_length;
        h->framing = HTTP_LENGTH;
    }
    else
        h->framing = HTTP_CLOSE;
    h->state = HTTP_BODY;
    return DMHTTP_OK;
}
