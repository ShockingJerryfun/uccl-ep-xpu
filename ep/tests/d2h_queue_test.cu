#include "d2h_queue_device.cuh"
#include "d2h_queue_host.hpp"
#include "ep_configs.cuh"
#include "exception.cuh"
#include <array>

constexpr int kProducers = 8;
constexpr int kCommands = 4096;

__global__ void produce(d2hq::D2HHandle queue) {
  if (threadIdx.x != 0) return;
  int const producer = blockIdx.x;
  uint64_t slot = 0;
  for (int sequence = 0; sequence < kCommands; ++sequence) {
    TransferCmd cmd{};
    cmd.cmd_type = make_cmd_type(CmdType::WRITE, sequence % 2, sequence % 3);
    cmd.dst_rank = producer;
    cmd.bytes_and_val = sequence ^ (producer << 16);
    cmd.req_rptr = sequence;
    cmd.req_lptr = ~static_cast<uint32_t>(sequence);
    cmd.expert_idx = producer + 1;
    queue.atomic_set_and_commit(cmd, &slot);
  }
  queue.fifo.sync(slot, -1);
}

int main() {
  mscclpp::Fifo fifo(64);
  d2hq::D2HHandle queue;
  queue.init_from_host_value(fifo.deviceHandle());
  std::array<int, kProducers> next{};
  bool valid = true;
  produce<<<kProducers, WARP_SIZE>>>(queue);
  CUDA_CHECK(cudaGetLastError());
  for (int i = 0; i < kProducers * kCommands; ++i) {
    auto trigger = fifo.poll();
    while (trigger.fst == 0) trigger = fifo.poll();
    auto cmd = d2hq::decode_from_trigger(trigger);
    int const producer = cmd.dst_rank;
    if (producer < kProducers) {
      int const sequence = next[producer]++;
      valid &= cmd.cmd_type ==
               make_cmd_type(CmdType::WRITE, sequence % 2, sequence % 3);
      valid &= cmd.bytes_and_val == (sequence ^ (producer << 16));
      valid &= cmd.req_rptr == sequence;
      valid &= cmd.req_lptr == ~static_cast<uint32_t>(sequence);
      valid &= cmd.expert_idx == producer + 1;
    } else {
      valid = false;
    }
    fifo.pop();
  }
  CUDA_CHECK(cudaDeviceSynchronize());
  for (int count : next) valid &= count == kCommands;
  return valid ? 0 : 1;
}
