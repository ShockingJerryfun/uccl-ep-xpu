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
    fake_metax = tmp / 'fake_mcruntime.cpp'
    fake_metax.write_text(r'''
#include <cstdlib>
#include <cstring>
#include <set>
static std::set<void*> allocations;
extern "C" int mcSetDevice(int device) { return device >= 0 ? 0 : 1; }
extern "C" int mcGetDeviceCount(int* count) { *count = 8; return 0; }
extern "C" int mcMalloc(void** ptr, size_t bytes) {
    if (bytes == 0 || bytes % 4096 != 0) return 1;
    *ptr = std::malloc(bytes);
    if (*ptr) allocations.insert(*ptr);
    return *ptr ? 0 : 1;
}
extern "C" int mcFree(void* ptr) {
    if (allocations.erase(ptr)) std::free(ptr);
    return 0;
}
extern "C" int mcMemcpy(void* dst, void const* src, size_t bytes, int) {
    std::memcpy(dst, src, bytes);
    return 0;
}
''')
    fake_metax_lib = tmp / ('libmcruntime.dylib' if os.uname().sysname == 'Darwin'
                            else 'libmcruntime.so')
    shared_flag = '-dynamiclib' if os.uname().sysname == 'Darwin' else '-shared'
    subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', shared_flag,
                    '-fPIC', str(fake_metax), '-o', str(fake_metax_lib)],
                   check=True)
    # Also support the existing installed-header spelling while testing baseline.
    nested = tmp / 'ub/umdk/urma'
    nested.parent.mkdir(parents=True)
    nested.symlink_to(a.sdk_include.resolve(), target_is_directory=True)
    cmd = [os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread',
           '-Wall', '-Wextra', '-Werror', '-DUSE_URMA',
           '-I' + str(ep / 'tests/urma_support'), '-I' + str(ep / 'include'),
           '-I' + str(a.sdk_include.resolve()), '-I' + str(tmp),
           '-include', str(ep / 'tests/urma_support/ep_shim.hpp')]
    cmd += [str(ep / 'src/gpu_p2p_runtime.cpp'),
            str(ep / 'src/urma_transport.cpp')]
    if a.syntax_only:
        cmd += ['-fsyntax-only']
    else:
        cmd += [str(ep / 'tests/urma_transport_test.cpp'), '-o', str(tmp / 'test')]
    subprocess.run(cmd, check=True)
    if not a.syntax_only:
        subprocess.run([str(tmp / 'test')], check=True)

    metax_cmd = [os.environ.get('CXX', 'c++'), '-std=c++17', '-pthread',
                 '-Wall', '-Wextra', '-Werror', '-DUSE_URMA',
                 '-DUSE_METAX',
                 '-I' + str(ep / 'tests/urma_support'),
                 '-I' + str(ep / 'include'), '-I' + str(a.sdk_include.resolve()),
                 '-I' + str(tmp), '-include',
                 str(ep / 'tests/urma_support/ep_shim.hpp'),
                 str(ep / 'src/gpu_p2p_runtime.cpp'),
                 str(ep / 'src/urma_transport.cpp')]
    if a.syntax_only:
        subprocess.run([*metax_cmd, '-fsyntax-only'], check=True)
    else:
        metax_test = tmp / 'metax_transport_test'
        subprocess.run([*metax_cmd, str(ep / 'tests/urma_transport_test.cpp'),
                        '-o', str(metax_test)], check=True)
        metax_env = dict(os.environ,
                         UCCL_METAX_RUNTIME_LIB=str(fake_metax_lib))
        subprocess.run([str(metax_test)], check=True, env=metax_env)

        runtime_test = tmp / 'metax_runtime_test'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                        '-Wall', '-Wextra', '-Werror', '-DUSE_METAX',
                        '-I' + str(ep / 'include'),
                        str(ep / 'src/gpu_p2p_runtime.cpp'),
                        str(ep / 'tests/gpu_p2p_runtime_test.cpp'),
                        '-o', str(runtime_test)], check=True)
        subprocess.run([str(runtime_test)], check=True, env=metax_env)

    for defines in ([], ['-DUSE_URMA'],
                    ['-DUSE_URMA', '-DUSE_METAX'],
                    ['-DUSE_URMA', '-DUSE_URMA_METAX'],
                    ['-DUSE_URMA', '-DUSE_SUBSET_BARRIER'],
                    ['-DUSE_URMA', '-D__HIPCC__']):
        binary = tmp / 'backend_test'
        subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17',
                        '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ep / 'include'), *defines,
                        str(ep / 'tests/backend_config_test.cpp'),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
