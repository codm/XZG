#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    python tools/backhaul/prepare-peer-tls.py --check
    python tools/backhaul/build-native-peer-tls.py .backhaul-tests/admin-tls-objects
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -Itests/backhaul/shims -Ilib/CzcBackhaul/src -Ilib/CzcPeerTls/src \
      tests/backhaul/test_admin_tls.cpp .backhaul-tests/admin-tls-objects/*.o \
      -Wl,--wrap=calloc -lssl -lcrypto -o .backhaul-tests/test-admin-tls
    ASAN_OPTIONS=detect_leaks=1 .backhaul-tests/test-admin-tls
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -DCZC_ADMIN_TLS -Itests/backhaul/shims -Ilib/CzcBackhaul/src -Ilib/CzcPeerTls/src \
      tests/backhaul/tls_peer.cpp .backhaul-tests/admin-tls-objects/*.o \
      -o .backhaul-tests/tls-admin
    CZC_TLS_PEER=.backhaul-tests/tls-admin ASAN_OPTIONS=detect_leaks=1 python tests/backhaul/test_host.py
  '
