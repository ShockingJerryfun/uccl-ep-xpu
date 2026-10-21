#pragma once

#include "gpu_platform.hpp"

#include <cstddef>

namespace uccl::gpu_p2p {

enum class Vendor { Nvidia, MetaX };
enum class MemoryKind { Device, HostMapped };

Vendor vendor();
char const* vendor_name();
std::size_t registration_alignment();
bool atomic_buffer_is_gpu_memory();

void set_device(int device);
void* allocate_device(std::size_t bytes, int device);
void* allocate_atomic_buffer(std::size_t bytes, int device);
void zero_memory(void* ptr, std::size_t bytes, MemoryKind kind, int device);
void copy_device_to_host(void* host, void const* device_ptr, std::size_t bytes,
                         int device);
void copy_host_to_device(void* device_ptr, void const* host, std::size_t bytes,
                         int device);
void prepare_main_buffer(void* ptr, std::size_t bytes, int device);
void prepare_atomic_buffer(void* ptr, std::size_t bytes, int device);
void release_memory(void* ptr, MemoryKind kind, int device);

}  // namespace uccl::gpu_p2p
