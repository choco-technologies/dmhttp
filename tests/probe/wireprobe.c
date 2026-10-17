/* Hardware interoperability diagnostic. TLS transport callbacks only touch
 * memory. Waiting for the host happens outside the library, never in BIO. */
#include "dmod.h"
#include "dmtls.h"
#include "dmhttp.h"
#include "dmrng.h"
#include "dmosi.h"
#include "dmtty_types.h"
#include <string.h>
#define CAPACITY 32768u
struct transport
{
    unsigned char rx[CAPACITY];
    size_t rx_pos, rx_size;
    unsigned char tx[CAPACITY];
    size_t tx_size;
};
static int entropy(void *user, uint8_t *out, size_t size)
{
    (void)user;
    return dmrng_fill(out, size);
}
static dmtls_entropy_fn const volatile entropy_source DMOD_GLOBAL_POINTER = entropy;
static dmtls_result_t send_bytes(void *user, const uint8_t *in, size_t size, size_t *sent)
{
    struct transport *t = user;
    *sent = 0;
    size_t n = CAPACITY - t->tx_size;
    if (n == 0)
        return DMTLS_WANT_WRITE;
    if (n > size)
        n = size;
    if (n > 257)
        n = 257;
    memcpy(t->tx + t->tx_size, in, n);
    t->tx_size += n;
    *sent = n;
    return DMTLS_OK;
}
static dmtls_result_t receive_bytes(void *user, uint8_t *out, size_t size, size_t *received)
{
    struct transport *t = user;
    *received = 0;
    size_t n = t->rx_size - t->rx_pos;
    if (n == 0)
        return DMTLS_WANT_READ;
    if (n > size)
        n = size;
    if (n > 113)
        n = 113;
    memcpy(out, t->rx + t->rx_pos, n);
    t->rx_pos += n;
    *received = n;
    return DMTLS_OK;
}
static void flush(struct transport *t)
{
    if (t->tx_size == 0)
        return;
    static const char digits[] = "0123456789abcdef";
    Dmod_Printf("HTTPWIRE_OUT ");
    char line[128];
    for (size_t pos = 0; pos < t->tx_size;)
    {
        size_t count = 0;
        while (pos < t->tx_size && count < sizeof(line))
        {
            line[count++] = digits[t->tx[pos] >> 4];
            line[count++] = digits[t->tx[pos] & 15];
            ++pos;
        }
        Dmod_FileWrite(line, 1, count, DMOD_STDOUT);
    }
    Dmod_Printf("\n");
    t->tx_size = 0;
}
static int unhex(int c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    return -1;
}
static bool exchange(struct transport *t)
{
    flush(t);
    Dmod_Printf("HTTPWIRE_INPUT\n");
    if (t->rx_pos != 0)
    {
        memmove(t->rx, t->rx + t->rx_pos, t->rx_size - t->rx_pos);
        t->rx_size -= t->rx_pos;
        t->rx_pos = 0;
    }
    int ch;
    do
    {
        ch = Dmod_Getc();
    } while (ch == '\r' || ch == '\n');
    if (ch != 'I')
        return false;
    int high = -1;
    while ((ch = Dmod_Getc()) != '\n' && ch != '\r' && ch != DMOD_EOF)
    {
        if (ch == ' ')
            continue;
        int nibble = unhex(ch);
        if (nibble < 0)
            return false;
        if (high < 0)
            high = nibble;
        else
        {
            if (t->rx_size == CAPACITY)
                return false;
            t->rx[t->rx_size++] = (unsigned char)((high << 4) | nibble);
            high = -1;
        }
    }
    return high < 0 && ch != DMOD_EOF;
}
static dmtls_result_t open_stream(void *user, const char *host, uint16_t port)
{
    struct transport *t = user;
    t->rx_pos = t->rx_size = t->tx_size = 0;
    Dmod_Printf("HTTPWIRE_OPEN %s %u\n", host, (unsigned)port);
    return DMTLS_OK;
}
static void close_stream(void *user)
{
    (void)user;
}
static dmhttp_open_fn const volatile open_cb DMOD_GLOBAL_POINTER = open_stream;
static dmhttp_close_fn const volatile close_cb DMOD_GLOBAL_POINTER = close_stream;
static dmtls_send_fn const volatile send_cb DMOD_GLOBAL_POINTER = send_bytes;
static dmtls_recv_fn const volatile recv_cb DMOD_GLOBAL_POINTER = receive_bytes;
int main(int argc, char *argv[])
{
    if (argc != 3)
    {
        Dmod_Printf("Usage: httpwireprobe URL CA_FILE\n");
        return 1;
    }
    struct transport *t = Dmod_Malloc(sizeof(*t));
    if (!t)
        return 1;
    memset(t, 0, sizeof(*t));
    void *file = Dmod_FileOpen(argv[2], "rb");
    if (!file)
    {
        Dmod_Free(t);
        return 1;
    }
    unsigned char ca[4096];
    size_t size = Dmod_FileRead(ca, 1, sizeof(ca), file);
    Dmod_FileClose(file);
    dmtls_platform_t platform = {0};
    platform.entropy = entropy_source;
    platform.heap_limit = 512 * 1024;
    platform.max_connections = 1;
    dmtls_config_t config = NULL;
    dmhttp_t request = NULL;
    int rc = dmtls_initialize(&platform);
    bool initialized = rc == DMTLS_OK;
    if (rc == DMTLS_OK)
        rc = dmtls_config_create(ca, size, &config);
    uint32_t flags = Dmod_Stdin_GetFlags();
    void *input = Dmod_LockStdio(DMOD_STDIN);
    if (input == NULL || Dmod_Ioctl(input, dmtty_ioctl_cmd_claim_foreground, NULL) != 0)
        rc = DMHTTP_ERR_TRANSPORT;
    Dmod_UnlockStdio(DMOD_STDIN);
    Dmod_Stdin_SetFlags(0);
    dmhttp_options_t options = {0};
    options.transport.open = open_cb;
    options.transport.close = close_cb;
    options.transport.stream.send = send_cb;
    options.transport.stream.receive = recv_cb;
    options.transport.stream.user = t;
    options.tls_config = config;
    options.timeout_ms = 90000;
    options.max_redirects = 3;
    if (rc == DMTLS_OK)
        rc = dmhttp_create(argv[1], &options, &request);
    bool headers = false;
    uint32_t hash = 2166136261u;
    while (rc >= 0 && rc != DMHTTP_DONE)
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
            unsigned char block[257];
            size_t n = 0;
            rc = dmhttp_read(request, block, sizeof(block), &n);
            for (size_t i = 0; i < n; ++i)
                hash = (hash ^ block[i]) * 16777619u;
        }
        if (rc == DMHTTP_WANT_READ || rc == DMHTTP_WANT_WRITE)
            if (!exchange(t))
            {
                rc = DMHTTP_ERR_TRANSPORT;
                break;
            }
    }
    dmhttp_response_t response = {0};
    if (request)
        dmhttp_response(request, &response);
    if (request)
        dmhttp_destroy(request);
    if (config)
        dmtls_config_destroy(config);
    if (initialized)
        dmtls_shutdown();
    Dmod_Stdin_SetFlags(flags);
    Dmod_Printf(
        "HTTPWIRE_DONE result=%d status=%u bytes=%u hash=%u redirects=%u tls=%d verify=%u\n", rc,
        response.status_code, (unsigned)response.body_bytes, hash, response.redirects,
        response.tls_error, response.verification_flags);
    Dmod_Free(t);
    return rc == DMHTTP_DONE ? 0 : 1;
}
