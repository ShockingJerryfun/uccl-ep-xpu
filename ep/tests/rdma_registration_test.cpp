#include "rdma_registration.hpp"

#include <cassert>
#include <cstdint>
#include <stdexcept>

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
  using uccl::rdma::GpuRegistrationMode;
  using uccl::rdma::make_dmabuf_export_range;
  using uccl::rdma::parse_gpu_registration_mode;

  assert(parse_gpu_registration_mode(nullptr) == GpuRegistrationMode::Auto);
  assert(parse_gpu_registration_mode("") == GpuRegistrationMode::Auto);
  assert(parse_gpu_registration_mode("auto") == GpuRegistrationMode::Auto);
  assert(parse_gpu_registration_mode("peermem") ==
         GpuRegistrationMode::PeerMemory);
  assert(parse_gpu_registration_mode("dmabuf") ==
         GpuRegistrationMode::DmaBuf);
  rejects([] { (void)parse_gpu_registration_mode("unknown"); });

  // Exact allocation registration keeps the allocation boundaries.
  auto exact = make_dmabuf_export_range(0x100000, 0x8000, 0x100000,
                                        0x8000, 0x1000);
  assert(exact.export_base == 0x100000);
  assert(exact.export_size == 0x8000);
  assert(exact.fd_offset == 0);

  // A subrange is aligned to the system page without leaving the allocation.
  auto sub = make_dmabuf_export_range(0x102123, 0x2345, 0x100000, 0x10000,
                                      0x1000);
  assert(sub.export_base == 0x102000);
  assert(sub.export_size == 0x3000);
  assert(sub.fd_offset == 0x123);
  assert(sub.fd_offset % 0x1000 == 0x102123 % 0x1000);

  // A request ending at the allocation boundary must not be rounded beyond it.
  auto tail = make_dmabuf_export_range(0x10f123, 0xedd, 0x100000, 0x10000,
                                       0x1000);
  assert(tail.export_base == 0x10f000);
  assert(tail.export_base + tail.export_size == 0x110000);

  rejects([] {
    (void)make_dmabuf_export_range(0x0fff, 1, 0x1000, 0x1000, 0x1000);
  });
  rejects([] {
    (void)make_dmabuf_export_range(0x1fff, 2, 0x1000, 0x1000, 0x1000);
  });
  rejects([] {
    (void)make_dmabuf_export_range(0x1000, 0, 0x1000, 0x1000, 0x1000);
  });
  rejects([] {
    (void)make_dmabuf_export_range(0x1000, 1, 0x1000, 0x1000, 3000);
  });
}
