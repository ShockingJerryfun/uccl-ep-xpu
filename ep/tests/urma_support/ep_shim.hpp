#pragma once
// Isolate the transport from CUDA kernels/verbs for SDK-backed host tests.
// The URMA and CUDA API boundary is simulated; the transport source is real.
#define RDMA_HPP
#define COMMON_HPP
#include <cstddef>
#include <cstdint>
constexpr int kMaxOutstandingSends = 16;
struct ProxyCtx {
  int local_rank = 0;
  int thread_idx = 0;
  static constexpr size_t kMaxAtomicOps = 16;
};
struct RDMAConnectionInfo {
  uintptr_t addr = 0;
  uint64_t len = 0;
  uintptr_t atomic_buffer_addr = 0;
  uint64_t atomic_buffer_len = 0;
  uint32_t urma_wire_version = 0;
  static constexpr uint32_t kMaxUrmaBlob = 2048;
  uint32_t urma_rjetty_len = 0, urma_main_seg_len = 0;
  uint32_t urma_atomic_seg_len = 0, urma_barrier_seg_len = 0;
  uint8_t urma_rjetty[2048]{}, urma_main_seg[2048]{};
  uint8_t urma_atomic_seg[2048]{}, urma_barrier_seg[2048]{};
};
