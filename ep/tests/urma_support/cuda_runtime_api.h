#pragma once
#include <cstddef>
typedef int cudaError_t;
constexpr int cudaSuccess = 0;
constexpr int cudaMemoryTypeHost = 1, cudaMemoryTypeDevice = 2;
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
