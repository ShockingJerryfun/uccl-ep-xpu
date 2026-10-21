#!/usr/bin/env python3
"""Compile the real transport against the chosen P2P SDK; simulate hardware only."""
import argparse
import os
from pathlib import Path
import subprocess
import tempfile

p = argparse.ArgumentParser()
p.add_argument('--sdk-include', required=True, type=Path,
               help='Directory containing the tested urma_api.h and urma_types.h')
p.add_argument('--syntax-only', action='store_true')
a = p.parse_args()
ep = Path(__file__).resolve().parents[1]
with tempfile.TemporaryDirectory(prefix='uccl-urma-test-') as tmp:
    tmp = Path(tmp)
    # Also support the existing installed-header spelling while testing baseline.
    nested = tmp / 'ub/umdk/urma'
    nested.parent.mkdir(parents=True)
    nested.symlink_to(a.sdk_include.resolve(), target_is_directory=True)
    cmd = [os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread',
           '-Wall', '-Wextra', '-Werror', '-DUSE_URMA',
           '-I' + str(ep / 'tests/urma_support'), '-I' + str(ep / 'include'),
           '-I' + str(a.sdk_include.resolve()), '-I' + str(tmp),
           '-include', str(ep / 'tests/urma_support/ep_shim.hpp')]
    cmd += [str(ep / 'src/urma_transport.cpp')]
    if a.syntax_only:
        cmd += ['-fsyntax-only']
    else:
        cmd += [str(ep / 'tests/urma_transport_test.cpp'), '-o', str(tmp / 'test')]
    subprocess.run(cmd, check=True)
    if not a.syntax_only:
        subprocess.run([str(tmp / 'test')], check=True)

    for defines in ([], ['-DUSE_URMA'],
                    ['-DUSE_URMA', '-DUSE_SUBSET_BARRIER'],
                    ['-DUSE_URMA', '-D__HIPCC__']):
        binary = tmp / 'backend_test'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                        '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ep / 'include'), *defines,
                        str(ep / 'tests/backend_config_test.cpp'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
