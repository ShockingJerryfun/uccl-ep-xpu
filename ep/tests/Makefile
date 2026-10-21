# Paths
CUDA_HOME := /usr/local/cuda
EFA_HOME := /opt/amazon/efa

# Tools
NVCC := $(CUDA_HOME)/bin/nvcc
CXX := g++

# Flags
INCLUDES := -I../include -I../../include -I$(CUDA_HOME)/include -I$(EFA_HOME)/include
LDFLAGS := -L$(CUDA_HOME)/lib64 -L$(EFA_HOME)/lib -lcuda -lcudart -libverbs -lpthread
CXXFLAGS := -O3 -g -std=c++17 $(INCLUDES)
CUDAFLAGS := -arch=sm_90  # Use sm_80 for A100
HOST_TEST_INCLUDES ?= -Iinclude -I../include
HOST_TEST_CXXFLAGS := -std=c++17 -Wall -Wextra -Werror $(HOST_TEST_INCLUDES)

# Files
COMMON_SRC := ../src/common.cpp
COMMON_OBJ := ../src/common.o

# Binaries
CUDA_BIN := pcie_bench gpu_to_cpu_bench batched_gpu_to_cpu_bench
HOST_TEST_BIN := rdma_addressing_test rdma_registration_test metax_gpu_rt_config_test
INTRANODE_TEST_BIN := intranode_combine_test

all: $(CUDA_BIN)

# Object file
$(COMMON_OBJ): $(COMMON_SRC) ../include/common.hpp
	$(CXX) -c $(CXXFLAGS) $(COMMON_SRC) -o $@

# Test benchmarks
pcie_bench: pcie_bench.cu $(COMMON_OBJ)
	$(NVCC) $(CUDAFLAGS) $(CXXFLAGS) pcie_bench.cu $(COMMON_OBJ) -o $@ $(LDFLAGS)

gpu_to_cpu_bench: gpu_to_cpu_bench.cu $(COMMON_OBJ)
	$(NVCC) $(CUDAFLAGS) $(CXXFLAGS) gpu_to_cpu_bench.cu $(COMMON_OBJ) -o $@ $(LDFLAGS)

batched_gpu_to_cpu_bench: batched_gpu_to_cpu_bench.cu $(COMMON_OBJ)
	$(NVCC) $(CUDAFLAGS) $(CXXFLAGS) batched_gpu_to_cpu_bench.cu $(COMMON_OBJ) -o $@ $(LDFLAGS)

$(INTRANODE_TEST_BIN): intranode_combine_test.cu ../src/intranode.cu $(wildcard ../include/*.cuh) $(wildcard ../include/*.hpp) ../../include/util/gpu_rt.h
	$(NVCC) $(CUDAFLAGS) $(CXXFLAGS) --expt-relaxed-constexpr intranode_combine_test.cu ../src/intranode.cu -o $@ $(LDFLAGS)

rdma_addressing_test: rdma_addressing_test.cpp ../include/rdma_addressing.hpp
	$(CXX) $(HOST_TEST_CXXFLAGS) $< -o $@

rdma_registration_test: rdma_registration_test.cpp ../include/rdma_registration.hpp
	$(CXX) $(HOST_TEST_CXXFLAGS) $< -o $@

metax_gpu_rt_config_test: metax_gpu_rt_config_test.cpp ../../include/util/gpu_rt.h
	$(CXX) $(HOST_TEST_CXXFLAGS) -DUSE_METAX -Iurma_support -I../../include $< -o $@

test: $(HOST_TEST_BIN)
	./rdma_addressing_test
	./rdma_registration_test
	./metax_gpu_rt_config_test

clean:
	rm -f $(CUDA_BIN) $(HOST_TEST_BIN) $(INTRANODE_TEST_BIN) urma_gpu_smoke urma_metax_gpu_smoke $(COMMON_OBJ)

.PHONY: all clean test

# Target-machine test: real CUDA, real UMDK, production transport, no test shim.
UMDK_HOME ?= /usr/local/umdk
URMA_INCLUDE_DIR ?= $(UMDK_HOME)/include
URMA_LIBRARY_DIR ?= $(UMDK_HOME)/lib64
URMA_SMOKE_FLAGS := -std=c++17 -DUSE_URMA -I../include -I../../include -I$(URMA_INCLUDE_DIR) -I$(CUDA_HOME)/include
urma_gpu_smoke: urma_gpu_smoke.cu ../src/urma_transport.cpp ../src/gpu_p2p_runtime.cpp ../include/urma_transport.hpp ../include/gpu_p2p_runtime.hpp
	$(NVCC) $(CUDAFLAGS) $(URMA_SMOKE_FLAGS) -Xcompiler=-pthread urma_gpu_smoke.cu ../src/urma_transport.cpp ../src/gpu_p2p_runtime.cpp -o $@ -L$(URMA_LIBRARY_DIR) -L$(CUDA_HOME)/lib64 -lurma -lcuda -lcudart -libverbs

# Target-machine test: MetaX MACA runtime is loaded dynamically.
URMA_METAX_SMOKE_FLAGS := -std=c++17 -pthread -DUSE_URMA -DUSE_METAX -I../include -I../../include -I$(URMA_INCLUDE_DIR)
urma_metax_gpu_smoke: urma_metax_gpu_smoke.cpp ../src/urma_transport.cpp ../src/gpu_p2p_runtime.cpp ../include/urma_transport.hpp ../include/gpu_p2p_runtime.hpp
	$(CXX) $(URMA_METAX_SMOKE_FLAGS) urma_metax_gpu_smoke.cpp ../src/urma_transport.cpp ../src/gpu_p2p_runtime.cpp -o $@ -L$(URMA_LIBRARY_DIR) -lurma -libverbs -ldl
