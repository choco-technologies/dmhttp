# Build and validation

Build profiles: x86-64, Cortex-M4 and Cortex-M7, with the existing SDK/loader.
No additional linker flags, binary rewriting or runtime fixups are required.
The library stores callbacks in request instances; diagnostic applications use
the SDK DMOD_GLOBAL_POINTER macro for their persistent callback addresses.

## Native

```sh
cmake -S . -B build/native
cmake --build build/native --parallel
ctest --test-dir build/native --output-on-failure
```

`test_dmhttp` runs the actual module through dmod_loader (4 checks): URL/TLS
validation, fragmented chunk decoding, HTTP 404 status/body handling, and
cancellation/connector ownership.

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

Recorded on STM32F746G-DISCO with the existing loader:

- Actual module checks before the review additions: 3/3.
- UART/OpenSSL engine scenarios: 9/9 (HTTP framing/redirects, HTTPS TLS 1.2/1.3,
  HTTPS reconnect/redirect, hostname/CA rejection and downgrade rejection).
- Real Ethernet/dmtcp HTTP: three complete 200 responses from example.com.
- Real Ethernet/dmtcp HTTPS: 3/3 complete verified downloads from httpbin.org;
  status, body length and FNV-1a hash match Python's independent HTTPS client.
- A 65536-byte HTTPS binary download also matched the independent reference;
  one recorded full-firmware run took 50.25 seconds including the handshake.
- After network TLS: UTC 6/6, RTC 12/12 and RNG 6/6. PC UTC was restored and the
  UTC library-service unit remained running.

The TCP diagnostic uses priority 1, checks buffered readiness before waiting on
coalesced semaphore notifications, and has a finite 60-second transaction limit.
Applications must make the same level-versus-edge distinction when integrating
an event-based byte queue. TLS arithmetic consumes CPU even though I/O is
nonblocking; schedule the calling worker appropriately for the target firmware.

A reproducible independent-reference network harness is included:

```sh
python3 tests/probe/run_tcp.py --boot-dir /path/to/dmod-boot \
  --url https://server.example/file --server-ip SERVER_IPV4 \
  --ca-board /ca.pem --ca-local build/board-fixtures/ca.pem --regressions
```

For public servers, omit --ca-local to use the PC system trust store and upload
an appropriate public root CA to the board. Test fixtures/private keys remain
local. The PC's isolated container was not reachable from the board's LAN, so
network validation used public endpoints instead of claiming a local-server run.
Renode is omitted as requested.

Long-chain latency remains a separate observed TLS limitation: some example.com
HTTPS attempts completed, while others ended in TLS truncation after a slow
handshake. Native execution and controlled OpenSSL peers passed. Track this in
[dmtls issue 3](https://github.com/choco-technologies/dmtls/issues/3); the three
passing HTTPS network repetitions above used httpbin.org. The upstream arithmetic
configuration update is [dmtls PR 2](https://github.com/choco-technologies/dmtls/pull/2).
No certificate or hostname verification is bypassed.
