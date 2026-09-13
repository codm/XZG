#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -DARDUINO_ARCH_ESP32 -Itests/backhaul/reconnect -Itests/backhaul/netconn -Itests/backhaul/shims \
      -Ilib/CzcBackhaul/src -Ilib/CzcPeerTls/src tests/backhaul/test_reconnect.cpp -o .backhaul-tests/test-reconnect
    .backhaul-tests/test-reconnect
    python -c "from pathlib import Path; s=Path(\"src/etc.cpp\").read_text(); Path(\".backhaul-tests/background-update-under-test.inc\").write_text(s[s.index(\"struct UpdateCheckResult\"):s.index(\"int numOfConnectedClients()\")])"
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie -pthread \
      -Itests/backhaul/rtos -Ilib/CzcBackhaul/src tests/backhaul/test_background_update.cpp -o .backhaul-tests/test-background-update
    .backhaul-tests/test-background-update
  '
