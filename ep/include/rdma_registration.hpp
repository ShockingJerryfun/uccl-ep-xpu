#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace uccl::rdma {

// USE_DMABUF controls whether the binary contains DMA-BUF support. This mode
// controls which registration path a running process is allowed to use.
enum class GpuRegistrationMode { Auto, PeerMemory, DmaBuf };

inline GpuRegistrationMode parse_gpu_registration_mode(char const* value) {
  if (!value || value[0] == '\0' || std::string(value) == "auto")
    return GpuRegistrationMode::Auto;
  if (std::string(value) == "peermem")
    return GpuRegistrationMode::PeerMemory;
  if (std::string(value) == "dmabuf") return GpuRegistrationMode::DmaBuf;
  throw std::invalid_argument(
      "UCCL_EP_VERBS_REG must be one of auto, peermem, or dmabuf");
}

inline char const* gpu_registration_mode_name(GpuRegistrationMode mode) {
  switch (mode) {
    case GpuRegistrationMode::Auto:
      return "auto";
    case GpuRegistrationMode::PeerMemory:
      return "peermem";
    case GpuRegistrationMode::DmaBuf:
      return "dmabuf";
  }
  return "invalid";
}

struct DmabufExportRange {
  uintptr_t export_base;
  std::size_t export_size;
  std::size_t fd_offset;
};

inline DmabufExportRange make_dmabuf_export_range(
    uintptr_t request_addr, std::size_t request_size, uintptr_t allocation_base,
    std::size_t allocation_size, std::size_t page_size) {
  if (request_size == 0 || allocation_size == 0)
    throw std::invalid_argument("DMA-BUF range must be non-empty");
  if (page_size == 0 || (page_size & (page_size - 1)) != 0)
    throw std::invalid_argument("DMA-BUF page size must be a power of two");
  if (allocation_base > UINTPTR_MAX - allocation_size ||
      request_addr > UINTPTR_MAX - request_size)
    throw std::overflow_error("DMA-BUF address range overflow");

  uintptr_t const allocation_end = allocation_base + allocation_size;
  uintptr_t const request_end = request_addr + request_size;
  if (request_addr < allocation_base || request_end > allocation_end)
    throw std::out_of_range("DMA-BUF request is outside the GPU allocation");

  uintptr_t export_base = request_addr & ~(uintptr_t(page_size - 1));
  if (export_base < allocation_base) export_base = allocation_base;

  if (request_end > UINTPTR_MAX - (page_size - 1))
    throw std::overflow_error("DMA-BUF aligned range overflow");
  uintptr_t export_end =
      (request_end + page_size - 1) & ~(uintptr_t(page_size - 1));
  if (export_end > allocation_end) export_end = allocation_end;
  if (export_base > request_addr || export_end < request_end ||
      export_end <= export_base)
    throw std::out_of_range("cannot align DMA-BUF range inside allocation");

  std::size_t const fd_offset = request_addr - export_base;
  if (fd_offset % page_size != request_addr % page_size)
    throw std::invalid_argument(
        "DMA-BUF offset and IOVA have incompatible page offsets");
  return {export_base, static_cast<std::size_t>(export_end - export_base),
          fd_offset};
}

}  // namespace uccl::rdma
