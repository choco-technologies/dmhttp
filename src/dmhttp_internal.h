#ifndef DMHTTP_INTERNAL_H
#define DMHTTP_INTERNAL_H
#include "dmod.h"
#include "dmhttp.h"
#include "dmosi.h"
#include <string.h>
#include <limits.h>
#define HTTP_IO_SIZE 1024u
#define HTTP_LINE_MAX 2048u
struct http_url
{
    char host[DMHTTP_HOST_MAX + 1];
    char target[DMHTTP_URL_MAX + 1];
    uint16_t port;
    bool secure;
};
enum http_state
{
    HTTP_CONNECT,
    HTTP_TLS,
    HTTP_SEND,
    HTTP_HEADERS,
    HTTP_BODY,
    HTTP_CLOSING,
    HTTP_REDIRECT,
    HTTP_FINISHED,
    HTTP_FAILED
};
enum http_framing
{
    HTTP_LENGTH,
    HTTP_CLOSE,
    HTTP_CHUNK_SIZE,
    HTTP_CHUNK_DATA,
    HTTP_CHUNK_CRLF,
    HTTP_TRAILERS
};
struct dmhttp
{
    uint32_t magic;
    dmhttp_options_t options;
    struct http_url url;
    dmhttp_response_t response;
    enum http_state state;
    enum http_framing framing;
    dmtls_connection_t tls;
    uint32_t started;
    bool opened;
    bool busy;
    char request[DMHTTP_URL_MAX + 512];
    size_t request_size, request_sent;
    char line[HTTP_LINE_MAX + 1];
    size_t line_size, header_bytes, trailer_bytes;
    bool status_seen, length_seen, transfer_seen, location_seen;
    unsigned informational;
    char location[DMHTTP_URL_MAX + 1];
    uint64_t remaining;
    unsigned chunk_crlf;
    uint8_t input[HTTP_IO_SIZE];
    size_t input_at, input_size;
};
dmhttp_result_t http_url_parse(const char *, struct http_url *);
dmhttp_result_t http_url_resolve(const struct http_url *, const char *, struct http_url *);
dmhttp_result_t http_request_build(dmhttp_t);
dmhttp_result_t http_header_line(dmhttp_t, bool trailer);
dmhttp_result_t http_headers_done(dmhttp_t);
dmhttp_result_t http_redirect_restart(dmhttp_t);
bool http_equal(const char *, const char *);
bool http_token(unsigned char);
void http_release(dmhttp_t);
dmhttp_result_t http_fail(dmhttp_t, dmhttp_result_t);
#endif
