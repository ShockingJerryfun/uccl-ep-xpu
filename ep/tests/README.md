## GPU Driven Test Scripts.

Below are the test scripts for investigating the feasibility of using multiple CPU proxy process for GPUDirect communication.

## Standalone low-latency kernel test

`low_latency_local_test` calls the production low-latency dispatch and combine
launchers directly on one GPU with one rank. It does not import PyTorch or use
MCCL, a CPU proxy, or URMA.

Build and run with the MACA environment activated:

```bash
CUCC_TARGETS=xcore1000 make -C ep/tests -f Makefile.maca MACA_PATH=/opt/maca-3.7.0 low_latency_local_test
timeout -k 5s 180s ep/tests/low_latency_local_test
```

On an NVIDIA A100:

```bash
make -C ep/tests low_latency_local_test CUDAFLAGS="-arch=sm_80 -DDISABLE_SM90_FEATURES"
timeout -k 5s 180s ep/tests/low_latency_local_test
```

The test checks BF16 and FP8 dispatch payloads, expert counts, source indices,
layout ranges, and BF16 weighted combine against CPU results. It covers empty
inputs, 32/64-thread boundaries, hidden sizes 2048 and 8192, top-k 1/4/8,
masked routes, empty experts, combined and separate send/receive launches, and
buffer reuse. Trailing guard bytes check for buffer overruns. Success
prints `PASS local low-latency dispatch/combine: 240 cases, 2 iterations each`.

This exercises the same-rank local-memory branch of `internode_ll.cu`. It does
not cover cross-rank IPC, the D2H command path, remote completion ordering,
`internode.cu`, UE8M0 scales, log-format combine, or zero-copy combine. It is a
correctness test, not a network performance benchmark.

`intranode_combine_test` also calls production dispatch/combine kernels without
MCCL or URMA, but requires 2, 4, or 8 peer-connected GPUs on the same host.
