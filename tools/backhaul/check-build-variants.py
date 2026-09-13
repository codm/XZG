#!/usr/bin/env python3
"""Check the actual linked ESP32 images, not only preprocessor declarations."""
from pathlib import Path
import subprocess

root=Path(__file__).resolve().parents[2]
nm=root/'.cache/backhaul/pio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-nm'
for variant in ('prod','debug'):
    folder=root/'.pio/build'/variant
    symbols=subprocess.check_output([str(nm),'-C',str(folder/'firmware.elf')],text=True)
    image=(folder/'firmware.bin').read_bytes()
    assert image[0]==0xe9 and len(image)<=1310720, (variant,'invalid or oversized OTA image')
    if variant!='debug':
        for forbidden in ('czc_admin3_', 'czc_tls_admin_', 'czc_tls_new_admin', 'adminWork', 'adminListen'):
            assert forbidden not in symbols, (variant,forbidden)
        assert b'czc-admin-v1' not in image
    else:
        assert 'czc_admin3_' in symbols and 'adminWork' in symbols and b'czc-admin-v1' in image
    assert b'czc-peer-v2' in image, (variant,'peer TLS missing')
    print(f'PASS {variant} linked image: debug service {"included" if variant=="debug" else "absent"}, peer TLS retained; {len(image)} bytes')
