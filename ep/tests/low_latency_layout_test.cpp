#include "ep_config.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <vector>

int main() {
  for (int hidden : {2048, 2560, 4096, 5120, 6144, 7168, 8192}) {
    for (int capacity : {2, 4, 8, 128}) {
      for (int experts : {1, 2, 8}) {
        int const ranks = experts == 1 ? 1 : 2;
        auto const bytes = uccl::get_low_latency_rdma_size_hint(
            capacity, hidden, ranks, experts);
        std::vector<std::uint8_t> storage(bytes + 255);
        auto base_addr = (reinterpret_cast<std::uintptr_t>(storage.data()) + 255) &
                         ~std::uintptr_t{255};
        auto* base = reinterpret_cast<void*>(base_addr);
        uccl::LowLatencyLayout layout(base, capacity, hidden, ranks, experts);
        auto send0 = reinterpret_cast<std::uintptr_t>(
            layout.buffers[0].dispatch_rdma_send_buffer);
        auto send1 = reinterpret_cast<std::uintptr_t>(
            layout.buffers[1].dispatch_rdma_send_buffer);
        auto recv0 = reinterpret_cast<std::uintptr_t>(
            layout.buffers[0].dispatch_rdma_recv_data_buffer);
        auto recv1 = reinterpret_cast<std::uintptr_t>(
            layout.buffers[1].dispatch_rdma_recv_data_buffer);
        assert(layout.total_bytes <= bytes);
        assert(base_addr <= send0 && send0 < send1 && send1 < recv0 &&
               recv0 < recv1 && recv1 < base_addr + layout.total_bytes);

        for (bool fp8 : {false, true}) {
          std::size_t message = 16 + (fp8 ? hidden + hidden / 128 * 4
                                            : hidden * 2);
          auto stride = uccl::ll_dispatch_send_stride(message);
          auto required = (capacity - 1) * stride + message;
          assert(required <= send1 - send0);
          assert(required <= recv0 - send1);
#if defined(USE_METAX) && !defined(PER_EXPERT_BATCHING)
          assert(stride % 256 == 0);
          assert(send0 % 256 == 0 && send1 % 256 == 0);
#else
          assert(stride == message);
#endif
        }
      }
    }
  }
  std::cout << "Low-latency send layouts passed\n";
}
