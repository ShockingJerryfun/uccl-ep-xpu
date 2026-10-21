#include "gpu_p2p_runtime.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {
template <class F>
void rejects(F&& f) {
  bool threw = false;
  try {
    f();
  } catch (std::exception const&) {
    threw = true;
  }
  assert(threw);
}
}  // namespace

int main() {
  using namespace uccl::gpu_p2p;

#ifdef USE_METAX
  assert(vendor() == Vendor::MetaX);
  assert(registration_alignment() == 4096);
  assert(atomic_buffer_is_gpu_memory());

  set_device(1);
  void* ptr = allocate_device(8192, 1);
  assert(ptr != nullptr);
  prepare_main_buffer(ptr, 8192, 1);
  prepare_atomic_buffer(ptr, 8192, 1);
  rejects([&] { prepare_main_buffer(ptr, 8192, 0); });

  zero_memory(ptr, 8192, MemoryKind::Device, 1);
  std::vector<std::uint8_t> host(8192, 0xff);
  copy_device_to_host(host.data(), ptr, host.size(), 1);
  for (auto byte : host) assert(byte == 0);
  std::fill(host.begin(), host.end(), 0x5a);
  copy_host_to_device(ptr, host.data(), host.size(), 1);
  std::fill(host.begin(), host.end(), 0);
  copy_device_to_host(host.data(), ptr, host.size(), 1);
  for (auto byte : host) assert(byte == 0x5a);
  release_memory(ptr, MemoryKind::Device, 1);

  // Production uses 81960 bytes; MetaX requires the backing mcMalloc size to
  // be rounded to its 4 KiB registration granularity.
  void* atomic = allocate_atomic_buffer(81960, 1);
  prepare_atomic_buffer(atomic, 81960, 1);
  release_memory(atomic, MemoryKind::Device, 1);
#else
  assert(vendor() == Vendor::Nvidia);
  assert(registration_alignment() == 64 * 1024);
  assert(!atomic_buffer_is_gpu_memory());
#endif
}
