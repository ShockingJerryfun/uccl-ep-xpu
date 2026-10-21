#include "ep_utils.cuh"
#include <vector>

constexpr int kWarps = 8;
constexpr int kThreads = kWarps * WARP_SIZE;

__global__ void test_primitives(int* results) {
  int const tid = threadIdx.x;
  int const lane = get_lane_id();
  int const group = (tid / WARP_SIZE) % 2;
  int errors = 0;
  __shared__ int values[kThreads];
#if defined(__HIP_PLATFORM_AMD__) || defined(__HIPCC__)
  if (tid < MAX_NUM_BARRIERS) amd::barrier_init(tid);
#elif defined(__MC_PLATFORM_MXCC__) || defined(__MCC__) || defined(__MXCC__)
  if (tid < MAX_NUM_BARRIERS) {
    metax::barrier_count[tid] = 0;
    metax::barrier_phase[tid] = 0;
  }
#endif
  __syncthreads();

  errors += lane != tid % WARP_SIZE;
  errors += warp_reduce_sum(lane) != WARP_SIZE * (WARP_SIZE - 1) / 2;
  errors += warp_reduce_max<16>(lane) != (lane / 16) * 16 + 15;
  int value = lane;
  errors += broadcast(value, WARP_SIZE - 1) != WARP_SIZE - 1;
  errors += __shfl_xor_sync(WARP_MASK, value, WARP_SIZE / 2) !=
            (lane ^ (WARP_SIZE / 2));

  for (int iteration = 0; iteration < 1024; ++iteration) {
    values[tid] = iteration * kThreads + tid;
    sync_barrier(group + 2, kThreads / 2);
    if (tid / WARP_SIZE < 2) {
      for (int warp = group; warp < kWarps; warp += 2)
        errors += values[warp * WARP_SIZE + lane] !=
                  iteration * kThreads + warp * WARP_SIZE + lane;
    }
    sync_barrier(group + 2, kThreads / 2);
  }
  results[tid] = errors;
}

int main() {
  int* results;
  CUDA_CHECK(cudaMalloc(&results, kThreads * sizeof(int)));
  test_primitives<<<1, kThreads>>>(results);
  CUDA_CHECK(cudaGetLastError());
  CUDA_CHECK(cudaDeviceSynchronize());
  std::vector<int> host_results(kThreads);
  CUDA_CHECK(cudaMemcpy(host_results.data(), results, kThreads * sizeof(int),
                        cudaMemcpyDeviceToHost));
  CUDA_CHECK(cudaFree(results));
  for (int errors : host_results)
    if (errors != 0) return 1;
  return 0;
}
