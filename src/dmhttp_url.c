#include "dmhttp_internal.h"

static bool starts(const char *text, const char *prefix)
{
    while (*prefix)
        if (*text++ != *prefix++)
            return false;
    return true;
}
static bool scheme(const char *text, const char *prefix)
{
    while (*prefix)
    {
        unsigned char c = *text++;
        if (c >= 'A' && c <= 'Z')
            c += 32;
        if (c != (unsigned char)*prefix++)
            return false;
    }
    return true;
}
static bool hex(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
static dmhttp_result_t target_copy(const char *p, char *target)
{
    size_t n = 0;
    if (*p != '/' && *p != '?' && *p != '#' && *p)
        return DMHTTP_ERR_URL;
    if (*p != '/')
        target[n++] = '/';
    while (*p && *p != '#')
    {
        unsigned char c = (unsigned char)*p;
        if (c <= 32 || c >= 127 || c == '\\' || c == '"' || c == '<' || c == '>' || c == '[' ||
            c == ']' || c == '^' || c == '`' || c == '{' || c == '|' || c == '}')
            return DMHTTP_ERR_URL;
        if (c == '%' && (!p[1] || !p[2] || !hex(p[1]) || !hex(p[2])))
            return DMHTTP_ERR_URL;
        if (n == DMHTTP_URL_MAX)
            return DMHTTP_ERR_LIMIT;
        target[n++] = *p++;
    }
    target[n] = 0;
    return DMHTTP_OK;
}
dmhttp_result_t http_url_parse(const char *text, struct http_url *out)
{
    if (!text || !out)
        return DMHTTP_ERR_ARGUMENT;
    size_t total = 0;
    while (text[total])
        if (++total > DMHTTP_URL_MAX)
            return DMHTTP_ERR_LIMIT;
    memset(out, 0, sizeof(*out));
    if (scheme(text, "https://"))
    {
        out->secure = true;
        text += 8;
        out->port = 443;
    }
    else if (scheme(text, "http://"))
    {
        text += 7;
        out->port = 80;
    }
    else
        return DMHTTP_ERR_URL;
    size_t n = 0;
    while (*text && *text != ':' && *text != '/' && *text != '?' && *text != '#')
    {
        unsigned char c = (unsigned char)*text++;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '-'))
            return DMHTTP_ERR_URL;
        if (n == DMHTTP_HOST_MAX)
            return DMHTTP_ERR_LIMIT;
        out->host[n++] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
    }
    if (!n)
        return DMHTTP_ERR_URL;
    out->host[n] = 0;
    if (*text == ':')
    {
        unsigned port = 0;
        size_t digits = 0;
        ++text;
        while (*text >= '0' && *text <= '9')
        {
            if (++digits > 5)
                return DMHTTP_ERR_URL;
            port = port * 10 + (*text++ - '0');
        }
        if (!digits || !port || port > 65535)
            return DMHTTP_ERR_URL;
        out->port = (uint16_t)port;
    }
    return target_copy(text, out->target);
}
/* RFC 3986 remove-dot-segments on the path only; preserve duplicate slashes,
 * percent encoding and query bytes. Input/output may not alias. */
