# Build and validation

Build profiles: x86-64, Cortex-M4 and Cortex-M7, with the existing SDK/loader.
A read-only final-ELF audit requires absolute pointers to use the existing
header/API/GOT sections; it does not rewrite binaries or add runtime fixups.
The normal DMF header remains 240 bytes.

## Native

```sh
cmake -S . -B build/native
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

`test_dmhttp` runs the actual module through dmod_loader (3 checks): URL/TLS
validation, fragmented chunk decoding, and cancellation/connector ownership.

`http_interoperability` compiles production HTTP and real dmtls/crypto sources
with host SAL/OSI/time providers. Independent OpenSSL peers exercise HTTP,
TLS 1.2/1.3 HTTPS, close_notify, hostname/CA rejection, HTTPS redirects and
downgrade rejection.
Scripted streams split headers, chunk sizes/CRLF, trailers and body bytes,
interleave WANT results and inject malformed/truncated framing. Other checks
cover redirect resolution/limits (including informational responses), HEAD/204/304,
206 metadata, pending connectors, cancellation/deadlines, module lifetime,
header/body limits, overflow and a 2 MiB streamed binary body. All 29 tests pass.
Tests verify final allocation cleanup after TLS shutdown.

Host tests fetch dmtls main solely for source. Production module dependency
versions remain unpinned. Standard FetchContent overrides select local sources;
DMOD_DMM_URL can select a local package manifest. CI requires no developer paths.

## STM32F746G-DISCO

```sh
cmake -S . -B build/cortex-m7 -DDMOD_TOOLS_NAME=arch/armv7/cortex-m7 \
  -DDMOD_CPU_FAMILY=stm32f7 -DDMHTTP_BUILD_BOARD_PROBE=ON
cmake --build build/cortex-m7 --parallel
python3 tests/probe/make_fixtures.py build/board-fixtures
```

Install the local dmhttp/test_dmhttp/httpprobe/httpwireprobe packages in test
firmware, with dmtls, RNG, RTC and the running UTC library-service unit. Upload
only ca.pem/other.pem to the board, not private keys. Keep raw logs and generated
keys under ignored build paths or outside the repository.

`httpprobe URL SERVER_IPV4 CA_FILE` uses real dmtcp and a semaphore notified by
callbacks. The IP override separates transport tests from DNS provisioning.
A reachable HTTP/HTTPS server must listen on the URL's port. Results report
status, length, FNV-1a body hash, redirects and TLS verification flags.
The receive ring fails visibly on overflow; it never silently drops bytes.
This adapter is a diagnostic, not a public DNS/TCP interface.

`httpwireprobe` uses UART byte queues against a PC OpenSSL peer:

```sh
python3 tests/probe/run_wire.py --boot-dir /path/to/dmod-boot \
  --fixtures build/board-fixtures
```

This executes the real MCU HTTP/TLS/RNG/UTC engines and checks decoded body
length/hash and redirects. It does not cover Ethernet/dmtcp. The UART fixture
has a finite 90-second total deadline to include serial handshakes/redirects.
Native socket tests and the TCP diagnostic use shorter finite limits.

Board module checks passed 3/3. Ethernet validation is pending a server address
reachable from the board; the isolated build container was unreachable from
its LAN. UART interoperability is tracked separately and does not substitute
for the missing network test. Renode is omitted as requested.
