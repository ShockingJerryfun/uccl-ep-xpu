#include "util/gpu_rt.h"

#include <cassert>
#include <string>

int main() {
  assert(std::string(GPU_DRIVER_LIB_NAME) == "libmcruntime.so");
  assert(std::string(GPU_DRIVER_GET_HANDLE_FOR_ADDRESS_RANGE_NAME) ==
         "mcMemGetHandleForAddressRange");
  assert(GPU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD ==
         CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD);
  assert(gpuMemoryTypeHost == cudaMemoryTypeHost);
}
