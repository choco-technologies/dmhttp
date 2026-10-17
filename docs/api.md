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
