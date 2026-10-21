#include "urma_transport.hpp"
#include "common.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <tuple>
#ifndef USE_URMA
// -----------------------------------------------------------------------
// Stub build (USE_URMA not requested): every entry point throws.
// -----------------------------------------------------------------------
UrmaTransport::~UrmaTransport() {
  try {
    destroy();
  } catch (std::exception const& e) {
    // Continuing would let CUDA buffers or op contexts be freed while DMA
    // may still reference them. Teardown failure is deliberately fail-stop.
    std::fprintf(stderr, "URMA teardown failed: %s\n", e.what());
    std::terminate();
  }
}
void UrmaTransport::unavailable() {
  throw std::runtime_error("UrmaTransport requires building with USE_URMA=1");
}
void UrmaTransport::init(ProxyCtx&) { unavailable(); }
void UrmaTransport::register_main_buffer(void*, size_t, int) { unavailable(); }
void UrmaTransport::register_atomic_buffer(void*, size_t, int) {
  unavailable();
}
LocalConnInfo UrmaTransport::local_info() const { unavailable(); }
void UrmaTransport::connect_peer(int, RemoteConnInfo const&) { unavailable(); }
void UrmaTransport::post_write(int, uint64_t, uint64_t, uint64_t, uint32_t,
                               bool) {
  unavailable();
}
void UrmaTransport::post_atomic_add(int, uint64_t, uint64_t, int64_t, bool) {
  unavailable();
}
void UrmaTransport::post_barrier_atomic_add(int, uint64_t, size_t, uint64_t) {
  unavailable();
}
uint64_t UrmaTransport::load_barrier_word(size_t) const { unavailable(); }
int UrmaTransport::poll(TransportCompletion*, int) { unavailable(); }
void UrmaTransport::destroy() {}
std::shared_ptr<void> UrmaTransport::acquire_buffer_ownership(void*, bool,
                                                              int) {
  unavailable();
}
#else  // USE_URMA
#include <cuda.h>
#include <cuda_runtime_api.h>
// Registration objects are only created/released while g_resource_mutex is
// held. Jetty/JFC remain private to each transport; identical main/control
// ranges share a pin/token/mapping on the same device context.
struct UrmaAllocation {
  void* ptr = nullptr;
  bool owned = false;
  bool host = false;
  int cuda_device = -1;
  ~UrmaAllocation() {
    if (!owned) return;
    if (!host && cudaSetDevice(cuda_device) != cudaSuccess) {
      std::fprintf(stderr,
                   "URMA failed to select CUDA device before release\n");
      std::terminate();
    }
    if ((host ? cudaFreeHost(ptr) : cudaFree(ptr)) != cudaSuccess) {
      std::fprintf(stderr, "URMA failed to release CUDA allocation\n");
      std::terminate();
    }
  }
};
struct UrmaRegistration {
  // Destructor body unregisters before this member releases CUDA storage.
  std::shared_ptr<UrmaAllocation> allocation;
  std::shared_ptr<urma_context_t> context;
  urma_target_seg_t* seg = nullptr;
  urma_token_id_t* token = nullptr;
  ~UrmaRegistration() {
    if ((seg && urma_unregister_seg(seg) != URMA_SUCCESS) ||
        (token && urma_free_token_id(token) != URMA_SUCCESS)) {
      std::fprintf(
          stderr,
          "URMA failed to release registered memory; refusing unsafe free\n");
      std::terminate();
    }
  }
};
namespace {
// urma_init()/urma_uninit() are process-global and must be paired.
std::mutex g_urma_init_mutex;
int g_urma_init_refcount = 0;
std::mutex g_resource_mutex;
using ContextKey = std::pair<std::string, uint32_t>;
std::map<ContextKey, std::weak_ptr<urma_context_t> > g_contexts;
using SegmentKey = std::tuple<urma_context_t*, uint64_t, size_t, bool, bool>;
std::map<SegmentKey, std::weak_ptr<UrmaRegistration> > g_segments;
std::map<void*, std::weak_ptr<UrmaAllocation> > g_allocations;
// Number of barrier words exported by each transport.  Slot 0 collects
// node-arrival counters, slot 1 collects rank-0 release notifications
// (identical to the CXI transport's scheme).
constexpr size_t kNumBarrierWords = 8;
void check_urma(int rc, char const* what) {
  if (rc != URMA_SUCCESS) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "%s failed: rc=%d (%s)", what, rc,
                  std::strerror(rc));
    throw std::runtime_error(buf);
  }
}
urma_transport_mode_t urma_trans_mode_from_env() {
  char const* tm = std::getenv("UCCL_URMA_TM");
  if (tm && std::string(tm) != "rc") {
    throw std::runtime_error(
        "NVIDIA P2P URMA backend requires UCCL_URMA_TM=rc");
  }
  return URMA_TM_RC;
}
void check_cuda(cudaError_t status, char const* op) {
  if (status != cudaSuccess)
    throw std::runtime_error(std::string(op) + ": " +
                             cudaGetErrorString(status));
}
void check_range(uint64_t offset, uint64_t bytes, uint64_t length) {
  if (offset > length || bytes > length - offset)
    throw std::out_of_range("URMA operation exceeds registered segment");
}
constexpr uint32_t kUrmaWireVersion = 1;
// Address of |offset| inside a remotely imported segment.  Prefer the
// driver-assigned mapping address (MVA) and fall back to the registered VA.
inline uint64_t remote_seg_addr(urma_target_seg_t const* tseg,
                                uint64_t offset) {
  if (tseg->mva != 0) return tseg->mva + offset;
  return tseg->seg.ubva.va + offset;
}
}  // namespace
UrmaTransport::~UrmaTransport() {
  try {
    destroy();
  } catch (std::exception const& e) {
    // Continuing would let CUDA buffers or op contexts be freed while DMA
    // may still reference them. Teardown failure is deliberately fail-stop.
    std::fprintf(stderr, "URMA teardown failed: %s\n", e.what());
    std::terminate();
  }
}
void UrmaTransport::unavailable() {
  throw std::runtime_error("UrmaTransport is unavailable");
}
void UrmaTransport::init(ProxyCtx& ctx) {
  if (init_acquired_) throw std::logic_error("URMA already initialized");
  (void)urma_trans_mode_from_env();
  ctx_ = &ctx;
  try {
    {
      std::lock_guard<std::mutex> lock(g_urma_init_mutex);
      if (g_urma_init_refcount == 0) {
        check_urma(urma_init(nullptr), "urma_init");
      }
      ++g_urma_init_refcount;
      init_acquired_ = true;
    }
    int num_devices = 0;
    urma_device_t** dev_list = urma_get_device_list(&num_devices);
    if (!dev_list || num_devices == 0) {
      if (dev_list) urma_free_device_list(dev_list);
      throw std::runtime_error("UrmaTransport: no URMA devices found");
    }
    int dev_idx = 0;
    char const* dev_name = std::getenv("UCCL_URMA_DEV_NAME");
    if (dev_name) {
      bool found = false;
      for (int i = 0; i < num_devices; ++i) {
        if (dev_list[i] && std::string(dev_list[i]->name) == dev_name) {
          dev_idx = i;
          found = true;
          break;
        }
      }
      if (!found) {
        urma_free_device_list(dev_list);
        throw std::runtime_error(
            std::string("UrmaTransport: UCCL_URMA_DEV_NAME="
                        " device not found: ") +
            dev_name);
      }
    } else {
      // Default: skip bonding devices (their EID is a placeholder on this
      // platform); round-robin real devices by local rank.
      std::vector<int> real;
      for (int i = 0; i < num_devices; ++i) {
        if (dev_list[i] && std::string(dev_list[i]->name).find("bonding") ==
                               std::string::npos) {
          real.push_back(i);
        }
      }
      if (real.empty()) {
        urma_free_device_list(dev_list);
        throw std::runtime_error(
            "URMA P2P requires a direct UB device, not bonding");
      }
      int const prefer = ctx_->local_rank >= 0 ? ctx_->local_rank : 0;
      dev_idx = real[static_cast<size_t>(prefer) % real.size()];
    }
    urma_device_t* dev = dev_list[dev_idx];
    if (!dev || dev->type != URMA_TRANSPORT_UB ||
        std::string(dev->name).find("bonding") != std::string::npos) {
      urma_free_device_list(dev_list);
      throw std::runtime_error("URMA P2P requires a direct UB device");
    }
    std::fprintf(stderr, "[URMA] rank=%d thread=%d using device %d/%d: %s\n",
                 ctx_->local_rank, ctx_->thread_idx, dev_idx, num_devices,
                 dev ? dev->name : "");
    // Any EID of the device works for context creation; use the first.
    uint32_t eid_cnt = 0;
    urma_eid_info_t* eid_list = urma_get_eid_list(dev, &eid_cnt);
    if (!eid_list || eid_cnt == 0) {
      if (eid_list) urma_free_eid_list(eid_list);
      urma_free_device_list(dev_list);
      throw std::runtime_error("UrmaTransport: device has no EIDs");
    }
    {
      std::lock_guard<std::mutex> lock(g_resource_mutex);
      ContextKey key{dev->name, eid_list[0].eid_index};
      context_owner_ = g_contexts[key].lock();
      if (!context_owner_) {
        auto* raw = urma_create_context(dev, eid_list[0].eid_index);
        if (raw) {
          context_owner_ =
              std::shared_ptr<urma_context_t>(raw, [](urma_context_t* c) {
                if (urma_delete_context(c) != URMA_SUCCESS) std::terminate();
              });
          g_contexts[key] = context_owner_;
        }
      }
      urma_ctx_ = context_owner_.get();
    }
    urma_free_eid_list(eid_list);
    urma_free_device_list(dev_list);
    if (!urma_ctx_) {
      throw std::runtime_error("urma_create_context failed");
    }
    urma_jfce_t* jfce = urma_create_jfce(urma_ctx_);
    if (!jfce) {
      throw std::runtime_error("urma_create_jfce failed");
    }
    jfce_ = jfce;
    urma_jfc_cfg_t jfc_cfg = {};
    jfc_cfg.depth = kMaxOutstandingSends * 2;
    jfc_cfg.jfce = jfce;
    jfc_ = urma_create_jfc(urma_ctx_, &jfc_cfg);
    if (!jfc_) {
      throw std::runtime_error("urma_create_jfc failed");
    }
    // RC jetty creation requires a JFR (shared) even for one-sided usage.
    urma_jfr_cfg_t jfr_cfg = {};
    jfr_cfg.depth = 1;
    jfr_cfg.trans_mode = urma_trans_mode_from_env();
    jfr_cfg.max_sge = 1;
    jfr_cfg.jfc = jfc_;
    jfr_cfg.token_value.token = 0;
    urma_jfr_t* jfr = urma_create_jfr(urma_ctx_, &jfr_cfg);
    if (!jfr) {
      throw std::runtime_error("urma_create_jfr failed");
    }
    jfr_ = jfr;
    urma_jetty_cfg_t jetty_cfg = {};
    jetty_cfg.flag.bs.share_jfr = 1; /* URMA_SHARE_JFR */
    jetty_cfg.jfs_cfg.depth = kMaxOutstandingSends;
    jetty_cfg.jfs_cfg.trans_mode = urma_trans_mode_from_env();
    jetty_cfg.jfs_cfg.max_sge = 1;
    jetty_cfg.jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
    jetty_cfg.jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
    jetty_cfg.jfs_cfg.jfc = jfc_;
    jetty_cfg.shared.jfr = jfr;
    jetty_cfg.shared.jfc = jfc_;
    jetty_ = urma_create_jetty(urma_ctx_, &jetty_cfg);
    if (!jetty_) {
      throw std::runtime_error("urma_create_jetty failed");
    }
    // Local scratch target for FADD old-value writeback.
    atomic_operands_.assign(ProxyCtx::kMaxAtomicOps, 0);
    atomic_operand_used_.assign(ProxyCtx::kMaxAtomicOps, 0);
    operand_tseg_ = register_segment(atomic_operands_.data(),
                                     atomic_operands_.size() * sizeof(uint64_t),
                                     false, false);
    barrier_words_.assign(kNumBarrierWords, 0);
    barrier_tseg_ =
        register_segment(barrier_words_.data(),
                         barrier_words_.size() * sizeof(uint64_t), false, true);
    std::fprintf(stderr, "[URMA] transport initialized (trans_mode=%d)\n",
                 static_cast<int>(jetty_cfg.jfs_cfg.trans_mode));
  } catch (...) {
    destroy();
    throw;
  }
}
std::shared_ptr<void> UrmaTransport::acquire_buffer_ownership(void* ptr,
                                                              bool host,
                                                              int cuda_device) {
  if (!ptr || (!host && cuda_device < 0))
    throw std::invalid_argument("invalid URMA allocation ownership");
  std::lock_guard<std::mutex> lock(g_resource_mutex);
  auto allocation = g_allocations[ptr].lock();
  if (!allocation) {
    allocation = std::make_shared<UrmaAllocation>();
    allocation->ptr = ptr;
    g_allocations[ptr] = allocation;
  }
  if (allocation->owned &&
      (allocation->host != host || allocation->cuda_device != cuda_device))
    throw std::logic_error("conflicting URMA allocation ownership");
  allocation->owned = true;
  allocation->host = host;
  allocation->cuda_device = cuda_device;
  return allocation;
}

