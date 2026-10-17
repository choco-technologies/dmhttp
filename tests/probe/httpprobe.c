/* Real dmtcp diagnostic. IP override isolates HTTP/TLS from DNS provisioning.
 * TCP callbacks wake the waiting application; no network polling loop. */
#include "dmod.h"
#include "dmhttp.h"
#include "dmtcp.h"
#include "dmdns.h"
#include "dmrng.h"
#include "dmosi.h"
#include <string.h>
#define RX_CAPACITY 32768u
struct adapter
{
    dmip_addr_t ip;
    dmtcp_conn_t conn;
    dmosi_mutex_t lock;
    dmosi_semaphore_t ready;
    uint8_t rx[RX_CAPACITY];
    size_t head, count;
    bool started, connected, eof, failed;
};
static void wake(struct adapter *a)
{
    dmosi_semaphore_post(a->ready, 1);
}
static void established(dmtcp_conn_t conn, void *user)
{
    (void)conn;
    struct adapter *a = user;
    dmosi_mutex_lock(a->lock);
    a->connected = true;
    wake(a);
    dmosi_mutex_unlock(a->lock);
}
static void data(dmtcp_conn_t conn, const uint8_t *bytes, size_t n, void *user)
{
    (void)conn;
    struct adapter *a = user;
    dmosi_mutex_lock(a->lock);
    if (!n)
        a->eof = true;
    else if (n > RX_CAPACITY - a->count)
        a->failed = true;
    else
        for (size_t i = 0; i < n; ++i)
        {
            a->rx[(a->head + a->count) % RX_CAPACITY] = bytes[i];
            ++a->count;
        }
    wake(a);
    dmosi_mutex_unlock(a->lock);
}
static void writable(dmtcp_conn_t conn, size_t n, void *user)
{
    (void)conn;
    (void)n;
    wake(user);
}
static void closed(dmtcp_conn_t conn, void *user)
{
    (void)conn;
    struct adapter *a = user;
    dmosi_mutex_lock(a->lock);
    a->conn = NULL;
    a->eof = true;
    wake(a);
    dmosi_mutex_unlock(a->lock);
}
static void reset(dmtcp_conn_t conn, void *user)
{
    (void)conn;
    struct adapter *a = user;
    dmosi_mutex_lock(a->lock);
    a->conn = NULL;
    a->failed = true;
    wake(a);
    dmosi_mutex_unlock(a->lock);
}
static void error(dmtcp_conn_t conn, int rc, void *user)
{
    (void)rc;
    reset(conn, user);
}
static dmtcp_established_handler_t const volatile established_cb DMOD_GLOBAL_POINTER = established;
static dmtcp_data_handler_t const volatile data_cb DMOD_GLOBAL_POINTER = data;
static dmtcp_writable_handler_t const volatile writable_cb DMOD_GLOBAL_POINTER = writable;
static dmtcp_closed_handler_t const volatile closed_cb DMOD_GLOBAL_POINTER = closed;
static dmtcp_reset_handler_t const volatile reset_cb DMOD_GLOBAL_POINTER = reset;
static dmtcp_error_handler_t const volatile error_cb DMOD_GLOBAL_POINTER = error;
static dmtls_result_t open_stream(void *user, const char *host, uint16_t port)
{
    (void)host;
    struct adapter *a = user;
    dmosi_mutex_lock(a->lock);
    if (!a->started)
    {
        a->started = true;
        dmtcp_conn_callbacks_t cb = {0};
        cb.on_established = established_cb;
        cb.on_data = data_cb;
        cb.on_writable = writable_cb;
        cb.on_closed = closed_cb;
        cb.on_reset = reset_cb;
        cb.on_error = error_cb;
        if (dmtcp_connect(&a->ip, port, 0, &cb, a, &a->conn) != 0)
            a->failed = true;
    }
    dmtls_result_t rc = a->failed ? DMTLS_ERR_TRANSPORT : a->connected ? DMTLS_OK : DMTLS_WANT_READ;
    dmosi_mutex_unlock(a->lock);
    return rc;
}
static void close_stream(void *user)
{
    struct adapter *a = user;
    dmosi_mutex_lock(a->lock);
    if (a->conn)
    {
        dmtcp_conn_set_callbacks(a->conn, NULL, NULL);
        dmtcp_abort(a->conn);
        a->conn = NULL;
    }
    a->started = a->connected = a->eof = a->failed = false;
    a->head = a->count = 0;
    dmosi_mutex_unlock(a->lock);
}
static dmtls_result_t send_stream(void *user, const uint8_t *bytes, size_t n, size_t *sent)
{
    struct adapter *a = user;
    *sent = 0;
    dmosi_mutex_lock(a->lock);
    int count = a->failed || !a->conn ? -1 : dmtcp_send(a->conn, bytes, n);
    dmosi_mutex_unlock(a->lock);
    if (count < 0)
        return DMTLS_ERR_TRANSPORT;
    if (!count)
        return DMTLS_WANT_WRITE;
    *sent = (size_t)count;
    return DMTLS_OK;
}
static dmtls_result_t recv_stream(void *user, uint8_t *bytes, size_t n, size_t *received)
{
    struct adapter *a = user;
    *received = 0;
    dmosi_mutex_lock(a->lock);
    dmtls_result_t rc = DMTLS_OK;
    if (a->failed)
        rc = DMTLS_ERR_TRANSPORT;
    else if (a->count)
    {
        if (n > a->count)
            n = a->count;
        for (size_t i = 0; i < n; ++i)
            bytes[i] = a->rx[(a->head + i) % RX_CAPACITY];
        a->head = (a->head + n) % RX_CAPACITY;
        a->count -= n;
        *received = n;
    }
    else if (!a->eof)
        rc = DMTLS_WANT_READ;
    dmosi_mutex_unlock(a->lock);
    return rc;
}
static int entropy(void *user, uint8_t *bytes, size_t n)
{
    (void)user;
    return dmrng_fill(bytes, n);
}
static dmhttp_open_fn const volatile open_cb DMOD_GLOBAL_POINTER = open_stream;
static dmhttp_close_fn const volatile close_cb DMOD_GLOBAL_POINTER = close_stream;
static dmtls_send_fn const volatile send_cb DMOD_GLOBAL_POINTER = send_stream;
static dmtls_recv_fn const volatile recv_cb DMOD_GLOBAL_POINTER = recv_stream;
static dmtls_entropy_fn const volatile entropy_cb DMOD_GLOBAL_POINTER = entropy;
int main(int argc, char *argv[])
{
    if (argc != 4)
    {
        Dmod_Printf("Usage: httpprobe URL SERVER_IPV4 CA_FILE\n");
        return 1;
    }
    struct adapter *a = Dmod_Malloc(sizeof(*a));
    if (!a)
        return 1;
    memset(a, 0, sizeof(*a));
    a->lock = dmosi_mutex_create(true);
    a->ready = dmosi_semaphore_create(0, 1);
    int rc = DMHTTP_ERR_ARGUMENT;
    dmtls_config_t config = NULL;
    dmhttp_t request = NULL;
    bool initialized = false;
    if (!a->lock || !a->ready || dmdns_parse_address(argv[2], &a->ip) != 0)
        goto cleanup;
    if (strncmp(argv[1], "https://", 8) == 0)
    {
        void *file = Dmod_FileOpen(argv[3], "rb");
        if (!file)
            goto cleanup;
        uint8_t ca[4096];
        size_t n = Dmod_FileRead(ca, 1, sizeof(ca), file);
        Dmod_FileClose(file);
        dmtls_platform_t platform = {0};
        platform.entropy = entropy_cb;
        platform.heap_limit = 512 * 1024;
        platform.max_connections = 1;
        rc = dmtls_initialize(&platform);
        if (rc != DMTLS_OK)
            goto cleanup;
        initialized = true;
        rc = dmtls_config_create(ca, n, &config);
        if (rc != DMTLS_OK)
            goto cleanup;
    }
    dmhttp_options_t options = {0};
    options.transport.open = open_cb;
    options.transport.close = close_cb;
    options.transport.stream.send = send_cb;
    options.transport.stream.receive = recv_cb;
    options.transport.stream.user = a;
    options.tls_config = config;
    options.timeout_ms = 20000;
    options.max_redirects = 3;
    options.max_body_bytes = 8 * 1024 * 1024;
    rc = dmhttp_create(argv[1], &options, &request);
    if (rc != DMHTTP_OK)
        goto cleanup;
    bool headers = false;
    uint32_t hash = 2166136261u, started = dmosi_get_tick_count();
    for (;;)
    {
        if (!headers)
        {
            rc = dmhttp_step(request);
            if (rc == DMHTTP_OK)
            {
                headers = true;
                continue;
            }
        }
        else
        {
            uint8_t block[1024];
            size_t n = 0;
            rc = dmhttp_read(request, block, sizeof(block), &n);
            for (size_t i = 0; i < n; ++i)
                hash = (hash ^ block[i]) * 16777619u;
        }
        if (rc < 0 || rc == DMHTTP_DONE)
            break;
        if (rc == DMHTTP_WANT_READ || rc == DMHTTP_WANT_WRITE)
        {
            uint32_t elapsed = dmosi_get_tick_count() - started;
            if (elapsed < options.timeout_ms)
                dmosi_semaphore_wait(a->ready, 1, (int32_t)(options.timeout_ms - elapsed));
        }
    }
    dmhttp_response_t response;
    dmhttp_response(request, &response);
    Dmod_Printf("HTTP_RESULT result=%d status=%u bytes=%u hash=%u redirects=%u tls=%d verify=%u\n",
                rc, response.status_code, (unsigned)response.body_bytes, hash, response.redirects,
                response.tls_error, response.verification_flags);
cleanup:
    if (request)
        dmhttp_destroy(request);
    if (config)
        dmtls_config_destroy(config);
    if (initialized)
        dmtls_shutdown();
    if (a->ready)
        dmosi_semaphore_destroy(a->ready);
    if (a->lock)
        dmosi_mutex_destroy(a->lock);
    Dmod_Free(a);
    return rc == DMHTTP_DONE ? 0 : 1;
}
