#include "urma_transport.hpp"
#include "common.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#ifndef USE_URMA
// -----------------------------------------------------------------------
// Stub build (USE_URMA not requested): every entry point throws.
// -----------------------------------------------------------------------
UrmaTransport::~UrmaTransport() { destroy(); }
void UrmaTransport::unavailable() {
  throw std::runtime_error(
      "UrmaTransport requires building with USE_URMA=1");
}
void UrmaTransport::init(ProxyCtx&) { unavailable(); }
void UrmaTransport::register_main_buffer(void*, size_t, int) { unavailable(); }
void UrmaTransport::register_atomic_buffer(void*, size_t, int) {
  unavailable();
}
LocalConnInfo UrmaTransport::local_info() const {
  unavailable();
}
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
uint64_t UrmaTransport::load_barrier_word(size_t) const {
  unavailable();
}
int UrmaTransport::poll(TransportCompletion*, int) { unavailable(); }
void UrmaTransport::destroy() {}
#else  // USE_URMA
#include <ub/umdk/urma/urma_api.h>
namespace {
// urma_init()/urma_uninit() are process-global and must be paired.
std::mutex g_urma_init_mutex;
int g_urma_init_refcount = 0;
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
  if (tm && std::string(tm) == "rc") return URMA_TM_RC;
  if (tm && std::string(tm) == "um") return URMA_TM_UM;
  // Default: reliable connection.  RC is the only mode the udma driver's
  // rjetty drv-extension currently accepts for import on this platform
  // (UM is rejected client-side by liburma).
  return URMA_TM_RC;
}
// Address of |offset| inside a remotely imported segment.  Prefer the
// driver-assigned mapping address (MVA) and fall back to the registered VA.
inline uint64_t remote_seg_addr(urma_target_seg_t const* tseg,
                                uint64_t offset) {
  if (tseg->mva != 0) return tseg->mva + offset;
  return tseg->seg.ubva.va + offset;
}
}  // namespace
UrmaTransport::~UrmaTransport() { destroy(); }
void UrmaTransport::unavailable() {
  throw std::runtime_error("UrmaTransport is unavailable");
}
void UrmaTransport::init(ProxyCtx& ctx) {
  ctx_ = &ctx;
  {
    std::lock_guard<std::mutex> lock(g_urma_init_mutex);
    if (g_urma_init_refcount == 0) {
      check_urma(urma_init(nullptr), "urma_init");
    }
    ++g_urma_init_refcount;
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
      throw std::runtime_error(std::string("UrmaTransport: UCCL_URMA_DEV_NAME="
                                           " device not found: ") +
                               dev_name);
    }
  } else {
    // Default: skip bonding devices (their EID is a placeholder on this
    // platform); round-robin real devices by local rank.
    std::vector<int> real;
    for (int i = 0; i < num_devices; ++i) {
      if (dev_list[i] &&
          std::string(dev_list[i]->name).find("bonding") == std::string::npos) {
        real.push_back(i);
      }
    }
    if (real.empty()) {
      real.push_back(0);
    }
    int const prefer = ctx_->local_rank >= 0 ? ctx_->local_rank : 0;
    dev_idx = real[static_cast<size_t>(prefer) % real.size()];
  }
  urma_device_t* dev = dev_list[dev_idx];
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
  urma_ctx_ = urma_create_context(dev, eid_list[0].eid_index);
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
  {
    urma_seg_cfg_t seg_cfg = {};
    seg_cfg.va = reinterpret_cast<uint64_t>(atomic_operands_.data());
    seg_cfg.len =
        atomic_operands_.size() * sizeof(atomic_operands_[0]);
    seg_cfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
    operand_tseg_ = urma_register_seg(urma_ctx_, &seg_cfg);
    if (!operand_tseg_) {
      throw std::runtime_error("urma_register_seg(operand) failed");
    }
  }
  // Barrier words exported to peers.
  barrier_words_.assign(kNumBarrierWords, 0);
  {
    urma_seg_cfg_t seg_cfg = {};
    seg_cfg.va = reinterpret_cast<uint64_t>(barrier_words_.data());
    seg_cfg.len = barrier_words_.size() * sizeof(barrier_words_[0]);
    seg_cfg.flag.bs.access =
        URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
    barrier_tseg_ = urma_register_seg(urma_ctx_, &seg_cfg);
    if (!barrier_tseg_) {
      throw std::runtime_error("urma_register_seg(barrier) failed");
    }
  }
  std::fprintf(stderr, "[URMA] transport initialized (trans_mode=%d)\n",
               static_cast<int>(jetty_cfg.jfs_cfg.trans_mode));
}
void UrmaTransport::register_main_buffer(void* ptr, size_t len, int) {
  if (!urma_ctx_) {
    throw std::runtime_error("UrmaTransport::init must run before MR setup");
  }
  main_buffer_ = ptr;
  main_buffer_len_ = len;
  urma_seg_cfg_t seg_cfg = {};
  seg_cfg.va = reinterpret_cast<uint64_t>(ptr);
  seg_cfg.len = len;
  seg_cfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
  main_tseg_ = urma_register_seg(urma_ctx_, &seg_cfg);
  if (!main_tseg_) {
    throw std::runtime_error("urma_register_seg(main) failed");
  }
  local_info_.addr = reinterpret_cast<uintptr_t>(ptr);
  local_info_.len = len;
}
void UrmaTransport::register_atomic_buffer(void* ptr, size_t len, int) {
  if (!urma_ctx_) {
    throw std::runtime_error("UrmaTransport::init must run before MR setup");
  }
  atomic_buffer_ = ptr;
  atomic_buffer_len_ = len;
  urma_seg_cfg_t seg_cfg = {};
  seg_cfg.va = reinterpret_cast<uint64_t>(ptr);
  seg_cfg.len = len;
  seg_cfg.flag.bs.access =
      URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
  atomic_tseg_ = urma_register_seg(urma_ctx_, &seg_cfg);
  if (!atomic_tseg_) {
    throw std::runtime_error("urma_register_seg(atomic) failed");
  }
  local_info_.atomic_buffer_addr = reinterpret_cast<uintptr_t>(ptr);
  local_info_.atomic_buffer_len = len;
}
LocalConnInfo UrmaTransport::local_info() const {
  LocalConnInfo info = local_info_;
  urma_rjetty_t* rjetty = nullptr;
  uint32_t rjetty_len = 0;
  check_urma(urma_get_rjetty(jetty_, &rjetty, &rjetty_len),
             "urma_get_rjetty");
  if (rjetty_len > LocalConnInfo::kMaxUrmaBlob) {
    urma_put_rjetty(rjetty);
    throw std::runtime_error("URMA rjetty exceeds exchange buffer");
  }
  std::memcpy(info.urma_rjetty, rjetty, rjetty_len);
  info.urma_rjetty_len = rjetty_len;
  urma_put_rjetty(rjetty);
  urma_seg_t* seg = nullptr;
  uint32_t seg_len = 0;
  check_urma(urma_get_seg_ctx(main_tseg_, &seg, &seg_len),
             "urma_get_seg_ctx(main)");
  if (seg_len > LocalConnInfo::kMaxUrmaBlob) {
    urma_put_seg_ctx(seg);
    throw std::runtime_error("URMA main seg exceeds exchange buffer");
  }
  std::memcpy(info.urma_main_seg, seg, seg_len);
  info.urma_main_seg_len = seg_len;
  urma_put_seg_ctx(seg);
  check_urma(urma_get_seg_ctx(atomic_tseg_, &seg, &seg_len),
             "urma_get_seg_ctx(atomic)");
  std::memcpy(info.urma_atomic_seg, seg, seg_len);
  info.urma_atomic_seg_len = seg_len;
  urma_put_seg_ctx(seg);
  check_urma(urma_get_seg_ctx(barrier_tseg_, &seg, &seg_len),
             "urma_get_seg_ctx(barrier)");
  std::memcpy(info.urma_barrier_seg, seg, seg_len);
  info.urma_barrier_seg_len = seg_len;
  urma_put_seg_ctx(seg);
  return info;
}
void UrmaTransport::connect_peer(int peer, RemoteConnInfo const& remote) {
  if (!urma_ctx_) {
    throw std::runtime_error("UrmaTransport::init must run first");
  }
  if (peer >= static_cast<size_t>(peers_.size())) {
    peers_.resize(peer + 1);
  }
  Peer& p = peers_[peer];
  if (remote.urma_rjetty_len == 0) {
    throw std::runtime_error("invalid URMA rjetty in remote info");
  }
  urma_token_t token = {};
  if (p.tjetty) {
    (void)urma_unimport_jetty(p.tjetty);
    p.tjetty = nullptr;
  }
  p.tjetty = urma_import_jetty(urma_ctx_,
                               reinterpret_cast<urma_rjetty_t*>(
                                   const_cast<uint8_t*>(remote.urma_rjetty)),
                               &token);
  if (!p.tjetty) {
    throw std::runtime_error("urma_import_jetty failed");
  }
  urma_import_seg_flag_t seg_flag = {};
  seg_flag.bs.mapping = URMA_SEG_MAPPED;
  seg_flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
  if (remote.urma_main_seg_len > 0) {
    p.main_tseg = urma_import_seg(
        urma_ctx_,
        reinterpret_cast<urma_seg_t*>(const_cast<uint8_t*>(remote.urma_main_seg)),
        &token, 0, seg_flag);
    if (!p.main_tseg) {
      throw std::runtime_error("urma_import_seg(main) failed");
    }
  }
  if (remote.urma_atomic_seg_len > 0) {
    urma_import_seg_flag_t atomic_flag = seg_flag;
    atomic_flag.bs.access |= URMA_ACCESS_ATOMIC;
    p.atomic_tseg = urma_import_seg(
        urma_ctx_,
        reinterpret_cast<urma_seg_t*>(
            const_cast<uint8_t*>(remote.urma_atomic_seg)),
        &token, 0, atomic_flag);
    if (!p.atomic_tseg) {
      throw std::runtime_error("urma_import_seg(atomic) failed");
    }
  }
  if (remote.urma_barrier_seg_len > 0) {
    urma_import_seg_flag_t barrier_flag = seg_flag;
    barrier_flag.bs.access |= URMA_ACCESS_ATOMIC;
    p.barrier_tseg = urma_import_seg(
        urma_ctx_,
        reinterpret_cast<urma_seg_t*>(
            const_cast<uint8_t*>(remote.urma_barrier_seg)),
        &token, 0, barrier_flag);
    if (!p.barrier_tseg) {
      throw std::runtime_error("urma_import_seg(barrier) failed");
    }
  }
  // Establish the transport path: bind for RC, advise for connectionless.
  urma_status_t rc;
  if (jetty_->jetty_cfg.jfs_cfg.trans_mode == URMA_TM_RC) {
    rc = urma_bind_jetty(jetty_, p.tjetty);
    if (rc != URMA_SUCCESS && rc != URMA_EEXIST) {
      throw std::runtime_error("urma_bind_jetty failed");
    }
  } else {
    rc = urma_advise_jetty(jetty_, p.tjetty);
    if (rc != URMA_SUCCESS && rc != URMA_EEXIST) {
      throw std::runtime_error("urma_advise_jetty failed");
    }
  }
}
void UrmaTransport::post_write(int dst_rank, uint64_t wr_id,
                               uint64_t local_offset, uint64_t remote_offset,
                               uint32_t bytes, bool) {
  if (dst_rank >= static_cast<size_t>(peers_.size()) || !peers_[dst_rank].tjetty) {
    throw std::runtime_error("URMA peer is not connected");
  }
  if (!main_tseg_ || !main_buffer_) {
    throw std::runtime_error("URMA main buffer MR is not registered");
  }
  if (local_offset + bytes > main_buffer_len_) {
    throw std::runtime_error("URMA write local range exceeds main buffer");
  }
  Peer& p = peers_[dst_rank];
  if (!p.main_tseg) {
    throw std::runtime_error("URMA peer main seg is not imported");
  }
  op_contexts_.push_back({});
  OpContext& op = op_contexts_.back();
  op.wr_id = wr_id;
  op.is_write = true;
  urma_sge_t src_sge = {};
  src_sge.addr =
      reinterpret_cast<uint64_t>(main_buffer_) + local_offset;
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
  urma_jfs_wr_t* bad = nullptr;
  check_urma(urma_post_jetty_send_wr(jetty_, &wr, &bad),
             "urma_post_jetty_send_wr(write)");
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
  if (dst_rank >= static_cast<size_t>(peers_.size()) || !peers_[dst_rank].tjetty) {
    throw std::runtime_error("URMA peer is not connected");
  }
  Peer& p = peers_[dst_rank];
  if (!p.atomic_tseg) {
    throw std::runtime_error("URMA peer atomic seg is not imported");
  }
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
  urma_jfs_wr_t* bad = nullptr;
  check_urma(urma_post_jetty_send_wr(jetty_, &wr, &bad),
             "urma_post_jetty_send_wr(fadd)");
}
void UrmaTransport::post_barrier_atomic_add(int dst_rank, uint64_t wr_id,
                                            size_t slot, uint64_t value) {
  if (dst_rank >= static_cast<size_t>(peers_.size()) || !peers_[dst_rank].tjetty) {
    throw std::runtime_error("URMA peer is not connected");
  }
  Peer& p = peers_[dst_rank];
  if (!p.barrier_tseg) {
    throw std::runtime_error("URMA peer barrier seg is not imported");
  }
  if (slot * sizeof(uint64_t) >=
      p.barrier_tseg->seg.len) {
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
  dst_sge.addr =
      remote_seg_addr(p.barrier_tseg, slot * sizeof(uint64_t));
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
  urma_jfs_wr_t* bad = nullptr;
  check_urma(urma_post_jetty_send_wr(jetty_, &wr, &bad),
             "urma_post_jetty_send_wr(barrier fadd)");
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
  std::vector<urma_cr_t> crs(max);
  int n = urma_poll_jfc(jfc_, max, crs.data());
  if (n < 0) {
    throw std::runtime_error("urma_poll_jfc failed");
  }
  for (int i = 0; i < n; ++i) {
    auto* op = reinterpret_cast<OpContext*>(crs[i].user_ctx);
    if (op) {
      out[i].wr_id = op->wr_id;
      out[i].is_write = op->is_write;
      out[i].is_atomic = op->is_atomic;
      if (op->operand_slot != static_cast<size_t>(-1)) {
        release_operand_slot(op->operand_slot);
      }
      for (auto it = op_contexts_.begin(); it != op_contexts_.end(); ++it) {
        if (&*it == op) {
          op_contexts_.erase(it);
          break;
        }
      }
    } else {
      out[i].wr_id = 0;
      out[i].is_write = false;
      out[i].is_atomic = false;
    }
    out[i].data = crs[i].imm_data;
    out[i].is_recv_data = false;
    out[i].status = crs[i].status == URMA_CR_SUCCESS
                        ? 0
                        : static_cast<int>(crs[i].status);
  }
  return n;
}
void UrmaTransport::destroy() {
  for (auto& p : peers_) {
    if (p.main_tseg) {
      (void)urma_unimport_seg(p.main_tseg);
      p.main_tseg = nullptr;
    }
    if (p.atomic_tseg) {
      (void)urma_unimport_seg(p.atomic_tseg);
      p.atomic_tseg = nullptr;
    }
    if (p.barrier_tseg) {
      (void)urma_unimport_seg(p.barrier_tseg);
      p.barrier_tseg = nullptr;
    }
    if (p.tjetty) {
      (void)urma_unimport_jetty(p.tjetty);
      p.tjetty = nullptr;
    }
  }
  peers_.clear();
  op_contexts_.clear();
  if (barrier_tseg_) {
    (void)urma_unregister_seg(barrier_tseg_);
    barrier_tseg_ = nullptr;
  }
  if (operand_tseg_) {
    (void)urma_unregister_seg(operand_tseg_);
    operand_tseg_ = nullptr;
  }
  if (atomic_tseg_) {
    (void)urma_unregister_seg(atomic_tseg_);
    atomic_tseg_ = nullptr;
  }
  if (main_tseg_) {
    (void)urma_unregister_seg(main_tseg_);
    main_tseg_ = nullptr;
  }
  if (jetty_) {
    (void)urma_delete_jetty(jetty_);
    jetty_ = nullptr;
  }
  if (jfr_) {
    (void)urma_delete_jfr(jfr_);
    jfr_ = nullptr;
  }
  if (jfc_) {
    (void)urma_delete_jfc(jfc_);
    jfc_ = nullptr;
  }
  if (jfce_) {
    (void)urma_delete_jfce(jfce_);
    jfce_ = nullptr;
  }
  if (urma_ctx_) {
    (void)urma_delete_context(urma_ctx_);
    urma_ctx_ = nullptr;
    std::lock_guard<std::mutex> lock(g_urma_init_mutex);
    if (g_urma_init_refcount > 0 && --g_urma_init_refcount == 0) {
      (void)urma_uninit();
    }
  }
}
#endif  // USE_URMA
