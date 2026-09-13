#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 - <<'PY'
from pathlib import Path
s=Path('src/main.cpp').read_text()
start=s.index('void connectWifi()\n{')
connect=s[start:s.index('void setupCoordinatorMode()',start)]
assert 'WiFi.IPv6(' not in connect
loop=s[s.index('void loop(void)'):]
assert loop.index('networkIpv6Loop();') < loop.index('if (!vars.zbFlashing)')
status=Path('src/network_status.cpp').read_text()
assert 'networkCfg.wifiEnable && WiFi.status()==WL_CONNECTED' in status
assert 'start(ipv6Eth' not in status and 'start(ipv6Ap' not in status
sdk=Path(Path('.backhaul-tests/sdk-path.txt').read_text())/'libraries/WiFi/src'
client=(sdk/'WiFiClient.cpp').read_text()
branch=client[client.index('int WiFiClient::connect(const char *host, uint16_t port, int32_t timeout_ms)'):]
branch=branch[:branch.index('int WiFiClient::setSocketOption')]
assert 'WIFI_WANT_IP6_BIT' in branch and 'hostByName6' in branch and 'hostByName(host' in branch
print('PASS IPv6 wiring: no global SDK DNS-policy flag, connected WiFi only, Ethernet/AP retain event initialization; Off still supported')
generic=(sdk/'WiFiGeneric.cpp').read_text()
# Exact non-AP branch; AP-server code is outside the DHCP-client regression.
a=generic.index('esp_err_t set_esp_interface_ip(')
b=generic.index('    } else {',a)
station=generic[a:b]+'    }\n    return err;\n}\n'
a=generic.index('esp_err_t set_esp_interface_dns(')
b=generic.index('\n#if ARDUHAL_LOG_LEVEL',a)
station+=generic[a:b]
sta=(sdk/'WiFiSTA.cpp').read_text()
a=sta.index('bool WiFiSTAClass::config(');b=sta.index('\n/**',a)
station+=sta[a:b]
Path('.backhaul-tests/sdk-station-ip-under-test.inc').write_text(station)
a=connect.index('    bool addressConfigured;');b=connect.index('    WiFi.setScanMethod',a)
app='static void configureFromApp() {\n'+connect[a:b]+'++beginCalls;\n}\n'
etc=Path('src/etc.cpp').read_text();a=etc.index('static bool restoreNetworkDns(');b=etc.index('\n/*void reCheckDNS()',a)
app+=etc[a:b]
Path('.backhaul-tests/app-ipv4-under-test.inc').write_text(app)

PY
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Itests/backhaul/network -I.pio/libdeps/prod/ArduinoJson/src \
      tests/backhaul/test_network_status.cpp -o .backhaul-tests/test-network-status &&
    .backhaul-tests/test-network-status
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -Itests/backhaul/network tests/backhaul/test_network_ipv6.cpp -o .backhaul-tests/test-network-ipv6
    .backhaul-tests/test-network-ipv6
    g++ -std=c++11 -Wall -Wextra -Werror -Wno-unused-parameter -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      tests/backhaul/test_ipv4_dhcp.cpp -o .backhaul-tests/test-ipv4-dhcp
    .backhaul-tests/test-ipv4-dhcp
  '
