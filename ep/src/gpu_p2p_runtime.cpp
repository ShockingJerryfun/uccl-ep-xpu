#include "gpu_p2p_runtime.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef USE_METAX
#include <dlfcn.h>
#else
#include <cuda.h>
#include <cuda_runtime_api.h>
#endif

namespace uccl::gpu_p2p {
namespace {

#ifdef USE_METAX
constexpr int kMcSuccess = 0;
constexpr int kMcMemcpyHostToDevice = 1;
constexpr int kMcMemcpyDeviceToHost = 2;

struct MetaXApi {
  using SetDevice = int (*)(int);
  using GetDeviceCount = int (*)(int*);
  using Malloc = int (*)(void**, std::size_t);
  using Free = int (*)(void*);
  using Memcpy = int (*)(void*, void const*, std::size_t, int);

  void* library = nullptr;
  SetDevice set_device = nullptr;
  GetDeviceCount get_device_count = nullptr;
  Malloc malloc = nullptr;
  Free free = nullptr;
  Memcpy memcpy = nullptr;
};

std::once_flag g_metax_once;
MetaXApi g_metax;
std::mutex g_allocations_mutex;
std::map<void*, int> g_allocations;

void* try_dlopen(std::string const& path) {
  if (path.empty()) return nullptr;
  return dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
}

void load_metax_runtime() {
  std::vector<std::string> candidates;
  if (char const* path = std::getenv("UCCL_METAX_RUNTIME_LIB"))
    candidates.emplace_back(path);
  if (char const* path = std::getenv("PERFTEST_METAX_LIB"))
    candidates.emplace_back(path);
  if (char const* home = std::getenv("MACA_PATH")) {
    for (char const* dir : {"lib", "lib64"}) {
      for (char const* name : {"libmcruntime.so", "libmcruntime.so.1",
                               "libmacart.so", "libmaca.so"})
        candidates.emplace_back(std::string(home) + "/" + dir + "/" + name);
    }
  }
  for (char const* root : {"/opt/maca", "/usr/local/maca"}) {
    for (char const* dir : {"lib", "lib64"})
      for (char const* name : {"libmcruntime.so", "libmcruntime.so.1",
                               "libmacart.so", "libmaca.so"})
        candidates.emplace_back(std::string(root) + "/" + dir + "/" + name);
  }
  for (char const* name : {"libmcruntime.so", "libmcruntime.so.1",
                           "libmcruntime.dylib", "libmacart.so",
                           "libmaca.so"})
    candidates.emplace_back(name);

  for (auto const& candidate : candidates) {
    g_metax.library = try_dlopen(candidate);
    if (g_metax.library) break;
  }
  if (!g_metax.library) {
    char const* error = dlerror();
    throw std::runtime_error(
        std::string("MetaX MACA runtime not found; set MACA_PATH or "
                    "UCCL_METAX_RUNTIME_LIB") +
        (error ? std::string(": ") + error : std::string()));
  }

  auto symbol = [](char const* name) -> void* {
    void* value = dlsym(g_metax.library, name);
    if (!value)
      throw std::runtime_error(std::string("MetaX runtime missing symbol ") +
                               name);
    return value;
  };
  g_metax.set_device = reinterpret_cast<MetaXApi::SetDevice>(symbol("mcSetDevice"));
  g_metax.get_device_count = reinterpret_cast<MetaXApi::GetDeviceCount>(
      dlsym(g_metax.library, "mcGetDeviceCount"));
  g_metax.malloc = reinterpret_cast<MetaXApi::Malloc>(symbol("mcMalloc"));
  g_metax.free = reinterpret_cast<MetaXApi::Free>(symbol("mcFree"));
  g_metax.memcpy = reinterpret_cast<MetaXApi::Memcpy>(symbol("mcMemcpy"));
}

MetaXApi& metax() {
  std::call_once(g_metax_once, load_metax_runtime);
  return g_metax;
}

void check_mc(int status, char const* operation) {
  if (status != kMcSuccess)
    throw std::runtime_error(std::string(operation) +
                             " failed: mcError=" + std::to_string(status));
}

void remember_allocation(void* ptr, int device) {
  std::lock_guard<std::mutex> lock(g_allocations_mutex);
  g_allocations[ptr] = device;
}

void validate_known_device(void* ptr, int device) {
  std::lock_guard<std::mutex> lock(g_allocations_mutex);
  auto const it = g_allocations.find(ptr);
  // External MACA allocations are accepted because the public runtime ABI used
  // by the working perftest has no pointer-attribute query. The UDMA MetaX pin
  // callback remains the authoritative validation during urma_register_seg().
  if (it != g_allocations.end() && it->second != device)
    throw std::invalid_argument("MetaX allocation belongs to another device");
}
#else
void check_cuda(cudaError_t status, char const* operation) {
  if (status != cudaSuccess)
    throw std::runtime_error(std::string(operation) + ": " +
                             cudaGetErrorString(status));
}
#endif

}  // namespace

Vendor vendor() {
#ifdef USE_METAX
  return Vendor::MetaX;
#else
  return Vendor::Nvidia;
#endif
}

char const* vendor_name() {
  return vendor() == Vendor::MetaX ? "MetaX" : "NVIDIA";
}

std::size_t registration_alignment() {
#ifdef USE_METAX
  return 4 * 1024;
#else
  return 64 * 1024;
#endif
}

bool atomic_buffer_is_gpu_memory() {
#ifdef USE_METAX
  return true;
#else
  return false;
#endif
}

void set_device(int device) {
  if (device < 0) throw std::invalid_argument("invalid GPU device ordinal");
#ifdef USE_METAX
  auto& api = metax();
  if (api.get_device_count) {
    int count = 0;
    check_mc(api.get_device_count(&count), "mcGetDeviceCount");
    if (device >= count)
      throw std::out_of_range("MetaX device ordinal is out of range");
  }
  check_mc(api.set_device(device), "mcSetDevice");
#else
  check_cuda(cudaSetDevice(device), "cudaSetDevice");
#endif
}

void* allocate_device(std::size_t bytes, int device) {
  if (bytes == 0) throw std::invalid_argument("zero-sized GPU allocation");
  set_device(device);
  void* ptr = nullptr;
#ifdef USE_METAX
  check_mc(metax().malloc(&ptr, bytes), "mcMalloc");
  if (!ptr) throw std::runtime_error("mcMalloc returned a null pointer");
  remember_allocation(ptr, device);
#else
  check_cuda(cudaMalloc(&ptr, bytes), "cudaMalloc");
#endif
  return ptr;
}

void* allocate_atomic_buffer(std::size_t bytes, int device) {
#ifdef USE_METAX
  std::size_t const alignment = registration_alignment();
  if (bytes > static_cast<std::size_t>(-1) - (alignment - 1))
    throw std::overflow_error("MetaX atomic allocation size overflow");
  std::size_t const aligned_bytes =
      (bytes + alignment - 1) & ~(alignment - 1);
  return allocate_device(aligned_bytes, device);
#else
  set_device(device);
  void* ptr = nullptr;
  check_cuda(cudaHostAlloc(&ptr, bytes, cudaHostAllocMapped), "cudaHostAlloc");
  return ptr;
#endif
}

void zero_memory(void* ptr, std::size_t bytes, MemoryKind kind, int device) {
  if (!ptr || bytes == 0) return;
  if (kind == MemoryKind::HostMapped) {
    std::memset(ptr, 0, bytes);
    return;
  }
  set_device(device);
#ifdef USE_METAX
  constexpr std::size_t kChunkBytes = 1024 * 1024;
  std::vector<unsigned char> zeros(std::min(bytes, kChunkBytes), 0);
  auto* out = static_cast<unsigned char*>(ptr);
  for (std::size_t offset = 0; offset < bytes; offset += zeros.size()) {
    std::size_t const count = std::min(zeros.size(), bytes - offset);
    check_mc(metax().memcpy(out + offset, zeros.data(), count,
                            kMcMemcpyHostToDevice),
             "mcMemcpy(H2D zero)");
  }
#else
  check_cuda(cudaMemset(ptr, 0, bytes), "cudaMemset");
#endif
}

void copy_device_to_host(void* host, void const* device_ptr, std::size_t bytes,
                         int device) {
  if (!host || !device_ptr) throw std::invalid_argument("null copy pointer");
  set_device(device);
#ifdef USE_METAX
  check_mc(metax().memcpy(host, device_ptr, bytes, kMcMemcpyDeviceToHost),
           "mcMemcpy(D2H)");
#else
  check_cuda(cudaMemcpy(host, device_ptr, bytes, cudaMemcpyDeviceToHost),
             "cudaMemcpy(D2H)");
#endif
}

void copy_host_to_device(void* device_ptr, void const* host, std::size_t bytes,
                         int device) {
  if (!host || !device_ptr) throw std::invalid_argument("null copy pointer");
  set_device(device);
#ifdef USE_METAX
  check_mc(metax().memcpy(device_ptr, host, bytes, kMcMemcpyHostToDevice),
           "mcMemcpy(H2D)");
#else
  check_cuda(cudaMemcpy(device_ptr, host, bytes, cudaMemcpyHostToDevice),
             "cudaMemcpy(H2D)");
#endif
}

void prepare_main_buffer(void* ptr, std::size_t bytes, int device) {
  if (!ptr || bytes == 0) throw std::invalid_argument("invalid main GPU buffer");
  set_device(device);
#ifdef USE_METAX
  validate_known_device(ptr, device);
#else
  cudaPointerAttributes attr{};
  check_cuda(cudaPointerGetAttributes(&attr, ptr),
             "cudaPointerGetAttributes(main)");
  if (attr.type != cudaMemoryTypeDevice || attr.device != device)
    throw std::invalid_argument(
        "URMA main buffer must be NVIDIA device memory on the selected CUDA "
        "ordinal");
  unsigned int sync_memops = 1;
  if (cuPointerSetAttribute(&sync_memops, CU_POINTER_ATTRIBUTE_SYNC_MEMOPS,
                            reinterpret_cast<CUdeviceptr>(ptr)) != CUDA_SUCCESS)
    throw std::runtime_error("cuPointerSetAttribute(SYNC_MEMOPS) failed");
#endif
}

void prepare_atomic_buffer(void* ptr, std::size_t bytes, int device) {
  if (!ptr || bytes == 0)
    throw std::invalid_argument("invalid atomic control buffer");
  set_device(device);
#ifdef USE_METAX
  validate_known_device(ptr, device);
#else
  cudaPointerAttributes attr{};
  check_cuda(cudaPointerGetAttributes(&attr, ptr),
             "cudaPointerGetAttributes(control)");
  if (attr.type != cudaMemoryTypeHost || attr.devicePointer != ptr)
    throw std::invalid_argument(
        "URMA control buffer requires CUDA mapped host memory with identical "
        "UVA");
#endif
}

void release_memory(void* ptr, MemoryKind kind, int device) {
  if (!ptr) return;
#ifdef USE_METAX
  if (kind != MemoryKind::Device)
    throw std::invalid_argument("MetaX URMA owns device allocations only");
  set_device(device);
  check_mc(metax().free(ptr), "mcFree");
  std::lock_guard<std::mutex> lock(g_allocations_mutex);
  g_allocations.erase(ptr);
#else
  if (kind == MemoryKind::Device) set_device(device);
  check_cuda(kind == MemoryKind::HostMapped ? cudaFreeHost(ptr) : cudaFree(ptr),
             kind == MemoryKind::HostMapped ? "cudaFreeHost" : "cudaFree");
#endif
}

}  // namespace uccl::gpu_p2p
