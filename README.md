# dmhttp

A bounded-memory HTTP/1.1 download client for DMOD. It supports GET and HEAD,
HTTP and verified HTTPS through dmtls, streamed response bodies, chunked framing,
redirects, finite transaction deadlines, cancellation and optional byte ranges.
There is no libcurl dependency, hardware port or driver layer.

The application supplies a nonblocking connector (DNS/connect, send, receive,
close), initializes dmtls with strong entropy and UTC, and supplies a trust-store
configuration. dmhttp opens/closes each connector and creates/destroys its TLS
connection. It never owns the application adapter or TLS configuration.

## Build

```sh
cmake -S . -B build/native
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

A local SDK can be selected with `-DDMOD_DIR=/path/to/dmod`. Supported build
profiles are `arch/x86_64`, `arch/armv7/cortex-m4` and `arch/armv7/cortex-m7`;
select with `-DDMOD_TOOLS_NAME=...`. Module dependencies use the packager's
version selection. The existing SDK/loader and 240-byte DMF format are unchanged.

## Use

1. Fill `dmhttp_options_t` with a connector and a nonzero timeout. For HTTPS,
   provide a dmtls configuration with explicit trust anchors. The TLS owner
   supplies strong entropy and a working dmutc source before starting requests.
2. Call `dmhttp_create(url, &options, &request)`.
3. Advance `dmhttp_step()` until `DMHTTP_OK`: final response headers are ready.
4. Inspect `dmhttp_response()`, including status and, for resumed downloads,
   `Content-Range`. Read the body with `dmhttp_read()` into your own buffers.
5. Commit a downloaded file only after `DMHTTP_DONE`; destroy the handle on all
   paths. HTTP error statuses remain available to the application.

On `DMHTTP_WANT_READ`/`DMHTTP_WANT_WRITE`, wait for the adapter's readiness event
or transaction deadline. Drain already-buffered adapter data before waiting for
a new edge notification. On `DMHTTP_AGAIN`, schedule another bounded execution
step. The library never sleeps or polls the network. Caller backpressure is
natural: when it stops reading, dmhttp stops consuming transport bytes.

See [API and contracts](docs/api.md) and [validation](docs/testing.md).

## Scope

Requests send `Connection: close` and `Accept-Encoding: identity`. Responses are
not transparently decompressed. Unsupported transfer/content encodings fail.
Redirects are bounded, resolved relative to the current URL, create a fresh
connection and repeat certificate/hostname validation. HTTPS-to-HTTP redirects
are rejected. No credentials, cookies or arbitrary request headers are forwarded.

URL input supports ASCII DNS/IPv4 hosts, ports and escaped path/query components;
fragments are omitted. IPv6 literals, authentication, uploads, proxy tunnelling,
HTTP/2, connection pooling and caching are outside this interface.

License: MIT. dmtls and its private crypto backend are separate dependencies.
