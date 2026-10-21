// Hardware-independent integration tests: real UMDK types + production
// transport.
#include "urma_transport.hpp"
#include <cassert>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <set>
#include <stdexcept>
#include <cuda.h>
#include <cuda_runtime_api.h>

namespace {
int resources = 0, init_count = 0, gpu_regs = 0, posts = 0;
bool fail_context = false, fail_post = false, fail_register = false;
bool fail_completion = false;
int selected_device = -1;
std::set<void const*> gpu_ptrs, host_ptrs, sync_ptrs;
std::map<urma_jfc_t*, std::deque<urma_cr_t>> completions;
urma_device_t device{};
template <class T>
T* create() {
  ++resources;
  return new T{};
}
template <class T>
int release(T* x) {
  if (x) {
    --resources;
    delete x;
  }
  return 0;
}
template <class F>
void rejects(F f) {
  bool threw = false;
  try {
    f();
  } catch (std::exception const&) {
    threw = true;
  }
  assert(threw);
}
}  // namespace
cudaError_t cudaSetDevice(int d) {
  selected_device = d;
  return 0;
}
cudaError_t cudaPointerGetAttributes(cudaPointerAttributes* a, void const* p) {
  if (gpu_ptrs.count(p)) {
    *a = {cudaMemoryTypeDevice, 2, const_cast<void*>(p), nullptr};
    return 0;
  }
  if (host_ptrs.count(p)) {
    *a = {cudaMemoryTypeHost, -1, const_cast<void*>(p), const_cast<void*>(p)};
    return 0;
  }
  return 1;
}
int gpu_frees = 0, host_frees = 0;
cudaError_t cudaFree(void*) {
  ++gpu_frees;
  assert(gpu_regs == 0);
  return 0;
}
cudaError_t cudaFreeHost(void*) {
  ++host_frees;
  return 0;
}
char const* cudaGetErrorString(cudaError_t) { return "test CUDA error"; }
extern "C" {
CUresult cuPointerSetAttribute(void const* flag, CUpointer_attribute attr,
                               CUdeviceptr ptr) {
  assert(attr == CU_POINTER_ATTRIBUTE_SYNC_MEMOPS &&
         *static_cast<unsigned const*>(flag) == 1);
  sync_ptrs.insert(reinterpret_cast<void*>(ptr));
  return CUDA_SUCCESS;
}
urma_status_t urma_init(urma_init_attr_t*) {
  ++init_count;
  return 0;
}
urma_status_t urma_uninit() {
  --init_count;
  return 0;
}
urma_device_t** urma_get_device_list(int* n) {
  static urma_device_t* list[] = {&device};
  std::strcpy(device.name, "udma_test");
  device.type = URMA_TRANSPORT_UB;
  *n = 1;
  return list;
}
void urma_free_device_list(urma_device_t**) {}
urma_eid_info_t* urma_get_eid_list(urma_device_t*, uint32_t* n) {
  static urma_eid_info_t e{};
  *n = 1;
  return &e;
}
void urma_free_eid_list(urma_eid_info_t*) {}
urma_context_t* urma_create_context(urma_device_t* d, uint32_t) {
  if (fail_context) return nullptr;
  auto* c = create<urma_context_t>();
  c->dev = d;
  return c;
}
urma_status_t urma_delete_context(urma_context_t* x) { return release(x); }
urma_jfce_t* urma_create_jfce(urma_context_t*) { return create<urma_jfce_t>(); }
urma_status_t urma_delete_jfce(urma_jfce_t* x) { return release(x); }
urma_jfc_t* urma_create_jfc(urma_context_t*, urma_jfc_cfg_t*) {
  return create<urma_jfc_t>();
}
urma_status_t urma_delete_jfc(urma_jfc_t* x) {
  completions.erase(x);
  return release(x);
}
urma_jfr_t* urma_create_jfr(urma_context_t*, urma_jfr_cfg_t*) {
  return create<urma_jfr_t>();
}
urma_status_t urma_delete_jfr(urma_jfr_t* x) { return release(x); }
urma_jetty_t* urma_create_jetty(urma_context_t*, urma_jetty_cfg_t* cfg) {
  auto* x = create<urma_jetty_t>();
  x->jetty_cfg = *cfg;
  x->jetty_id.id = 42;
  return x;
}
urma_status_t urma_delete_jetty(urma_jetty_t* x) { return release(x); }
urma_token_id_t* urma_alloc_token_id(urma_context_t* c) {
  auto* t = create<urma_token_id_t>();
  t->urma_ctx = c;
  t->token_id = 123;
  return t;
}
urma_status_t urma_free_token_id(urma_token_id_t* t) { return release(t); }
urma_target_seg_t* urma_register_seg(urma_context_t* c, urma_seg_cfg_t* cfg) {
  if (fail_register) return nullptr;
  if (!cfg->token_id || !cfg->flag.bs.token_id_valid) return nullptr;
  bool gpu = gpu_ptrs.count(reinterpret_cast<void*>(cfg->va));
  if (gpu != bool(cfg->is_gpu_seg) || cfg->flag.bs.non_pin) return nullptr;
  if (gpu && !sync_ptrs.count(reinterpret_cast<void*>(cfg->va))) return nullptr;
  if (gpu) ++gpu_regs;
  auto* s = create<urma_target_seg_t>();
  s->urma_ctx = c;
  s->token_id = cfg->token_id;
  s->seg.ubva.va = cfg->va;
  s->seg.len = cfg->len;
  s->seg.token_id = cfg->token_id->token_id;
  s->seg.attr.bs.access = cfg->flag.bs.access;
  return s;
}
urma_status_t urma_unregister_seg(urma_target_seg_t* s) {
  if (gpu_ptrs.count(reinterpret_cast<void*>(s->seg.ubva.va))) --gpu_regs;
  return release(s);
}
urma_target_seg_t* urma_import_seg(urma_context_t*, urma_seg_t* seg,
                                   urma_token_t*, uint64_t,
                                   urma_import_seg_flag_t flag) {
  assert(flag.bs.mapping == URMA_SEG_NOMAP);
  auto* x = create<urma_target_seg_t>();
  x->seg = *seg;
  return x;
}
urma_status_t urma_unimport_seg(urma_target_seg_t* s) { return release(s); }
urma_target_jetty_t* urma_import_jetty(urma_context_t*, urma_rjetty_t* r,
                                       urma_token_t*) {
  assert(r->trans_mode == URMA_TM_RC && r->type == URMA_JETTY &&
         r->tp_type == URMA_RTP);
  auto* t = create<urma_target_jetty_t>();
  t->id = r->jetty_id;
  return t;
}
urma_status_t urma_unimport_jetty(urma_target_jetty_t* t) { return release(t); }
urma_status_t urma_bind_jetty(urma_jetty_t* j, urma_target_jetty_t* t) {
  j->remote_jetty = t;
  return 0;
}
urma_status_t urma_unbind_jetty(urma_jetty_t* j) {
  j->remote_jetty = nullptr;
  return 0;
}
urma_status_t urma_post_jetty_send_wr(urma_jetty_t* j, urma_jfs_wr_t* w,
                                      urma_jfs_wr_t**) {
  if (fail_post) return URMA_EINVAL;
  ++posts;
  assert(w->flag.bs.place_order == URMA_STRONG_ORDER);
  if (w->opcode == URMA_OPC_WRITE) {
    std::memcpy(reinterpret_cast<void*>(w->rw.dst.sge->addr),
                reinterpret_cast<void*>(w->rw.src.sge->addr),
                w->rw.src.sge->len);
  } else {
    assert(w->opcode == URMA_OPC_FADD);
    auto* dst = reinterpret_cast<uint64_t*>(w->faa.dst->addr);
    *reinterpret_cast<uint64_t*>(w->faa.src->addr) = *dst;
    *dst += w->faa.operand;
  }
  urma_cr_t c{};
  c.status = fail_completion ? URMA_CR_LOC_LEN_ERR : URMA_CR_SUCCESS;
  c.user_ctx = w->user_ctx;
  completions[j->jetty_cfg.jfs_cfg.jfc].push_back(c);
  return 0;
}
int urma_poll_jfc(urma_jfc_t* j, int max, urma_cr_t* out) {
  int n = 0;
  auto& q = completions[j];
  while (n < max && !q.empty()) {
    out[n++] = q.front();
    q.pop_front();
  }
  return n;
}
}
int main() {
  ProxyCtx ctx;
  // Failure before a context exists must release global initialization
  // ownership.
  fail_context = true;
  {
    UrmaTransport t;
    rejects([&] { t.init(ctx); });
  }
  fail_context = false;
  assert(init_count == 0 && resources == 0);
  alignas(65536) uint64_t a[32]{}, b[32]{};
  alignas(4096) uint64_t ac[8]{}, bc[8]{};
  gpu_ptrs = {a, b};
  host_ptrs = {ac, bc};
  {
    UrmaTransport left, right;
    left.init(ctx);
    right.init(ctx);
    rejects([&] { left.register_main_buffer(ac, sizeof(ac), 2); });
    rejects([&] { left.register_main_buffer(a, sizeof(a), 0); });
    left.register_main_buffer(a, sizeof(a), 2);
    right.register_main_buffer(b, sizeof(b), 2);
    assert(selected_device == 2 && gpu_regs == 2);
    {
      UrmaTransport duplicate;
      duplicate.init(ctx);
      duplicate.register_main_buffer(a, sizeof(a), 2);
      assert(gpu_regs == 2);  // same device/context/range must pin exactly once
      duplicate.destroy();
      assert(gpu_regs == 2);  // first registration is still live
    }
    {
      UrmaTransport overlapping;
      overlapping.init(ctx);
      gpu_ptrs.insert(a + 1);
      rejects([&] { overlapping.register_main_buffer(a + 1, 8, 2); });
      gpu_ptrs.erase(a + 1);
    }
    left.register_atomic_buffer(ac, sizeof(ac), 2);
    right.register_atomic_buffer(bc, sizeof(bc), 2);
    auto ri = right.local_info(), li = left.local_info();
    auto broken = ri;
    broken.urma_main_seg_len = 1;
    rejects([&] { left.connect_peer(1, broken); });
    broken = ri;
    ++broken.urma_wire_version;
    rejects([&] { left.connect_peer(1, broken); });
    left.connect_peer(1, ri);
    right.connect_peer(0, li);
    a[0] = 0x1122334455667788ULL;
    left.post_write(1, 7, 0, 8, 8, false);
    left.post_atomic_add(1, 8, 0, 3, true);
    assert(b[1] == a[0] && bc[0] == 3);
    TransportCompletion cs[8];
    assert(left.poll(cs, 8) == 2);
    assert(cs[0].wr_id == 7 && cs[1].wr_id == 8);
    int n = posts;
    rejects([&] { left.post_write(1, 9, UINT64_MAX, 0, 8, false); });
    rejects([&] { left.post_write(1, 9, 0, sizeof(b), 8, false); });
    rejects([&] { left.post_atomic_add(1, 9, 1, 1, true); });
    assert(posts == n);
    fail_post = true;
    for (int i = 0; i < 32; ++i)
      rejects([&] { left.post_atomic_add(1, 10, 0, 1, true); });
    fail_post = false;
    left.post_atomic_add(1, 11, 0, -1, true);
    assert(left.poll(cs, 8) == 1 && bc[0] == 2);
    left.post_barrier_atomic_add(1, 12, 0, 1);
    assert(left.poll(cs, 8) == 1 && right.load_barrier_word(0) == 1);
    // destroy drains outstanding work and is idempotent.
    left.post_write(1, 13, 0, 16, 8, false);
    left.destroy();
    left.destroy();
    fail_completion = true;
    right.post_atomic_add(0, 14, 0, 1, true);
    assert(right.poll(cs, 8) == 1 && cs[0].status != 0);
    rejects([&] { right.post_atomic_add(0, 15, 0, 1, true); });
    fail_completion = false;
    right.destroy();
  }
  assert(resources == 0 && init_count == 0 && gpu_regs == 0);
  // An owning proxy may stop before another proxy sharing its allocation.
  {
    auto main_owner = UrmaTransport::acquire_buffer_ownership(a, false, 2);
    auto control_owner = UrmaTransport::acquire_buffer_ownership(ac, true, 2);
    UrmaTransport first, last;
    first.init(ctx);
    last.init(ctx);
    first.register_main_buffer(a, sizeof(a), 2);
    last.register_main_buffer(a, sizeof(a), 2);
    first.register_atomic_buffer(ac, sizeof(ac), 2);
    last.register_atomic_buffer(ac, sizeof(ac), 2);
    first.destroy();
    main_owner.reset();
    control_owner.reset();
    assert(gpu_frees == 0 && host_frees == 0 && gpu_regs == 1);
    last.destroy();
    assert(gpu_frees == 1 && host_frees == 1 && gpu_regs == 0);
  }
  // DLPack owns the same allocation; proxy teardown must not free it twice.
  {
    auto tensor_owner = UrmaTransport::acquire_buffer_ownership(a, false, 2);
    auto proxy_owner = UrmaTransport::acquire_buffer_ownership(a, false, 2);
    UrmaTransport t;
    t.init(ctx);
    t.register_main_buffer(a, sizeof(a), 2);
    t.destroy();
    proxy_owner.reset();
    assert(gpu_frees == 1);
    tensor_owner.reset();
    assert(gpu_frees == 2);
  }
  fail_register = true;
  {
    UrmaTransport t;
    rejects([&] { t.init(ctx); });
  }
  fail_register = false;
  assert(resources == 0 && init_count == 0);
  std::cout
      << "URMA transport tests passed (simulated hardware, real SDK headers)\n";
}
