#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 - <<'PY'
from pathlib import Path
s=Path('src/web.cpp').read_text()
out=Path('.backhaul-tests')
(out/'web-events-under-test.inc').write_text(s[s.index('// SSE transport:'):s.index('void sendGzip(')])
(out/'web-task-under-test.inc').write_text(s[s.index('void updateWebTask('):s.index('void handleLoginGet(')].replace('void *parameter','void *'))
sdk=Path(Path('.backhaul-tests/sdk-path.txt').read_text())
s=(sdk/'libraries/WiFi/src/WiFiClient.cpp').read_text()
(out/'legacy-web-write.inc').write_text(s[s.index('size_t WiFiClient::write(const uint8_t *'):s.index('size_t WiFiClient::write_P(')].replace('WiFiClient::write','LegacyClient::write'))
s=(sdk/'libraries/WebServer/src/WebServer.cpp').read_text()
current=s[s.index('void WebServer::handleClient()'):s.index('void WebServer::close()')]
start=current.index('    // CZC: idle HTTP sockets')
end=current.index('    if (!_currentClient)',start)
old=current[:start]+'    _currentClient = _server.available();\n'+current[end:]
(out/'web-queue-under-test.inc').write_text(current+old.replace('void WebServer::handleClient()','void WebServer::legacyHandleClient()'))
PY
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -pthread \
      -Isrc tests/backhaul/test_web_events.cpp -o .backhaul-tests/test-web-events
    .backhaul-tests/test-web-events
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      tests/backhaul/test_web_queue.cpp -o .backhaul-tests/test-web-queue
    .backhaul-tests/test-web-queue
  '
