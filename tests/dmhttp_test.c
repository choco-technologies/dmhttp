#define DMOD_ENABLE_REGISTRATION ON
#include "dmod_test.h"
#include "dmhttp.h"
#include <string.h>
struct fixture
{
    size_t at;
    unsigned closes;
};
static dmtls_result_t open_stream(void *p, const char *host, uint16_t port)
{
    (void)p;
    return strcmp(host, "example.test") == 0 && port == 80 ? DMTLS_OK : DMTLS_ERR_TRANSPORT;
}
static void close_stream(void *p)
{
    ((struct fixture *)p)->closes++;
}
static dmtls_result_t send_stream(void *p, const uint8_t *bytes, size_t n, size_t *sent)
{
    (void)p;
    (void)bytes;
    *sent = n;
    return DMTLS_OK;
}
static dmtls_result_t recv_stream(void *p, uint8_t *out, size_t cap, size_t *n)
{
    struct fixture *f = p;
    static const char reply[] =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
    *n = 0;
    if (f->at == sizeof(reply) - 1)
        return DMTLS_OK;
    if (cap)
    {
        out[0] = reply[f->at++];
        *n = 1;
    }
    return DMTLS_OK;
}
static dmhttp_open_fn const volatile open_cb DMOD_GLOBAL_POINTER = open_stream;
static dmhttp_close_fn const volatile close_cb DMOD_GLOBAL_POINTER = close_stream;
static dmtls_send_fn const volatile send_cb DMOD_GLOBAL_POINTER = send_stream;
static dmtls_recv_fn const volatile recv_cb DMOD_GLOBAL_POINTER = recv_stream;
static void options(dmhttp_options_t *o, struct fixture *f)
{
    memset(o, 0, sizeof(*o));
    o->timeout_ms = 5000;
    o->transport.open = open_cb;
    o->transport.close = close_cb;
    o->transport.stream.send = send_cb;
    o->transport.stream.receive = recv_cb;
    o->transport.stream.user = f;
}
DMOD_TEST_STEP(http_validates_urls_and_tls_requirement)
{
    struct fixture f = {0};
    dmhttp_options_t o;
    options(&o, &f);
    dmhttp_t h = NULL;
    DMOD_TEST_EXPECT_EQ(dmhttp_create("http://user:pass@example.test/", &o, &h), DMHTTP_ERR_URL);
    DMOD_TEST_EXPECT_NULL(h);
    DMOD_TEST_EXPECT_EQ(dmhttp_create("https://example.test/", &o, &h), DMHTTP_ERR_TLS);
    DMOD_TEST_EXPECT_NULL(h);
}
DMOD_TEST_STEP(http_decodes_fragmented_chunked_response)
{
    struct fixture f = {0};
    dmhttp_options_t o;
    options(&o, &f);
    dmhttp_t h = NULL;
    DMOD_TEST_EXPECT_EQ(dmhttp_create("http://example.test/a", &o, &h), DMHTTP_OK);
    dmhttp_result_t rc;
    unsigned guard = 0;
    do
    {
        rc = dmhttp_step(h);
    } while (rc == DMHTTP_AGAIN && ++guard < 1000);
    DMOD_TEST_EXPECT_EQ(rc, DMHTTP_OK);
    char body[16] = {0};
    size_t size = 0, n = 0;
    do
    {
        rc = dmhttp_read(h, body + size, sizeof(body) - 1 - size, &n);
        size += n;
    } while ((rc == DMHTTP_OK || rc == DMHTTP_AGAIN) && ++guard < 1000);
    DMOD_TEST_EXPECT_EQ(rc, DMHTTP_DONE);
    DMOD_TEST_EXPECT_EQ(size, 5);
    DMOD_TEST_EXPECT_EQ(strcmp(body, "hello"), 0);
    dmhttp_response_t r;
    DMOD_TEST_EXPECT_EQ(dmhttp_response(h, &r), DMHTTP_OK);
    DMOD_TEST_EXPECT_EQ(r.status_code, 200);
    dmhttp_destroy(h);
    DMOD_TEST_EXPECT_EQ(f.closes, 1);
}
DMOD_TEST_STEP(http_cancel_is_terminal_and_releases_connector)
{
    struct fixture f = {0};
    dmhttp_options_t o;
    options(&o, &f);
    dmhttp_t h = NULL;
    DMOD_TEST_EXPECT_EQ(dmhttp_create("http://example.test/", &o, &h), DMHTTP_OK);
    DMOD_TEST_EXPECT_EQ(dmhttp_step(h), DMHTTP_AGAIN);
    DMOD_TEST_EXPECT_EQ(dmhttp_cancel(h), DMHTTP_ERR_CANCELLED);
    DMOD_TEST_EXPECT_EQ(dmhttp_step(h), DMHTTP_ERR_CANCELLED);
    dmhttp_destroy(h);
    DMOD_TEST_EXPECT_EQ(f.closes, 1);
}
