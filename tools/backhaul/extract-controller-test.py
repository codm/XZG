#!/usr/bin/env python3
"""Test production handlers, including the boot wiring that enables them."""
from pathlib import Path

root = Path(__file__).resolve().parents[2]
source = (root / 'src/backhaul.cpp').read_text()
out = root / '.backhaul-tests'
(out / 'raw-under-test.inc').write_text(source[source.index('static void rawClose() {'):source.index('static void statusLogLine(')])
(out / 'lifecycle-under-test.inc').write_text(source[source.index('static void serviceRadioLifecycle()'):source.index('static void initializeMaster()')])
(out / 'master-startup-under-test.inc').write_text(source[source.index('static void initializeMaster()'):source.index('static void peerWork()')])
network = source[source.index('static void networkTask(void *)'):source.index('bool backhaulConfigured()')]
assert network.index('serviceRadioLifecycle();') < network.index('peerWork();') < network.index('adminWork();') < network.index('rawWork();')
peer = source[source.index('static void peerWork()'):source.index('static void ieeeText(')]
assert peer.index('if(radio.resetWaiting)') < peer.index('info()')
# Execute the actual Satellite boot branch with the real UART task, before
# peer TLS/network work. This is distinct from commissioning driven by Z2M.
(out / 'satellite-startup-under-test.inc').write_text(
    source[source.index('static bool info()'):source.index('static bool bindRadio(')]
    + peer[:peer.index('    if(!lastInfo || millis()-lastInfo>2000)')] + '}\n')
(out / 'migration-under-test.inc').write_text(source[source.index('class MigrationNv '):source.index('static void readBootstrapFailure(')])
assert peer.index('bootstrapState==3') < peer.index('if(cfg.mode==2 && !satelliteInitialized)')
assert peer.index('if(bootstrapState==3) return;') < peer.index('radio.rpc(0x2f,5')
(out / 'begin-under-test.inc').write_text(
    source[source.index('static bool backhaulRoleAllowed()'):source.index('void backhaulLoad()')]
    + source[source.index('void backhaulBegin() {'):source.index('void backhaulLoop()')])
(out / 'role-change-under-test.inc').write_text(source[source.index('void backhaulRoleChanged()'):source.index('bool backhaulStatus(')])

# Startup must occur exactly once, in setup, after all stock UART probes. A
# call in a traffic logger deadlocks both the controller and peer on first boot.
main = (root / 'src/main.cpp').read_text()
setup = main[main.index('void setup()'):main.index('\nWiFiClient client[')]
assert main.count('backhaulBegin();') == setup.count('backhaulBegin();') == 1
assert setup.index('backhaulBegin();') > max(setup.index('zbHwCheck();'), setup.index('zbFwCheck()'))
assert setup.index('backhaulBegin();') > setup.index('setupCoordinatorMode();')
assert main.index('backhaulLoad();') < main.index('Serial2.begin(')
sdk_root = root / (root / '.backhaul-tests/sdk-path.txt').read_text()
sdk = sdk_root / 'cores/esp32'
assert 'virtual int availableForWrite() { return 0; }' in (sdk / 'Print.h').read_text()
assert 'availableForWrite' not in (sdk_root / 'libraries/WiFi/src/WiFiClient.h').read_text()

(out / "commission-under-test.inc").write_text(source[source.index("static void commissionSatellite()"):source.index("static void networkTask(void *)")])

# Actual peer result handler and the service block whose order is critical for APS ACK.
(out / 'peer-results-under-test.inc').write_text(source[source.index('static void complete(Peer '):source.index('static void disconnectPeer(')])
work = source[source.index('static void workPeer(Peer '):source.index('static void serviceRadioLifecycle()')]
(out / 'peer-order-under-test.inc').write_text(work[work.index('    // A Confirm and'):work.index('    if(p.profileNeeded')])
