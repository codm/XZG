#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    python tools/backhaul/extract-controller-test.py
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Ilib/CzcBackhaul/src tests/backhaul/test_migration.cpp -o .backhaul-tests/test-migration
    .backhaul-tests/test-migration
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
      -Itests/backhaul/rtos -Ilib/CzcBackhaul/src tests/backhaul/test_controller.cpp -o .backhaul-tests/test-controller
    .backhaul-tests/test-controller
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
      -Ilib/CzcBackhaul/src tests/backhaul/test_startup.cpp -o .backhaul-tests/test-startup
    .backhaul-tests/test-startup
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer \
      tests/backhaul/test_commission.cpp -o .backhaul-tests/test-commission
    .backhaul-tests/test-commission
    g++ -std=c++11 -DDEBUG -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -pthread \
      -Itests/backhaul/rtos -Ilib/CzcBackhaul/src -I.pio/libdeps/prod/ArduinoJson/src \
      tests/backhaul/test_satellite_startup.cpp -o .backhaul-tests/test-satellite-startup
    .backhaul-tests/test-satellite-startup
  '
