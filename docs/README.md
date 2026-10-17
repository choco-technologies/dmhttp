# dmhttp — HTTP and HTTPS download library

## Purpose

dmhttp is a DMOD library for fetching resources with HTTP/1.1 GET and HEAD.
It streams response bytes into application buffers without keeping the whole
file in RAM. HTTPS uses dmtls with certificate and hostname verification.
There is no command-line download program in this module.

The application provides a nonblocking network connector and schedules request
progress when the connector becomes ready. dmhttp handles request formatting,
response headers, chunk decoding, redirects, deadlines and connection cleanup.
It creates no thread and performs no network polling or sleeps.

## Requirements

Link your module with `dmod_link_modules(your_module dmhttp)`, then include
`dmhttp.h`. Install the module and documentation using the normal package tools:

```sh
dmf-get install dmhttp
dmf-get docs dmhttp
dmf-man dmhttp
dmf-man dmhttp api
dmf-man dmhttp testing
```

Runtime dependencies are dmosi and dmtls. The application must implement the
connector's DNS/connect, send, receive and close callbacks. Each concurrent
request requires its own connector state. The diagnostic TCP adapter shipped
with the source is an integration example, not a DNS service supplied by dmhttp.

For HTTPS, initialize dmtls with a strong entropy source and a working dmutc UTC
source. Create a dmtls configuration containing the appropriate trusted CA
certificates and pass it as `options.tls_config`. Keep the configuration alive
until all requests borrowing it have been destroyed. Plain HTTP does not require
TLS initialization; using an HTTPS URL without a TLS configuration fails.

## Configure a request

Zero-initialize `dmhttp_options_t`, then assign the connector callbacks and its
`transport.stream.user` state. Persistent callback pointers in DMOD applications
use the SDK `DMOD_GLOBAL_POINTER` macro; see `dmf-man dmhttp api` for the pattern.
Set these options before calling `dmhttp_create`:

| Option | Meaning |
|--------|---------|
| method | DMHTTP_GET (zero/default) or DMHTTP_HEAD |
| timeout_ms | Required total deadline, 1 through INT32_MAX milliseconds |
| max_redirects | 0 disables following; 1 through 16 limits followed redirects |
| max_body_bytes | Maximum decoded body size; 0 permits unlimited streaming |
| tls_config | Borrowed dmtls trust configuration, required for HTTPS |
| range_enabled | Send a byte-range request; GET only |
| range_start | First requested byte when range_enabled is true |

The URL and options are copied. Connector state and the TLS configuration remain
owned by the application. The deadline starts at create and includes DNS,
connection establishment, TLS, redirects, transfer and time paused by the caller.

## Request lifecycle

1. Call `dmhttp_create(url, &options, &request)`. Continue only on DMHTTP_OK.
2. Call `dmhttp_step(request)` until it returns DMHTTP_OK. Final response headers
   are now available. Handle readiness and scheduling results as described below.
3. Call `dmhttp_response(request, &response)` and check response.status_code.
   Decide whether to accept this status before writing to the destination file.
4. Call `dmhttp_read(request, buffer, capacity, &received)` to stream the decoded
   body. The buffer and capacity must be valid and nonzero. Consume exactly
   received bytes, which may contain binary data and are not NUL-terminated.
5. Complete the download only on DMHTTP_DONE. A later framing error can follow
   earlier body bytes, so write to a temporary file until the transfer completes.
6. Call `dmhttp_destroy(request)` on success, failure and cancellation paths.

Use `dmhttp_cancel(request)` to stop a pending request. It releases the connector
and returns DMHTTP_ERR_CANCELLED. Destroy the handle afterwards. Serialize calls
on a handle and do not call back into it from a transport callback.

## Operation results and HTTP status

The result returned by a library operation describes local progress:

| Result | Caller action |
|--------|---------------|
| DMHTTP_OK | Operation made progress; step has headers, read may have bytes |
| DMHTTP_WANT_READ | Resume when the connector is readable or the deadline fires |
| DMHTTP_WANT_WRITE | Resume when the connector is writable or the deadline fires |
| DMHTTP_AGAIN | Schedule another bounded call without waiting for a network event |
| DMHTTP_DONE | The body and normal connection cleanup are complete |
| DMHTTP_ERR_* | Transfer failed; inspect response diagnostics and destroy |

Always schedule the transaction deadline as well as readiness notifications.
Check data already buffered by the adapter before waiting for another event.
A lost or coalesced edge notification must not leave a readable request asleep.
TLS cryptography still consumes CPU during calls; use an appropriate worker
priority and stack budget rather than executing it on a network callback stack.

HTTP response codes are standard numbers in `response.status_code`, with named
constants in `dmhttp_status_t`. For example, DMHTTP_STATUS_OK is 200,
DMHTTP_STATUS_PARTIAL_CONTENT is 206 and DMHTTP_STATUS_NOT_FOUND is 404.
A complete 404 response can finish with DMHTTP_DONE: the exchange succeeded and
the server reported that the resource was missing. A negative DMHTTP_ERR_TLS or
DMHTTP_ERR_TRANSPORT instead means the exchange failed locally.

```c
dmhttp_response_t response;
if (dmhttp_response(request, &response) == DMHTTP_OK) {
    if (response.status_code == DMHTTP_STATUS_NOT_FOUND) {
        /* Report the missing resource; cancel or read its error body. */
    } else if (response.status_code == DMHTTP_STATUS_OK) {
        /* Read the body into a new temporary destination file. */
    }
}
```

Inspect the status after step returns DMHTTP_OK; during connection/redirect
progress it may be zero or refer to an intermediate response. The numeric field
also preserves extension status codes without requiring a new library version.

## Resuming downloads and redirects

To request bytes from offset N, set range_enabled and range_start=N. Append only
after confirming status 206 and a Content-Range matching the desired offset and
resource. A 200 response requires starting a new file. A 416 response is a server
range error, not proof that an existing local file is complete.

Redirects are followed only up to max_redirects. Each hop opens a fresh
connection; HTTPS repeats hostname and certificate validation. HTTPS-to-HTTP
redirects are rejected. With following disabled, inspect the 3xx status normally.

## Limits and troubleshooting

URLs support HTTP/HTTPS, ASCII DNS or IPv4 hosts, explicit ports and escaped
paths/queries. IPv6 literals and URL credentials are unsupported. Requests use
Connection: close and Accept-Encoding: identity. There are no uploads, cookies,
authentication, custom request headers, gzip decoding, proxy tunnelling or HTTP/2.

Header storage is bounded to 8192 bytes per response hop and 2048 bytes per line;
trailers have a separate 8192-byte budget. Body framing supports Content-Length,
chunked and connection-close delimiters. Premature EOF, conflicting lengths and
unsupported encodings fail. HTTPS close-delimited bodies require close_notify.

On failure, query dmhttp_response before destroy: error holds the local failure;
tls_error and verification_flags give TLS details. Check the CA store, hostname,
UTC and entropy source for certificate/TLS failures. DMHTTP_ERR_TIMEOUT covers
the entire transaction; DMHTTP_ERR_LIMIT indicates a configured or parser bound.
Use a transport adapter that reports overflow rather than dropping payload bytes.

Current STM32 validation found slow TLS handshakes with long certificate chains
and intermittent truncation against some public servers. This is tracked in
dmtls issue 3. See `dmf-man dmhttp testing` for measured coverage and limitations,
and `dmf-man dmhttp api` for callback contracts and error definitions.
