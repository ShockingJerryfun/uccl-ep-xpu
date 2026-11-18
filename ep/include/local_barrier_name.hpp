#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>

// Local ranks in one torchrun job must share a barrier; independent rendezvous
// endpoints must not accidentally attach to one another's barrier.
inline std::string local_barrier_shm_name(std::string const& ip, bool normal,
                                          int thread_idx, std::uint32_t uid,
                                          std::string const& master_addr,
                                          std::string const& master_port) {
  std::string result = "/uccl_barrier_" + ip + "_uid" + std::to_string(uid) +
                       (normal ? "_ht" : "_ll") + "_th" + std::to_string(thread_idx);
  if (master_port.empty()) return result;  // Preserve non-torchrun callers.
  unsigned port = 0;
  if (master_port.size() > 5) throw std::invalid_argument("Invalid MASTER_PORT");
  for (char c : master_port) {
    if (c < '0' || c > '9') throw std::invalid_argument("Invalid MASTER_PORT");
    port = port * 10 + static_cast<unsigned>(c - '0');
  }
  if (port == 0 || port > 65535) throw std::invalid_argument("Invalid MASTER_PORT");
  // FNV-1a is deterministic across processes, unlike an implementation-defined
  // std::hash. The human-readable port is retained for diagnosis.
  std::uint64_t hash = UINT64_C(14695981039346656037);
  for (unsigned char c : master_addr) { hash ^= c; hash *= UINT64_C(1099511628211); }
  return result + "_job" + std::to_string(hash) + "_p" + std::to_string(port);
}
