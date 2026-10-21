#pragma once
#include <cstdint>
using CUdeviceptr = uintptr_t;
using CUresult = int;
using CUpointer_attribute = int;
constexpr int CUDA_SUCCESS = 0;
constexpr int CU_POINTER_ATTRIBUTE_SYNC_MEMOPS = 6;
extern "C" CUresult cuPointerSetAttribute(void const*, CUpointer_attribute,
                                          CUdeviceptr);
