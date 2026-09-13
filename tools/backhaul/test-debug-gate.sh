#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
mkdir -p .backhaul-tests
python3 tools/backhaul/extract-controller-test.py
python3 - <<'PY'
from pathlib import Path
s=Path('src/backhaul.cpp').read_text()
Path('.backhaul-tests/debug-config-under-test.inc').write_text(
    s[s.index('void backhaulLoad()'):s.index('void backhaulBegin()')]+
    s[s.index('static void webConfig()'):s.index('static void webKey()')])
Path('.backhaul-tests/dial-under-test.inc').write_text(s[s.index('static netconn *dial('):s.index('static void endpointFailed(')])
PY
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -e TMPDIR=/workspace/.backhaul-tests \
  -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    for variant in prod debug; do
      flags=""; test "$variant" != debug || flags=-DDEBUG
      g++ -std=c++11 $flags -Wall -Wextra -Werror -Wno-class-memaccess -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
        -Ilib/CzcBackhaul/src tests/backhaul/test_startup.cpp -o .backhaul-tests/startup-$variant
      .backhaul-tests/startup-$variant
      g++ -std=c++11 $flags -Wall -Wextra -Werror -Wno-class-memaccess -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
        -Ilib/CzcBackhaul/src -I.pio/libdeps/prod/ArduinoJson/src \
        tests/backhaul/test_debug_config.cpp -o .backhaul-tests/config-$variant
      .backhaul-tests/config-$variant
    done
    g++ -std=c++11 -Wall -Wextra -Werror -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      -Ilib/CzcBackhaul/src tests/backhaul/test_dial.cpp -o .backhaul-tests/test-dial
    .backhaul-tests/test-dial
  '
