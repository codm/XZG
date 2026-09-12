#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
docker build -q -t czc-backhaul-tests:1 -f tests/backhaul/Dockerfile tests/backhaul
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    python tools/backhaul/prepare-peer-tls.py --check
    python tools/backhaul/build-native-peer-tls.py .backhaul-tests/peer-tls-objects
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -Itests/backhaul/shims -Ilib/CzcBackhaul/src -Ilib/CzcPeerTls/src -Ilib/Backhaul/src \
      tests/backhaul/test_fanout_tls.cpp lib/Backhaul/src/Backhaul.cpp .backhaul-tests/peer-tls-objects/*.o \
      -lmbedtls -lmbedx509 -lmbedcrypto -o .backhaul-tests/test-fanout-tls
    ASAN_OPTIONS=detect_leaks=1 .backhaul-tests/test-fanout-tls
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -Itests/backhaul/shims -Ilib/CzcBackhaul/src -Ilib/CzcPeerTls/src \
      tests/backhaul/test_tls13_policy.cpp .backhaul-tests/peer-tls-objects/*.o \
      -lmbedtls -lmbedx509 -lmbedcrypto -lssl -lcrypto -o .backhaul-tests/test-tls13-policy
    ASAN_OPTIONS=detect_leaks=1 .backhaul-tests/test-tls13-policy
  '
