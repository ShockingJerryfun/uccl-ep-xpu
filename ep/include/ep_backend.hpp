#pragma once
#include <cstdlib>
#include <cstring>
#include <stdexcept>

// Runtime selection is independent of build availability. Keep legacy fallback
// for unknown values, but never silently run verbs when URMA was requested.
inline bool ep_uses_urma() {
  char const* value = std::getenv("UCCL_EP_TRANSPORT");
  if (!value || std::strcmp(value, "urma") != 0) return false;
#if !defined(USE_URMA)
  throw std::runtime_error("UCCL_EP_TRANSPORT=urma requires USE_URMA=1");
#elif defined(__HIP_PLATFORM_AMD__) || defined(__HIPCC__)
  throw std::runtime_error("URMA P2P backend currently requires NVIDIA CUDA");
#elif defined(USE_SUBSET_BARRIER)
  throw std::runtime_error(
      "URMA requires the default barrier (disable USE_SUBSET_BARRIER)");
#else
  return true;
#endif
}