static void normalize(const char *source, char *output)
{
    size_t used = 0;
    while (*source && *source != '?')
    {
        if (starts(source, "/./"))
        {
            source += 2;
            continue;
        }
        if (strcmp(source, "/.") == 0 || starts(source, "/.?"))
        {
            source += 2;
            output[used++] = '/';
            break;
        }
        if (starts(source, "/../") || strcmp(source, "/..") == 0 || starts(source, "/..?"))
        {
            source += 3;
            if (used)
            {
                while (used && output[used - 1] != '/')
                    --used;
                if (used)
                    --used;
            }
            if (!*source || *source == '?')
            {
                output[used++] = '/';
                break;
            }
            continue;
        }
        output[used++] = *source++;
        while (*source && *source != '/' && *source != '?')
            output[used++] = *source++;
    }
    while (*source)
        output[used++] = *source++;
    if (!used)
        output[used++] = '/';
    output[used] = 0;
}
dmhttp_result_t http_url_resolve(const struct http_url *base, const char *ref, struct http_url *out)
{
    if (scheme(ref, "http://") || scheme(ref, "https://"))
    {
        dmhttp_result_t rc = http_url_parse(ref, out);
        if (rc != DMHTTP_OK)
            return rc;
        char clean[DMHTTP_URL_MAX + 1];
        normalize(out->target, clean);
        memcpy(out->target, clean, strlen(clean) + 1);
        return DMHTTP_OK;
    }
    if (starts(ref, "//"))
    {
        char full[DMHTTP_URL_MAX + 1];
        int n = Dmod_SnPrintf(full, sizeof(full), "%s:%s", base->secure ? "https" : "http", ref);
        if (n < 0 || (size_t)n >= sizeof(full))
            return DMHTTP_ERR_LIMIT;
        dmhttp_result_t rc = http_url_parse(full, out);
        if (rc != DMHTTP_OK)
            return rc;
        char clean[DMHTTP_URL_MAX + 1];
        normalize(out->target, clean);
        memcpy(out->target, clean, strlen(clean) + 1);
        return DMHTTP_OK;
    }
    *out = *base;
    char joined[DMHTTP_URL_MAX + 1], clean[DMHTTP_URL_MAX + 1];
    if (!*ref || *ref == '#')
        return DMHTTP_OK;
    if (*ref == '/')
    {
        dmhttp_result_t rc = target_copy(ref, clean);
        if (rc == DMHTTP_OK)
            normalize(clean, out->target);
        return rc;
    }
    size_t prefix = 0;
    while (base->target[prefix] && base->target[prefix] != '?')
        ++prefix;
    if (*ref != '?')
        while (prefix && base->target[prefix - 1] != '/')
            --prefix;
    /* A colon in the first relative segment denotes an unsupported scheme. */
    for (size_t i = 0; ref[i] && ref[i] != '/' && ref[i] != '?' && ref[i] != '#'; ++i)
        if (ref[i] == ':')
            return DMHTTP_ERR_URL;
    size_t length = strlen(ref);
    if (prefix + length > DMHTTP_URL_MAX)
        return DMHTTP_ERR_LIMIT;
    memcpy(joined, base->target, prefix);
    memcpy(joined + prefix, ref, length + 1);
    dmhttp_result_t rc = target_copy(joined, clean);
    if (rc != DMHTTP_OK)
        return rc;
    normalize(clean, out->target);
    return DMHTTP_OK;
}
dmhttp_result_t http_request_build(dmhttp_t h)
{
    char authority[DMHTTP_HOST_MAX + 8], range[64];
    range[0] = 0;
    int n;
    if (h->url.port == (h->url.secure ? 443 : 80))
        n = Dmod_SnPrintf(authority, sizeof(authority), "%s", h->url.host);
    else
        n = Dmod_SnPrintf(authority, sizeof(authority), "%s:%u", h->url.host,
                          (unsigned)h->url.port);
    if (n < 0 || (size_t)n >= sizeof(authority))
        return DMHTTP_ERR_LIMIT;
    if (h->options.range_enabled)
    {
        /* Avoid requiring 64-bit printf support in embedded SAL. */
        char digits[21];
        size_t at = sizeof(digits) - 1;
        uint64_t v = h->options.range_start;
        digits[at] = 0;
        do
        {
            digits[--at] = '0' + v % 10;
            v /= 10;
        } while (v);
        Dmod_SnPrintf(range, sizeof(range), "Range: bytes=%s-\r\n", digits + at);
    }
    n = Dmod_SnPrintf(
        h->request, sizeof(h->request),
        "%s %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nAccept-Encoding: identity\r\n%s\r\n",
        h->options.method == DMHTTP_HEAD ? "HEAD" : "GET", h->url.target, authority, range);
    if (n < 0 || (size_t)n >= sizeof(h->request))
        return DMHTTP_ERR_LIMIT;
    h->request_size = (size_t)n;
    h->request_sent = 0;
    return DMHTTP_OK;
}
