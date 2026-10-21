#pragma once

#include "transport.hpp"
#ifdef USE_URMA
#if __has_include(<ub/umdk/urma/urma_api.h>)
#include <ub/umdk/urma/urma_api.h>
#else
#include <urma_api.h>
#endif
#endif

#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <stdexcept>
#include <vector>

#ifdef USE_URMA
struct UrmaRegistration;
#endif

// URMA-based transport for UCCL-EP.
//
// Maps the four UCCL proxy command classes onto URMA hardware primitives:
//   WRITE   -> URMA_OPC_WRITE (one-sided RDMA write)
//   ATOMIC  -> URMA_OPC_FADD  (hardware fetch-and-add, 64-bit operand)
//   BARRIER -> URMA_OPC_FADD  (FADD into a per-transport barrier word)
//   QUIET   -> drain the JFC until all outstanding WRs complete
//
// The barrier state machine (proxy.cpp) is the same as the CXI one: peers
// FADD into "barrier words" exported by each transport, and rank-0 releases
// by FADDing into per-peer release slots.
class UrmaTransport final : public EpTransport {
 public:
  UrmaTransport() = default;
  ~UrmaTransport() override;
  UrmaTransport(UrmaTransport const&) = delete;
  UrmaTransport& operator=(UrmaTransport const&) = delete;
  void init(ProxyCtx& ctx) override;
  void register_main_buffer(void* ptr, size_t len, int cuda_device) override;
  void register_atomic_buffer(void* ptr, size_t len, int cuda_device) override;
  LocalConnInfo local_info() const override;
  void connect_peer(int peer, RemoteConnInfo const& remote) override;
  void post_write(int dst_rank, uint64_t wr_id, uint64_t local_offset,
                  uint64_t remote_offset, uint32_t bytes,
                  bool low_latency) override;
  void post_atomic_add(int dst_rank, uint64_t wr_id,
                       uint64_t remote_atomic_offset, int64_t value,
                       bool fence) override;
  // Barrier support: FADD |value| into the remote peer's barrier word |slot|.
  // |wr_id| may be an internal sentinel (kUrmaInternalWrId) that the proxy
  // does not ack to the GPU.
  void post_barrier_atomic_add(int dst_rank, uint64_t wr_id, size_t slot,
                               uint64_t value);
  // Read a locally-owned barrier word (written by remote peers).
  uint64_t load_barrier_word(size_t slot) const;
  int poll(TransportCompletion* out, int max) override;
  void destroy() override;
  // Share CUDA-allocation ownership with every registration of this pointer.
  // The returned lease keeps the allocation alive; the final lease or
  // registration releases it after unregistering the segment.
  static std::shared_ptr<void> acquire_buffer_ownership(void* ptr, bool host,
                                                        int cuda_device);

 private:
  [[noreturn]] static void unavailable();
#ifdef USE_URMA
  ProxyCtx* ctx_ = nullptr;
  bool init_acquired_ = false;
  bool bound_ = false;
  bool failed_ = false;
  std::shared_ptr<urma_context_t> context_owner_;
  std::vector<std::shared_ptr<UrmaRegistration>> registrations_;
  urma_target_seg_t* register_segment(void* ptr, size_t len, bool gpu,
                                      bool atomic);
  void drain();
  void submit(urma_jfs_wr_t& wr);
  urma_context_t* urma_ctx_ = nullptr;
  urma_jfce_t* jfce_ = nullptr;
  urma_jfc_t* jfc_ = nullptr;
  urma_jfr_t* jfr_ = nullptr;
  urma_jetty_t* jetty_ = nullptr;
  void* main_buffer_ = nullptr;
  void* atomic_buffer_ = nullptr;
  size_t main_buffer_len_ = 0;
  size_t atomic_buffer_len_ = 0;
  // Locally registered segments (registered here, imported by peers).
  urma_target_seg_t* main_tseg_ = nullptr;
  urma_target_seg_t* atomic_tseg_ = nullptr;
  // Scratch buffer receiving the "old value" writeback of FADD ops.
  std::vector<uint64_t> atomic_operands_;
  std::vector<uint8_t> atomic_operand_used_;
  urma_target_seg_t* operand_tseg_ = nullptr;
  // Locally-owned barrier words; remote peers FADD into them.
  std::vector<uint64_t> barrier_words_;
  urma_target_seg_t* barrier_tseg_ = nullptr;
  LocalConnInfo local_info_{};
  // Per-peer imported objects.
  struct Peer {
    urma_target_jetty_t* tjetty = nullptr;
    urma_target_seg_t* main_tseg = nullptr;
    urma_target_seg_t* atomic_tseg = nullptr;
    urma_target_seg_t* barrier_tseg = nullptr;
  };
  std::vector<Peer> peers_;
  // In-flight op bookkeeping (mirrors the CXI OpContext pattern).
  struct OpContext {
    uint64_t wr_id = 0;
    bool is_write = false;
    bool is_atomic = false;
    size_t operand_slot = static_cast<size_t>(-1);
  };
  std::list<OpContext> op_contexts_;
  bool allocate_operand_slot(size_t& slot);
  void release_operand_slot(size_t slot);
#endif
};
