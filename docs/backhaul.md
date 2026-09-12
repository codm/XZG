# Zigbee packet forwarding over IP

Master and Satellite modes let CZC gateways in separate radio coverage areas
carry Zigbee frames over an encrypted IP connection. Zigbee software connects to
Master using its ZNP TCP interface. Satellites provide remote radio
coverage in the same Zigbee network. One Master supports up to eight Satellite
connections.

## Configuration

1. Install radio firmware with the optional extensions from
   [Z-Stack-firmware PR #609](https://github.com/Koenkk/Z-Stack-firmware/pull/609)
   on each device.
2. In **Role**, select **Master** on the gateway used by your Zigbee software.
   Generate a shared key.
3. Select **Satellite** on the other gateway, enter Master's IP address or
   hostname and the same key. IEEE identities are exchanged automatically.
4. Start or restore the Zigbee network on Master through your Zigbee software.
   Satellite receives the initial network profile through the encrypted channel and
   initializes its radio without an initial join within Master's radio range.

The modes are disabled by default. Select **Off** to use the ordinary CZC role.
The controls are available for the Zigbee Coordinator firmware role only.
Selecting Router or OpenThread hides the controls; applying that firmware role
disables forwarding and restarts ESP32 with the ordinary UART path. The PSK and
Master address remain saved. Returning to Coordinator leaves forwarding off.
A Satellite previously using another network is migrated to the configured
Master's network.

Role controls the peer port and shows each peer's IEEE, IP and connection state.
Network lists current addresses of both interfaces. An IPv6 Master address
uses IPv6; IPv4 uses IPv4; hostnames use their resolved addresses.
Peer TLS defaults to port `7443`. Zigbee applications use the configured ZNP
port (`6638` by default).

The radio update dialog accepts a URL or local BIN file. ESP32 updates report
completion across the device's restart.

## Transport and authentication

Peer and administrative channels use TLS 1.3, a 256-bit external PSK, X25519
key exchange and `TLS_AES_128_GCM_SHA256`. The services use distinct PSK identities.
Nodes sharing a key belong to the same trust group.

The key is returned to the browser by the configuration API so it can be shown
and copied, using the web authentication policy. The HTTP UI
and ZNP TCP connection do not gain TLS from this feature.

See the [TLS engine documentation](../lib/CzcPeerTls/README.md) for dependencies,
build configuration and memory profiles.

`lib/Backhaul` provides framing, epochs, a bounded transaction window, heartbeats
and deadlines. `src/backhaul.cpp` maps admission results to radio tickets,
renews virtual-neighbor leases and handles reconnection. Admission means the
destination radio queued the frame; it does not replace APS acknowledgements.
`lib/CzcBackhaul` owns UART arbitration, bootstrap, sockets and recovery state.

## Build and test

Initialize dependencies with `git submodule update --init --recursive` after cloning.
Master/Satellite support is part of the main firmware: build with `pio run`
or `bash tools/backhaul/build.sh -e prod` for the container build. Forwarding
is enabled by selecting the mode in Role.

The `DEBUG` build flag selects the acceptance service. `pio run -e debug`
includes administrative TLS on fixed port `7444`, diagnostic commands and
extended logs. These diagnostics are available only in `debug` builds.
The API's `debug_mode` reports this build capability. Updating between variants
preserves the PSK, mode, Master address and peer port. Both builds use LTO to
keep the firmware and localized UI within the original OTA slot.

The build generates TLS wrappers and the symbol header inside `.pio/build/`.
The `prepare-web-server.py` step applies a bounded idle-client queue to
Arduino WebServer, preventing idle browser connections from delaying ready
API requests.

See [host and hardware tests](../tests/backhaul/README.md). Images remain close
to the original OTA slot size, checked by PlatformIO on each build. Eight TLS
peer pairs are covered by native tests; hardware acceptance has covered one
Master and one Satellite, not eight physical nodes.
