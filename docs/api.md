# API and ownership

Public declarations and exact return values live in `include/dmhttp.h`.

A request owns a copied URL/options structure and fixed parser/I/O buffers.
It borrows adapter user data and the TLS trust-store configuration until destroy.
One handle uses one connector; simultaneous requests need distinct connector
instances. Serialize all operations on the same handle. Callbacks must not
reenter it; operation reentry is rejected. Destroy during a callback is ignored.

`transport.open(user, hostname, port)` starts or advances nonblocking name
resolution and TCP connection establishment. It returns dmtls-style OK/WANT/error
results. A repeat after WANT uses the same endpoint. `send`/`receive` are the
same contract as dmtls: OK reports positive progress, receive OK/0 is EOF,
WANT reports zero bytes. `close` must synchronously cancel pending work, release
the stream and detach callbacks. It is called exactly once per attempted open,
including a failed/pending open. A redirect can then start a new open on that
same adapter. The connector supplies readiness notification to its owner.

For DMOD applications, callback addresses must use the existing GOT mechanism.
Keep each persistent function pointer in a typed `const volatile` variable
annotated with `DMOD_GLOBAL_POINTER`, then assign it into a zero-initialized
options structure at runtime. For example:

```c
static dmhttp_open_fn const volatile open_callback DMOD_GLOBAL_POINTER = app_open;
/* Use the same pattern for close/send/receive. */
dmhttp_options_t options = {0};
options.transport.open = open_callback;
```

The complete pattern is in `tests/probe/httpprobe.c`. A mixed static aggregate
containing callbacks is not covered by the original loader's GOT relocation.


The diagnostic applications reserve 16 KiB of stack; budget caller stack for
HTTP parsing and TLS arithmetic, rather than invoking them on a small network
callback stack.

The application schedules the absolute transaction deadline as well as stream
readiness; it cannot wait forever for an absent peer event. `timeout_ms` is
nonzero, at most INT32_MAX, and starts at create. It includes all redirects,
handshakes, transfer and time spent paused by the application. There is no
library worker thread. Each parser call processes at most 1024 framing bytes
before yielding AGAIN. Body reads copy at most one 1024-byte input buffer.

Headers are bounded to 8192 bytes per response hop, including informational
responses; individual lines to 2048 bytes. At most eight informational responses
and sixteen redirects are accepted. Trailers have a separate 8192-byte budget;
chunk extensions are line-bounded. Header parsing requires CRLF, valid field
names and values, and rejects folded fields, conflicting lengths, CL+TE,
unsupported transfer codings, upgrades and framing fields in trailers.
Identical duplicate Content-Length values are accepted; comma lists are not.

Bodies use Content-Length, decoded chunked framing, or EOF framing when neither
is given. HEAD, 204 and 304 have no body. Known lengths and chunk sizes are
checked for overflow and optional body limits before consuming data. TCP EOF
before a declared body/chunk/trailer is complete is an error. For HTTPS,
close-delimited completion requires TLS close_notify; an abrupt TCP EOF fails.
A framed body completes when its HTTP framing is complete, independently of the
peer's subsequent connection closure. Before normal completion and redirects, dmhttp sends TLS close_notify, yielding
on WANT without blocking. The connector is then released; its close operation
should preserve queued bytes on an orderly TCP close. Cancellation, destruction
and errors abort immediately. Module deinitialization returns BUSY while any
request handle remains alive.

Callers must check HTTP status explicitly. A completed 404 body is a successful
protocol transfer. Range requests send bytes=N-; applications must validate a
206 status and Content-Range before appending, and restart on a 200 response.
Metadata strings that exceed the public response buffers fail with LIMIT.

URL parsing accepts http/https schemes case-insensitively and no userinfo.
DNS/IPv4 hosts are lowercased. Query/path percent escapes are validated but not
decoded. Relative redirect references use RFC 3986 path merging and dot-segment
removal; unsupported schemes and HTTPS downgrade fail. Since no application
credentials/custom headers are accepted, redirects cannot forward those secrets
across origins. An application that restricts network destinations must enforce
its policy in the connector before every open.

