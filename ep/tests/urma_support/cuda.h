#pragma once
#include <cstddef>
#include <cstdint>
using CUdeviceptr = uintptr_t;
using CUresult = int;
using CUpointer_attribute = int;
using CUmemRangeHandleType = int;
constexpr int CUDA_SUCCESS = 0;
constexpr int CU_POINTER_ATTRIBUTE_SYNC_MEMOPS = 6;
constexpr int CU_MEM_RANGE_HANDLE_TYPE_DMA_BUF_FD = 1;
extern "C" CUresult cuPointerSetAttribute(void const*, CUpointer_attribute,
                                          CUdeviceptr);
extern "C" CUresult cuMemGetAddressRange(CUdeviceptr*, size_t*, CUdeviceptr);
