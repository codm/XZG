#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
python3 - <<'PY'
from pathlib import Path
s=Path('src/main.cpp').read_text(); out=Path('.backhaul-tests')
log=s[s.index('void printRecvSocket('):s.index('void loop(void)')]
(out/'stock-uart-under-test.inc').write_text(log)
(out/'old-uart-log.inc').write_text(log.replace('output_sprintf[3]','output_sprintf[2]'))
(out/'stock-net-read.inc').write_text(s[s.index('          while (net_bytes_read'):s.index(' // send to Zigbee',s.index('          while (net_bytes_read'))])
(out/'stock-radio-read.inc').write_text(s[s.index('        while (serial_bytes_read'):s.index('        // send to LAN',s.index('        while (serial_bytes_read'))])
PY
docker run --rm --network none --cap-drop ALL --security-opt no-new-privileges \
  --user "$(id -u):$(id -g)" -v "$PWD:/workspace" -w /workspace czc-backhaul-tests:1 sh -c '
    set -eu
    g++ -std=c++11 -Wall -Wextra -Werror -Wno-sign-compare -fsanitize=address,undefined -fno-omit-frame-pointer -no-pie \
      tests/backhaul/test_stock_uart.cpp -o .backhaul-tests/test-stock-uart
    .backhaul-tests/test-stock-uart
    python -c "import subprocess; r=subprocess.run([\".backhaul-tests/test-stock-uart\",\"old\"],capture_output=True,text=True); assert r.returncode and \"stack-buffer-overflow\" in r.stderr,r.stderr; print(\"PASS old UART log overflow reproduced by ASan on one received byte\")"
  '
