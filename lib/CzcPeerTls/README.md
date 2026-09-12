# CZC TLS engine

The peer and debug-only administrative PSK channels use Mbed TLS 3.6.7.
The `mbedtls/` submodule's release and commit are recorded in `upstream.json`.

`CzcTlsConfig.h` selects TLS 1.3, external PSK with X25519, and
TLS_AES_128_GCM_SHA256. It uses two buffer profiles:

| Profile | Input | Output | Record Size Limit extension |
| --- | ---: | ---: | --- |
| Radio peers, client and server | 1024 | 1024 | 1024 bytes |
| Administrative server | 16384 | 1024 | Disabled |

Both TLS profiles share one crypto implementation. The administrative profile
accepts standard 16 KiB records from clients without a record size limit.
Only builds with `DEBUG` compile that profile and expose `czc_tls_new_admin`.
Production builds generate and compile the peer profile and shared crypto.
`CzcTlsEngine.inc` adapts the Mbed TLS API to the opaque
`CzcPeerTls.h` interface. `prepare-peer-tls.py` generates the C wrappers and
`CzcTlsSymbols.h` into `.pio/build/<environment>/czc-peer-tls/generated/`.
PlatformIO compiles them and the adapter into one private archive with LTO.
Symbol prefixes prevent collisions with the SDK's Mbed TLS. Headers and
compiler settings are scoped to this archive.
Generation runs automatically with `pio run` and preserves unchanged files for
incremental builds. `pio run -t clean` removes the generated inputs.

The adapter records TLS-owned PSA key imports and destroys leftover keys when
their session closes. This covers the Mbed TLS 3.6.7 error path that frees an
incomplete handshake transform without destroying its first imported key when
the second import fails. All engines and their PSA state belong to one task
and use the application's DRBG.

From the repository root:

```sh
# Initialize dependencies.
git submodule update --init --recursive

# Check the Mbed TLS dependency.
python3 tools/backhaul/prepare-peer-tls.py --check

bash tools/backhaul/test-tls.sh
bash tools/backhaul/test-admin-tls.sh
bash tools/backhaul/build.sh -e prod -e debug
```

Tests cover protocol version enforcement, eight TLS 1.3 peer pairs,
OpenSSL interoperability, malformed authentication, standard admin records,
allocation failures with another live session,
reconnects and complete cleanup under ASan/UBSan. Native memory measurements
are not measurements of the complete application on ESP32 hardware.
