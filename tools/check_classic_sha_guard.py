"""Compile-time contention tests; works with a native or Xtensa C++ compiler."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--cxx', help='C++ compiler executable (or set CXX)')
args = parser.parse_args()
compiler = args.cxx or os.environ.get('CXX') or shutil.which('g++') or shutil.which('clang++')
if not compiler:
    candidate = Path.home() / '.platformio/packages/toolchain-xtensa-esp32/bin/xtensa-esp32-elf-g++.exe'
    if candidate.is_file():
        compiler = str(candidate)
if not compiler:
    parser.error('Pass --cxx with a C++14-capable compiler')
with tempfile.TemporaryDirectory(prefix='nerdminer-sha-test-') as tmp:
    for test in ('classic_sha_reservation.cpp', 'mail_schedule.cpp'):
        subprocess.run([compiler, '-std=c++14', '-DCONFIG_IDF_TARGET_ESP32=1',
                        '-I' + str(root / 'tools/compile_tests/shim'), '-I' + str(root / 'src'),
                        '-c', str(root / 'tools/compile_tests' / test),
                        '-o', str(Path(tmp) / (test + '.o'))], check=True)
print('PASS: SHA contention/release and mail scheduling compile-time assertions')
