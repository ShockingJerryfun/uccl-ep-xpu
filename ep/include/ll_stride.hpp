#pragma once

#include <cstddef>

#if defined(__CUDACC__) || defined(__HIPCC__) || defined(__MCC__) || \
    defined(__MXCC__) || defined(__MC_PLATFORM_MXCC__)
#define UCCL_LL_HOST_DEVICE __host__ __device__
#else
#define UCCL_LL_HOST_DEVICE
#endif

namespace uccl {

// Only the local send slots are padded; the message and receive layout stay
// unchanged. Batched sends require tightly packed messages.
UCCL_LL_HOST_DEVICE constexpr std::size_t ll_dispatch_send_stride(
    std::size_t message_bytes) {
#if defined(USE_METAX) && !defined(PER_EXPERT_BATCHING)
  return (message_bytes + 255) / 256 * 256;
#else
  return message_bytes;
#endif
}

}  // namespace uccl

#undef UCCL_LL_HOST_DEVICE
