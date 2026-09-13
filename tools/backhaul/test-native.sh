#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p .backhaul-tests
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    g++ -std=c++11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -Itests/backhaul/netconn -Ilib/CzcBackhaul/src tests/backhaul/test_netconn.cpp -o .backhaul-tests/test-netconn
    .backhaul-tests/test-netconn
    g++ -std=c++11 -Wall -Wextra -Werror -g -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -Ilib/Backhaul/src tests/backhaul/test_backhaul.cpp lib/Backhaul/src/Backhaul.cpp -o .backhaul-tests/test-transport
    .backhaul-tests/test-transport
  '
