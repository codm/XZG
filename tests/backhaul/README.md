# Host tests

From the repository root, with Docker, Python 3.9+ and Node.js 18+ installed:

```sh
bash tools/backhaul/build.sh -e prod -e debug
bash tools/backhaul/test.sh
```

Build output is in `bin/` and `.pio/build/{prod,debug}/`. Test binaries and logs go
to `.backhaul-tests/`; downloaded dependencies go to `.cache/backhaul/`.
The runner prints one result per suite and
the failing log on error. Individual `test-*.sh` scripts can also be run after
the build and test image have been prepared.

The native tests use ASan/UBSan and cover framing, socket backpressure, eight
peer TLS sessions, admin TLS interoperability, authentication failures,
allocation failure cleanup, UART arbitration, bootstrap recovery, network
address handling and both firmware updaters. JavaScript tests execute the
Role, Network and update UI. Python tests exercise the hardware acceptance
runner with simulated devices. None of these tests contacts physical devices.

Generate a single file for a machine with access to the hardware:

```sh
python3 tools/backhaul/make_acceptance_script.py
```

Copy `.backhaul-tests/czc-two-device-acceptance.py` to that machine. It requires
Python 3.9+ and Docker; a Python 3.13 container provides the TLS PSK client.
Pass your addresses and key explicitly. After exporting `CZC_PSK` with the
configured 64-character hexadecimal key, for example:

```sh
python3 czc-two-device-acceptance.py --mode local \
  --master 192.0.2.1 --satellite 192.0.2.2 --psk "$CZC_PSK" --compose-dir .
```

Use the existing Compose project with services named `mosquitto` and
`zigbee2mqtt`. The runner starts Mosquitto, temporarily stops Zigbee2MQTT,
tests exchanges and recovery, reboots the devices and starts Zigbee2MQTT again.
Install the `debug` build on both devices for access to the administrative service.
`local` needs no physical
confirmations and cannot certify operation through a remote Satellite.

`--mode full --sensor IEEE` adds manual checks using a SONOFF SNZB-02WD sensor:
fresh reports and changing temperature units on its display, no display change
with Satellite powered off, and recovery without rejoining. Place the sensor
outside Master's radio range. Direct radio communication between the gateways
is not excluded by this test. A successful two-device run does not certify
eight physical Satellites. Install `prod` after acceptance for normal operation.

`--mode diagnose` only reads HTTP status and local route information; it needs
no key or Docker and changes no services or settings. `--admin-family` selects
the administrative connection's address family; `--peer-family` requires IPv4
or IPv6 on the actual inter-device channel. IEEE addresses are discovered and
pinned for the run, or supplied with `--master-ieee` and `--satellite-ieee`.
