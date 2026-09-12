#!/usr/bin/env python3
"""Build the peer and admin TLS profiles for host regression tests."""
import argparse
import importlib.util
import re
from pathlib import Path
import subprocess

root = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('output', type=Path)
args = parser.parse_args()
args.output.mkdir(parents=True, exist_ok=True)
for old in args.output.glob('*.o'):
    old.unlink()
lib = root / 'lib/CzcPeerTls'
spec = importlib.util.spec_from_file_location('czc_tls_generator', root/'tools/backhaul/prepare-peer-tls.py')
generator = importlib.util.module_from_spec(spec)
spec.loader.exec_module(generator)
generated = generator.prepare(args.output/'generated', debug=True)
sources = [(path, 'adapter_' + path.name) for path in sorted((lib/'src').glob('*.c'))]
sources += [(path, path.relative_to(generated).as_posix().replace('/', '_'))
            for path in sorted(generated.rglob('*.c'))]
for source, name in sources:
    name += '.o'
    subprocess.run(['gcc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
        '-Wno-unused-parameter',  # Mbed TLS leaves a parameter unused when tickets are disabled.
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
        '-DDEBUG', '-DMBEDTLS_CONFIG_FILE="CzcTlsConfig.h"', '-I'+str(lib/'src'),
        '-I'+str(generated), '-I'+str(lib/'mbedtls'),
        '-I'+str(lib/'mbedtls/include'), '-I'+str(lib/'mbedtls/library'),
        '-c', str(source), '-o', str(args.output/name)], check=True)
symbols = subprocess.check_output(['nm', '-g'] + [str(p) for p in args.output.glob('*.o')], text=True)
assert not re.search(r'\b[UTDBRVW]\s+(?:mbedtls|psa)_\w+', symbols), 'Unisolated Mbed TLS symbol'
print('PASS peer/admin TLS build and symbol isolation')
