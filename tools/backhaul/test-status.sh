#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Ilib/CzcBackhaul/src -I.pio/libdeps/prod/ArduinoJson/src \
      tests/backhaul/test_status_snapshot.cpp -o .backhaul-tests/test-status &&
    .backhaul-tests/test-status
  '
