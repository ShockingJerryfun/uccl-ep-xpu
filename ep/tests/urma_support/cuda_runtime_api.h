#pragma once
#include <cstddef>
typedef int cudaError_t;
constexpr int cudaSuccess = 0;
constexpr int cudaMemoryTypeHost = 1, cudaMemoryTypeDevice = 2;
constexpr unsigned int cudaHostAllocMapped = 2;
constexpr int cudaMemcpyDeviceToHost = 2;
constexpr int cudaMemcpyHostToDevice = 1;
struct cudaPointerAttributes {
  int type;
  int device;
  void* devicePointer;
  void* hostPointer;
};
cudaError_t cudaSetDevice(int);
cudaError_t cudaPointerGetAttributes(cudaPointerAttributes*, void const*);
char const* cudaGetErrorString(cudaError_t);

cudaError_t cudaFree(void*);
cudaError_t cudaFreeHost(void*);
cudaError_t cudaMalloc(void**, std::size_t);
cudaError_t cudaHostAlloc(void**, std::size_t, unsigned int);
cudaError_t cudaMemset(void*, int, std::size_t);
cudaError_t cudaMemcpy(void*, void const*, std::size_t, int);