`max_body_bytes=0` explicitly permits unlimited streaming; payload size does not
change the library's fixed per-request allocation. TLS and adapter queues have
separate memory budgets. The real TCP test adapter has a bounded receive ring
and fails on overflow; dmtcp currently has no consumer-controlled receive window.
An adapter must never silently discard acknowledged bytes when its queue fills.

Protocol references: [HTTP semantics](https://www.rfc-editor.org/rfc/rfc9110),
[HTTP/1.1 framing](https://www.rfc-editor.org/rfc/rfc9112),
[URI resolution](https://www.rfc-editor.org/rfc/rfc3986).

## Public functions

| Function | Contract |
|----------|----------|
| dmhttp_create(url, options, output) | Copies URL/options; returns OK with a new handle, otherwise sets output to NULL |
| dmhttp_step(handle) | Advances connection, redirects and final headers; OK means inspect response and start reading |
| dmhttp_read(handle, buffer, capacity, received) | Streams body after headers; requires capacity greater than zero; DONE means completion |
| dmhttp_response(handle, output) | Copies the current metadata and diagnostics; it does not advance the request |
| dmhttp_cancel(handle) | Aborts pending work; returns ERR_CANCELLED, or preserves an existing terminal result |
| dmhttp_destroy(handle) | Releases the handle and any pending connection; accepts NULL |

Destroy every successfully created handle. Module unload is BUSY while handles
remain alive. Both TLS configuration and adapter user data must outlive the
handle. For HTTPS initialization and trust configuration, see `dmf-man dmtls`.

## Local results versus HTTP statuses

`dmhttp_result_t` is the local operation result, not an HTTP status code.
OK=0, WANT_READ=1, WANT_WRITE=2, AGAIN=3 and DONE=4 describe request progress.
Check WANT on both step and read. AGAIN needs another scheduled call; WANT
needs connector readiness or a deadline wakeup. DONE is a complete transfer,
including normal connection shutdown, and is repeated by subsequent reads.

`dmhttp_status_t` supplies named HTTP codes with standard wire values, including
STATUS_OK=200, STATUS_PARTIAL_CONTENT=206 and STATUS_NOT_FOUND=404 (all prefixed
DMHTTP_). The authoritative registry is
https://www.iana.org/assignments/http-status-codes/ . Reserved, obsolete and
temporary assignments are not named constants. Naming a status does not add
support for its feature: e.g. protocol upgrades still fail. The unsigned
response.status_code preserves extension values and remains 0 until a status
line is received. Inspect it after step returns OK for the final response.

HTTP 4xx and 5xx responses remain readable, including their error bodies.
For example, receiving all of a 404 response returns DONE with status_code=404
and error=OK. A malformed or truncated 404 body instead fails with the appropriate
local error while retaining the HTTP status. Do not interpret DONE as HTTP 200.

## Local errors

All names below have the DMHTTP_ERR_ prefix.

| Name | Value | Meaning |
|------|-------|---------|
| ARGUMENT | -1 | Missing required argument, invalid method/option, zero read capacity |
| MEMORY | -2 | Allocation failed |
| STATE | -3 | Wrong request phase or reentrant operation |
| URL | -4 | Invalid URL or unsupported URL form |
| TRANSPORT | -5 | Connector failure or violated callback byte-count contract |
| TLS | -6 | TLS setup, handshake, verification or record failure; inspect tls_error |
| PROTOCOL | -7 | Invalid HTTP syntax, unsupported framing or encoding |
| LIMIT | -8 | Header/line/metadata/body bound or numeric overflow |
| TRUNCATED | -9 | HTTP body/framing ended prematurely |
| TIMEOUT | -10 | Total transaction deadline or TLS timeout elapsed |
| CANCELLED | -11 | Request was cancelled |
| REDIRECT | -12 | Redirect limit or HTTPS downgrade rejection |

A lower-layer TLS truncation is reported as ERR_TLS with the corresponding
DMTLS_ERR_TRUNCATED in response.tls_error. The response also exposes stable
DMTLS_VERIFY_* flags for trust, hostname and certificate-time failures.