urma_target_seg_t* UrmaTransport::register_segment(void* ptr, size_t len,
                                                   bool gpu, bool atomic) {
  if (!ptr || !len || len - 1 > UINT64_MAX - reinterpret_cast<uint64_t>(ptr))
    throw std::invalid_argument("invalid URMA segment range");
  std::lock_guard<std::mutex> lock(g_resource_mutex);
  SegmentKey key{urma_ctx_, reinterpret_cast<uint64_t>(ptr), len, gpu, atomic};
  if (auto owner = g_segments[key].lock()) {
    registrations_.push_back(owner);
    return owner->seg;
  }
  // Do not let a second, partially overlapping GPU mapping replace a live
  // MATT entry. Exact ranges above share ownership; differing ranges fail.
  for (auto it = g_segments.begin(); it != g_segments.end();) {
    auto owner = it->second.lock();
    if (!owner) {
      it = g_segments.erase(it);
      continue;
    }
    auto const& k = it->first;
    if (gpu && std::get<3>(k) && std::get<0>(k) == urma_ctx_) {
      uint64_t va = reinterpret_cast<uint64_t>(ptr), other = std::get<1>(k);
      constexpr uint64_t page_mask = ~uint64_t(65535);
      uint64_t end_page = (va + len - 1) & page_mask;
      uint64_t other_end_page = (other + std::get<2>(k) - 1) & page_mask;
      if ((va & page_mask) <= other_end_page && (other & page_mask) <= end_page)
        throw std::invalid_argument(
            "overlapping URMA GPU registrations require identical "
            "range/permissions");
    }
    ++it;
  }
  auto allocation = g_allocations[ptr].lock();
  if (!allocation) {
    allocation = std::make_shared<UrmaAllocation>();
    allocation->ptr = ptr;
    g_allocations[ptr] = allocation;
  }
  auto owner = std::make_shared<UrmaRegistration>();
  owner->allocation = allocation;
  owner->context = context_owner_;
  auto* token = urma_alloc_token_id(urma_ctx_);
  if (!token) throw std::runtime_error("urma_alloc_token_id failed");
  urma_seg_cfg_t cfg{};
  cfg.va = reinterpret_cast<uint64_t>(ptr);
  cfg.len = len;
  cfg.token_id = token;
  cfg.flag.bs.token_id_valid = URMA_TOKEN_ID_VALID;
  cfg.flag.bs.token_policy = URMA_TOKEN_NONE;
  cfg.flag.bs.cacheable = URMA_NON_CACHEABLE;
  cfg.flag.bs.non_pin = 0;
  cfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
  if (atomic) cfg.flag.bs.access |= URMA_ACCESS_ATOMIC;
  cfg.is_gpu_seg = gpu ? 1 : 0;
  auto* seg = urma_register_seg(urma_ctx_, &cfg);
  if (!seg) {
    check_urma(urma_free_token_id(token), "urma_free_token_id");
    throw std::runtime_error(
        gpu ? "URMA NVIDIA P2P segment registration failed (no host fallback)"
            : "URMA host segment registration failed");
  }
  owner->seg = seg;
  owner->token = token;
  registrations_.push_back(owner);
  g_segments[key] = owner;
  return seg;
}
void UrmaTransport::register_main_buffer(void* ptr, size_t len, int device) {
  if (!urma_ctx_ || main_tseg_)
    throw std::logic_error("invalid main registration state");
  check_cuda(cudaSetDevice(device), "cudaSetDevice");
  cudaPointerAttributes attr{};
  check_cuda(cudaPointerGetAttributes(&attr, ptr),
             "cudaPointerGetAttributes(main)");
  if (attr.type != cudaMemoryTypeDevice || attr.device != device)
    throw std::invalid_argument(
        "URMA main buffer must be NVIDIA device memory on the selected CUDA "
        "ordinal");
  // Same prerequisite as the working NVIDIA P2P perftest; keep synchronous
  // CUDA memory operations ordered before handing the VA to the NIC driver.
  unsigned int sync_memops = 1;
  if (cuPointerSetAttribute(&sync_memops, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                            reinterpret_cast<CUdeviceptr>(ptr)) != CUDA_SUCCESS)
    throw std::runtime_error("cuPointerSetAttribute(SYNC_MEMOPS) failed");
  main_tseg_ = register_segment(ptr, len, true, false);
  main_buffer_ = ptr;
  main_buffer_len_ = len;
  local_info_.addr = reinterpret_cast<uintptr_t>(ptr);
  local_info_.len = len;
}
void UrmaTransport::register_atomic_buffer(void* ptr, size_t len, int device) {
  if (!urma_ctx_ || atomic_tseg_)
    throw std::logic_error("invalid control registration state");
  check_cuda(cudaSetDevice(device), "cudaSetDevice");
  cudaPointerAttributes attr{};
  check_cuda(cudaPointerGetAttributes(&attr, ptr),
             "cudaPointerGetAttributes(control)");
  if (attr.type != cudaMemoryTypeHost || attr.devicePointer != ptr)
    throw std::invalid_argument(
        "URMA control buffer requires CUDA mapped host memory with identical "
        "UVA");
  atomic_tseg_ = register_segment(ptr, len, false, true);
  atomic_buffer_ = ptr;
  atomic_buffer_len_ = len;
  local_info_.atomic_buffer_addr = reinterpret_cast<uintptr_t>(ptr);
  local_info_.atomic_buffer_len = len;
}
LocalConnInfo UrmaTransport::local_info() const {
  if (!jetty_ || !main_tseg_ || !atomic_tseg_ || !barrier_tseg_)
    throw std::logic_error("URMA local_info before registration");
  LocalConnInfo info = local_info_;
  info.urma_wire_version = kUrmaWireVersion;
  urma_rjetty_t rjetty{};
  rjetty.jetty_id = jetty_->jetty_id;
  rjetty.trans_mode = URMA_TM_RC;
  rjetty.type = URMA_JETTY;
  rjetty.tp_type = URMA_RTP;
  static_assert(sizeof(rjetty) <= LocalConnInfo::kMaxUrmaBlob);
  static_assert(sizeof(urma_seg_t) <= LocalConnInfo::kMaxUrmaBlob);
  std::memcpy(info.urma_rjetty, &rjetty, sizeof(rjetty));
  info.urma_rjetty_len = sizeof(rjetty);
  std::memcpy(info.urma_main_seg, &main_tseg_->seg, sizeof(urma_seg_t));
  std::memcpy(info.urma_atomic_seg, &atomic_tseg_->seg, sizeof(urma_seg_t));
  std::memcpy(info.urma_barrier_seg, &barrier_tseg_->seg, sizeof(urma_seg_t));
  info.urma_main_seg_len = info.urma_atomic_seg_len =
      info.urma_barrier_seg_len = sizeof(urma_seg_t);
  return info;
}
void UrmaTransport::connect_peer(int peer, RemoteConnInfo const& remote) {
  if (!jetty_ || peer < 0) throw std::invalid_argument("invalid URMA peer");
  if (bound_)
    throw std::logic_error("each RC transport binds exactly one peer");
  if (remote.urma_wire_version != kUrmaWireVersion ||
      remote.urma_rjetty_len != sizeof(urma_rjetty_t) ||
      remote.urma_main_seg_len != sizeof(urma_seg_t) ||
      remote.urma_atomic_seg_len != sizeof(urma_seg_t) ||
      remote.urma_barrier_seg_len != sizeof(urma_seg_t))
    throw std::invalid_argument("URMA descriptor ABI/version mismatch");
  // Copy into aligned native objects; the byte arrays need not be aligned.
  urma_rjetty_t rjetty{};
  urma_seg_t segs[3]{};
  std::memcpy(&rjetty, remote.urma_rjetty, sizeof(rjetty));
  std::memcpy(&segs[0], remote.urma_main_seg, sizeof(urma_seg_t));
  std::memcpy(&segs[1], remote.urma_atomic_seg, sizeof(urma_seg_t));
  std::memcpy(&segs[2], remote.urma_barrier_seg, sizeof(urma_seg_t));
  if (rjetty.trans_mode != URMA_TM_RC || rjetty.type != URMA_JETTY ||
      rjetty.tp_type != URMA_RTP || rjetty.flag.bs.has_drv_ext)
    throw std::invalid_argument("unsupported remote URMA jetty");
  for (int i = 0; i < 3; ++i) {
    uint32_t access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
    if (i != 0) access |= URMA_ACCESS_ATOMIC;
    if (!segs[i].len || !segs[i].ubva.va ||
        segs[i].len > UINT64_MAX - segs[i].ubva.va ||
        (segs[i].attr.bs.access & access) != access ||
        segs[i].attr.bs.token_policy != URMA_TOKEN_NONE)
      throw std::invalid_argument("invalid remote URMA segment");
  }
  if (segs[2].len < kNumBarrierWords * sizeof(uint64_t))
    throw std::invalid_argument("remote barrier segment is too short");
  peers_.resize(static_cast<size_t>(peer) + 1);
  Peer& p = peers_[peer];
  urma_token_t token{};
  try {
    p.tjetty = urma_import_jetty(urma_ctx_, &rjetty, &token);
    if (!p.tjetty) throw std::runtime_error("urma_import_jetty failed");
    urma_target_seg_t** targets[] = {&p.main_tseg, &p.atomic_tseg,
                                     &p.barrier_tseg};
    for (int i = 0; i < 3; ++i) {
      urma_import_seg_flag_t flag{};
      flag.bs.mapping = URMA_SEG_NOMAP;  // matches the tested P2P perftest
      flag.bs.cacheable = URMA_NON_CACHEABLE;
      flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
      if (i) flag.bs.access |= URMA_ACCESS_ATOMIC;
      *targets[i] = urma_import_seg(urma_ctx_, &segs[i], &token, 0, flag);
      if (!*targets[i]) throw std::runtime_error("urma_import_seg failed");
    }
    check_urma(urma_bind_jetty(jetty_, p.tjetty), "urma_bind_jetty");
    bound_ = true;
  } catch (...) {
    if (p.barrier_tseg)
      check_urma(urma_unimport_seg(p.barrier_tseg), "unimport barrier");
    if (p.atomic_tseg)
      check_urma(urma_unimport_seg(p.atomic_tseg), "unimport control");
    if (p.main_tseg)
      check_urma(urma_unimport_seg(p.main_tseg), "unimport main");
    if (p.tjetty) check_urma(urma_unimport_jetty(p.tjetty), "unimport jetty");
    p = {};
    throw;
  }
}
void UrmaTransport::post_write(int dst_rank, uint64_t wr_id,
                               uint64_t local_offset, uint64_t remote_offset,
                               uint32_t bytes, bool) {
  if (dst_rank < 0 || static_cast<size_t>(dst_rank) >= peers_.size() ||
      !peers_[dst_rank].tjetty) {
    throw std::runtime_error("URMA peer is not connected");
  }
  if (!main_tseg_ || !main_buffer_) {
    throw std::runtime_error("URMA main buffer MR is not registered");
  }
  check_range(local_offset, bytes, main_buffer_len_);
  Peer& p = peers_[dst_rank];
  if (!p.main_tseg) {
    throw std::runtime_error("URMA peer main seg is not imported");
  }
  check_range(remote_offset, bytes, p.main_tseg->seg.len);
  if (!bytes) throw std::invalid_argument("zero-length URMA write");
  op_contexts_.push_back({});
  OpContext& op = op_contexts_.back();
  op.wr_id = wr_id;
  op.is_write = true;
  urma_sge_t src_sge = {};
  src_sge.addr = reinterpret_cast<uint64_t>(main_buffer_) + local_offset;
  src_sge.len = bytes;
  src_sge.tseg = main_tseg_;
  urma_sge_t dst_sge = {};
  dst_sge.addr = remote_seg_addr(p.main_tseg, remote_offset);
  dst_sge.len = bytes;
  dst_sge.tseg = p.main_tseg;
  urma_jfs_wr_t wr = {};
  wr.opcode = URMA_OPC_WRITE;
  wr.flag.bs.complete_enable = 1;
  wr.flag.bs.place_order = URMA_STRONG_ORDER;
  wr.tjetty = p.tjetty;
  wr.user_ctx = reinterpret_cast<uint64_t>(&op);
  wr.rw.src.sge = &src_sge;
  wr.rw.src.num_sge = 1;
  wr.rw.dst.sge = &dst_sge;
  wr.rw.dst.num_sge = 1;
  submit(wr);
}
bool UrmaTransport::allocate_operand_slot(size_t& slot) {
  for (size_t i = 0; i < atomic_operand_used_.size(); ++i) {
    if (!atomic_operand_used_[i]) {
      atomic_operand_used_[i] = 1;
      slot = i;
      return true;
    }
  }
  return false;
}
void UrmaTransport::release_operand_slot(size_t slot) {
  if (slot < atomic_operand_used_.size()) {
    atomic_operand_used_[slot] = 0;
  }
}
void UrmaTransport::post_atomic_add(int dst_rank, uint64_t wr_id,
                                    uint64_t remote_atomic_offset,
                                    int64_t value, bool fence) {
  if (dst_rank < 0 || static_cast<size_t>(dst_rank) >= peers_.size() ||
      !peers_[dst_rank].tjetty) {
    throw std::runtime_error("URMA peer is not connected");
  }
  Peer& p = peers_[dst_rank];
  if (!p.atomic_tseg) {
    throw std::runtime_error("URMA peer atomic seg is not imported");
  }
  check_range(remote_atomic_offset, sizeof(uint64_t), p.atomic_tseg->seg.len);
  if ((p.atomic_tseg->seg.ubva.va + remote_atomic_offset) % alignof(uint64_t))
    throw std::invalid_argument("unaligned URMA atomic");
  size_t slot = 0;
  if (!allocate_operand_slot(slot)) {
    throw std::runtime_error("no free URMA atomic operand slot");
  }
  atomic_operands_[slot] = 0;
  op_contexts_.push_back({});
  OpContext& op = op_contexts_.back();
  op.wr_id = wr_id;
  op.is_atomic = true;
  op.operand_slot = slot;
  urma_sge_t dst_sge = {};
  dst_sge.addr = remote_seg_addr(p.atomic_tseg, remote_atomic_offset);
  dst_sge.len = sizeof(uint64_t);
  dst_sge.tseg = p.atomic_tseg;
  urma_sge_t src_sge = {};
  src_sge.addr = reinterpret_cast<uint64_t>(&atomic_operands_[slot]);
  src_sge.len = sizeof(uint64_t);
  src_sge.tseg = operand_tseg_;
  urma_jfs_wr_t wr = {};
  wr.opcode = URMA_OPC_FADD;
  wr.flag.bs.complete_enable = 1;
  wr.flag.bs.place_order = URMA_STRONG_ORDER;
  wr.flag.bs.fence = fence ? URMA_FENCE_ENABLE : URMA_FENCE_DISABLE;
  wr.tjetty = p.tjetty;
  wr.user_ctx = reinterpret_cast<uint64_t>(&op);
  wr.faa.dst = &dst_sge;
  wr.faa.src = &src_sge;
  wr.faa.operand = static_cast<uint64_t>(value);
  submit(wr);
}
void UrmaTransport::post_barrier_atomic_add(int dst_rank, uint64_t wr_id,
                                            size_t slot, uint64_t value) {
  if (dst_rank < 0 || static_cast<size_t>(dst_rank) >= peers_.size() ||
      !peers_[dst_rank].tjetty) {
    throw std::runtime_error("URMA peer is not connected");
  }
  Peer& p = peers_[dst_rank];
  if (!p.barrier_tseg) {
    throw std::runtime_error("URMA peer barrier seg is not imported");
  }
  if (slot * sizeof(uint64_t) >= p.barrier_tseg->seg.len) {
    throw std::runtime_error("URMA barrier slot out of range");
  }
  size_t operand_slot = 0;
  if (!allocate_operand_slot(operand_slot)) {
    throw std::runtime_error("no free URMA barrier operand slot");
  }
  atomic_operands_[operand_slot] = 0;
  op_contexts_.push_back({});
  OpContext& op = op_contexts_.back();
  op.wr_id = wr_id;
  op.is_atomic = true;
  op.operand_slot = operand_slot;
  urma_sge_t dst_sge = {};
  dst_sge.addr = remote_seg_addr(p.barrier_tseg, slot * sizeof(uint64_t));
  dst_sge.len = sizeof(uint64_t);
  dst_sge.tseg = p.barrier_tseg;
  urma_sge_t src_sge = {};
  src_sge.addr = reinterpret_cast<uint64_t>(&atomic_operands_[operand_slot]);
  src_sge.len = sizeof(uint64_t);
  src_sge.tseg = operand_tseg_;
  urma_jfs_wr_t wr = {};
  wr.opcode = URMA_OPC_FADD;
  wr.flag.bs.complete_enable = 1;
  wr.flag.bs.place_order = URMA_STRONG_ORDER;
  wr.flag.bs.fence = URMA_FENCE_ENABLE;
  wr.tjetty = p.tjetty;
  wr.user_ctx = reinterpret_cast<uint64_t>(&op);
  wr.faa.dst = &dst_sge;
  wr.faa.src = &src_sge;
  wr.faa.operand = value;
  submit(wr);
}
uint64_t UrmaTransport::load_barrier_word(size_t slot) const {
  if (slot >= barrier_words_.size()) {
    throw std::runtime_error("URMA barrier slot out of range");
  }
  return __atomic_load_n(&barrier_words_[slot], __ATOMIC_ACQUIRE);
}
int UrmaTransport::poll(TransportCompletion* out, int max) {
  if (!jfc_ || max <= 0) return 0;
  // urma_poll_jfc documents at most 16 CRs per call for RDMA devices.
  if (max > 16) max = 16;
  urma_cr_t crs[16]{};
  int n = urma_poll_jfc(jfc_, max, crs);
  if (n < 0) {
    throw std::runtime_error("urma_poll_jfc failed");
  }
  for (int i = 0; i < n; ++i) {
    auto* op = reinterpret_cast<OpContext*>(crs[i].user_ctx);
    auto it = std::find_if(op_contexts_.begin(), op_contexts_.end(),
                           [op](OpContext const& x) { return &x == op; });
    if (it == op_contexts_.end()) {
      failed_ = true;
      throw std::runtime_error("unexpected URMA completion context");
    }
    out[i] = {};
    out[i].wr_id = it->wr_id;
    out[i].is_write = it->is_write;
    out[i].is_atomic = it->is_atomic;
    if (it->operand_slot != static_cast<size_t>(-1))
      release_operand_slot(it->operand_slot);
    op_contexts_.erase(it);
    if (crs[i].status != URMA_CR_SUCCESS) failed_ = true;
    out[i].data = crs[i].imm_data;
    out[i].is_recv_data = false;
    out[i].status =
        crs[i].status == URMA_CR_SUCCESS ? 0 : static_cast<int>(crs[i].status);
  }
  return n;
}
void UrmaTransport::submit(urma_jfs_wr_t& wr) {
  urma_jfs_wr_t* bad = nullptr;
  int rc = failed_ ? URMA_EINVAL : urma_post_jetty_send_wr(jetty_, &wr, &bad);
  if (rc != URMA_SUCCESS) {
    auto& op = op_contexts_.back();
    if (op.operand_slot != static_cast<size_t>(-1))
      release_operand_slot(op.operand_slot);
    op_contexts_.pop_back();
    check_urma(rc, "urma_post_jetty_send_wr");
  }
}
void UrmaTransport::drain() {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  TransportCompletion out[16];
  while (!op_contexts_.empty()) {
    poll(out, 16);
    if (std::chrono::steady_clock::now() >= deadline)
      throw std::runtime_error("URMA teardown timed out with in-flight DMA");
    std::this_thread::yield();
  }
}
void UrmaTransport::destroy() {
  drain();
  if (bound_) {
    check_urma(urma_unbind_jetty(jetty_), "urma_unbind_jetty");
    bound_ = false;
  }
  for (auto& p : peers_) {
    if (p.main_tseg) {
      check_urma(urma_unimport_seg(p.main_tseg), "unimport main");
      p.main_tseg = nullptr;
    }
    if (p.atomic_tseg) {
      check_urma(urma_unimport_seg(p.atomic_tseg), "unimport control");
      p.atomic_tseg = nullptr;
    }
    if (p.barrier_tseg) {
      check_urma(urma_unimport_seg(p.barrier_tseg), "unimport barrier");
      p.barrier_tseg = nullptr;
    }
    if (p.tjetty) {
      check_urma(urma_unimport_jetty(p.tjetty), "unimport jetty");
      p.tjetty = nullptr;
    }
  }
  peers_.clear();
  if (jetty_) {
    check_urma(urma_delete_jetty(jetty_), "delete jetty");
    jetty_ = nullptr;
  }
  if (jfr_) {
    check_urma(urma_delete_jfr(jfr_), "delete jfr");
    jfr_ = nullptr;
  }
  if (jfc_) {
    check_urma(urma_delete_jfc(jfc_), "delete jfc");
    jfc_ = nullptr;
  }
  if (jfce_) {
    check_urma(urma_delete_jfce(jfce_), "delete jfce");
    jfce_ = nullptr;
  }
  {
    std::lock_guard<std::mutex> lock(g_resource_mutex);
    registrations_.clear();
    barrier_tseg_ = operand_tseg_ = atomic_tseg_ = main_tseg_ = nullptr;
    for (auto it = g_allocations.begin(); it != g_allocations.end();)
      if (it->second.expired())
        it = g_allocations.erase(it);
      else
        ++it;
    context_owner_.reset();
    urma_ctx_ = nullptr;
    for (auto it = g_contexts.begin(); it != g_contexts.end();)
      if (it->second.expired())
        it = g_contexts.erase(it);
      else
        ++it;
    for (auto it = g_segments.begin(); it != g_segments.end();)
      if (it->second.expired())
        it = g_segments.erase(it);
      else
        ++it;
  }
  if (init_acquired_) {
    std::lock_guard<std::mutex> lock(g_urma_init_mutex);
    if (--g_urma_init_refcount == 0) check_urma(urma_uninit(), "urma_uninit");
    init_acquired_ = false;
  }
  failed_ = false;
}
#endif  // USE_URMA
