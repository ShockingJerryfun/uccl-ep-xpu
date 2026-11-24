#include "ep_configs.cuh"
#include "exception.cuh"
#include "internode_ll.cuh"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <utility>
#include <vector>

void* allocate(size_t bytes,
               std::vector<std::pair<void*, size_t>>& allocations) {
  bytes = std::max(bytes, size_t(16));
  void* ptr;
  CUDA_CHECK(cudaMalloc(&ptr, bytes + 128));
  CUDA_CHECK(cudaMemset(ptr, 0, bytes));
  CUDA_CHECK(cudaMemset(static_cast<char*>(ptr) + bytes, 0xa5, 128));
  allocations.emplace_back(ptr, bytes);
  return ptr;
}

float decode_fp8(uint8_t bits) {
  int exponent = (bits >> 3) & 15, mantissa = bits & 7;
  if (exponent == 15 && mantissa == 7)
    return std::numeric_limits<float>::quiet_NaN();
  float value = exponent == 0
                    ? std::ldexp(float(mantissa), -9)
                    : std::ldexp(1.0f + mantissa / 8.0f, exponent - 7);
  return bits & 128 ? -value : value;
}

void run(int tokens, int hidden, int topk, bool fp8, bool split) {
  constexpr int experts = 8;
  int capacity = std::max(16, (tokens + 15) / 16 * 16);
  int scales = hidden / 128;
  size_t slots = size_t(experts) * capacity;
  size_t message_bytes = 16 + hidden * (fp8 ? 1 : 2) + (fp8 ? scales * 4 : 0);
  std::vector<std::pair<void*, size_t>> allocations;
  auto alloc = [&](size_t bytes) { return allocate(bytes, allocations); };
  auto input = alloc(size_t(tokens) * hidden * sizeof(nv_bfloat16));
  auto indices =
      static_cast<int64_t*>(alloc(size_t(tokens) * topk * sizeof(int64_t)));
  auto weights =
      static_cast<float*>(alloc(size_t(tokens) * topk * sizeof(float)));
  auto packed = alloc(slots * hidden * (fp8 ? 1 : sizeof(nv_bfloat16)));
  auto packed_scales = alloc(slots * scales * sizeof(float));
  auto source = static_cast<int*>(alloc(slots * sizeof(int)));
  auto layout = static_cast<int64_t*>(alloc(experts * sizeof(int64_t)));
  auto counts = static_cast<int*>(alloc(experts * sizeof(int)));
  auto dispatch_recv = alloc(slots * message_bytes);
  auto dispatch_send = alloc(size_t(1 + experts) * capacity * message_bytes);
  auto dispatch_count = static_cast<int*>(alloc(experts * sizeof(int)));
  auto dispatch_count_remote =
      static_cast<int64_t*>(alloc(experts * sizeof(int64_t)));
  auto dispatch_workspace = alloc((3 * experts + 2) * sizeof(int));
  auto expert_output = alloc(slots * hidden * sizeof(nv_bfloat16));
  auto output = alloc(size_t(tokens) * hidden * sizeof(nv_bfloat16));
  auto combine_recv = alloc(slots * hidden * sizeof(nv_bfloat16));
  auto combine_send = alloc(slots * hidden * sizeof(nv_bfloat16));
  auto combine_flag = static_cast<int*>(alloc(experts * sizeof(int)));
  auto combine_flag_remote =
      static_cast<int64_t*>(alloc(experts * sizeof(int64_t)));
  auto combine_workspace = alloc(2 * sizeof(int));
  auto local_bases = static_cast<void**>(alloc(sizeof(void*)));
  CUDA_CHECK(cudaMemcpy(local_bases, &dispatch_recv, sizeof(void*),
                        cudaMemcpyHostToDevice));
  cudaStream_t stream;
  CUDA_CHECK(cudaStreamCreate(&stream));

  printf("RUN local LL N%d H%d K%d FP8%d split%d\n", tokens, hidden, topk, fp8,
         split);
  fflush(stdout);
  for (int repeat = 0; repeat < 2; ++repeat) {
    std::vector<nv_bfloat16> x(size_t(tokens) * hidden);
    std::vector<int64_t> route(size_t(tokens) * topk);
    std::vector<float> weight(route.size());
    std::vector<int> expected_counts(experts, 0);
    for (int t = 0; t < tokens; ++t) {
      for (int h = 0; h < hidden; ++h)
        x[size_t(t) * hidden + h] = __float2bfloat16(
            std::ldexp(float((t * 3 + h * 5 + repeat * 7) % 257 - 128) / 128,
                       (h / 128) % 5 - 2));
      for (int k = 0; k < topk; ++k) {
        int expert = repeat == 0 ? (t + k) % experts : k;
        if ((t + k) % 5 == 0) expert = -1;
        route[t * topk + k] = expert;
        weight[t * topk + k] = float((t + k) % 4 + 1) / 4;
        if (expert >= 0) ++expected_counts[expert];
      }
    }
    if (tokens) {
      CUDA_CHECK(cudaMemcpy(input, x.data(), x.size() * sizeof(nv_bfloat16),
                            cudaMemcpyHostToDevice));
      CUDA_CHECK(cudaMemcpy(indices, route.data(),
                            route.size() * sizeof(int64_t),
                            cudaMemcpyHostToDevice));
      CUDA_CHECK(cudaMemcpy(weights, weight.data(),
                            weight.size() * sizeof(float),
                            cudaMemcpyHostToDevice));
    }

    auto dispatch = [&](int phases) {
      uccl::internode_ll::dispatch(
          packed, packed_scales, source, layout, counts, nullptr, nullptr,
          dispatch_recv, dispatch_count, dispatch_send, input, indices,
          combine_flag, combine_flag_remote, experts, tokens, hidden, capacity,
          topk, experts, 0, 1, fp8, true, false, dispatch_workspace, 4, stream,
          phases, nullptr, 0, 1, 0, local_bases, dispatch_recv, nullptr,
          dispatch_count_remote);
      CUDA_CHECK(cudaStreamSynchronize(stream));
    };
    if (split) {
      dispatch(LOW_LATENCY_SEND_PHASE);
      dispatch(LOW_LATENCY_RECV_PHASE);
    } else {
      dispatch(LOW_LATENCY_SEND_PHASE | LOW_LATENCY_RECV_PHASE);
    }

    std::vector<int> got_counts(experts), got_source(slots);
    std::vector<int64_t> got_layout(experts);
    std::vector<nv_bfloat16> got_bf16(slots * hidden);
    std::vector<uint8_t> got_fp8(fp8 ? slots * hidden : 0);
    std::vector<float> got_scales(fp8 ? slots * scales : 0);
    CUDA_CHECK(cudaMemcpy(got_counts.data(), counts, experts * sizeof(int),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(got_source.data(), source, slots * sizeof(int),
                          cudaMemcpyDeviceToHost));
    CUDA_CHECK(cudaMemcpy(got_layout.data(), layout, experts * sizeof(int64_t),
                          cudaMemcpyDeviceToHost));
    if (fp8) {
      CUDA_CHECK(cudaMemcpy(got_fp8.data(), packed, got_fp8.size(),
                            cudaMemcpyDeviceToHost));
      CUDA_CHECK(cudaMemcpy(got_scales.data(), packed_scales,
                            got_scales.size() * sizeof(float),
                            cudaMemcpyDeviceToHost));
    } else {
      CUDA_CHECK(cudaMemcpy(got_bf16.data(), packed,
                            got_bf16.size() * sizeof(nv_bfloat16),
                            cudaMemcpyDeviceToHost));
    }
    std::vector<char> seen(size_t(experts) * tokens, 0);
    std::vector<nv_bfloat16> response(slots * hidden);
    std::vector<float> expected(size_t(tokens) * hidden, 0);
    for (int e = 0; e < experts; ++e) {
      if (got_counts[e] != expected_counts[e] ||
          uint32_t(got_layout[e]) != uint32_t(expected_counts[e]) ||
          uint64_t(got_layout[e]) >> 32 != 0) {
        fprintf(stderr, "dispatch count/layout mismatch expert=%d\n", e);
        exit(2);
      }
      for (int s = 0; s < got_counts[e]; ++s) {
        int t = got_source[size_t(e) * capacity + s];
        if (t < 0 || t >= tokens || seen[size_t(e) * tokens + t]) {
          fprintf(stderr, "dispatch source index mismatch expert=%d slot=%d\n",
                  e, s);
          exit(3);
        }
        seen[size_t(e) * tokens + t] = 1;
        int k = 0;
        while (k < topk && route[t * topk + k] != e) ++k;
        if (k == topk) {
          fprintf(stderr, "dispatch sent token to wrong expert\n");
          exit(4);
        }
        for (int h = 0; h < hidden; ++h) {
          size_t index = (size_t(e) * capacity + s) * hidden + h;
          float actual =
              fp8 ? decode_fp8(got_fp8[index]) *
                        got_scales[(size_t(e) * scales + h / 128) * capacity +
                                   s]
                  : __bfloat162float(got_bf16[index]);
          float wanted = __bfloat162float(x[size_t(t) * hidden + h]);
          float tolerance = fp8 ? std::abs(wanted) * 0.07f + 1e-6f : 0;
          if (!std::isfinite(actual) || std::abs(actual - wanted) > tolerance) {
            fprintf(stderr,
                    "dispatch payload mismatch expert=%d token=%d h=%d got=%g "
                    "expected=%g\n",
                    e, t, h, actual, wanted);
            exit(5);
          }
          response[index] = __float2bfloat16(actual + (e + 1) * 0.25f);
          expected[size_t(t) * hidden + h] +=
              __bfloat162float(response[index]) * weight[t * topk + k];
        }
      }
    }
    CUDA_CHECK(cudaMemcpy(expert_output, response.data(),
                          response.size() * sizeof(nv_bfloat16),
                          cudaMemcpyHostToDevice));
    CUDA_CHECK(cudaMemsetAsync(
        output, 0x7f, size_t(tokens) * hidden * sizeof(nv_bfloat16), stream));
    auto combine = [&](int phases) {
      uccl::internode_ll::combine(
          output, combine_recv, combine_flag, combine_send, expert_output,
          indices, weights, source, layout, nullptr, dispatch_count,
          dispatch_count_remote, experts, tokens, hidden, capacity, topk,
          experts, 0, 1, false, combine_workspace, 4, stream, phases, false,
          nullptr, 0, 1, 0, local_bases, combine_recv, nullptr,
          combine_flag_remote);
      CUDA_CHECK(cudaStreamSynchronize(stream));
    };
    if (split) {
      combine(LOW_LATENCY_SEND_PHASE);
      combine(LOW_LATENCY_RECV_PHASE);
    } else {
      combine(LOW_LATENCY_SEND_PHASE | LOW_LATENCY_RECV_PHASE);
    }
    std::vector<nv_bfloat16> got(expected.size());
    if (tokens)
      CUDA_CHECK(cudaMemcpy(got.data(), output,
                            got.size() * sizeof(nv_bfloat16),
                            cudaMemcpyDeviceToHost));
    for (size_t i = 0; i < got.size(); ++i) {
      float wanted = __bfloat162float(__float2bfloat16(expected[i]));
      if (__bfloat162float(got[i]) != wanted) {
        fprintf(stderr, "combine mismatch index=%zu got=%g expected=%g\n", i,
                __bfloat162float(got[i]), wanted);
        exit(6);
      }
    }
  }
  CUDA_CHECK(cudaStreamDestroy(stream));
  for (auto const& allocation : allocations) {
    unsigned char guard[128];
    CUDA_CHECK(cudaMemcpy(
        guard, static_cast<char*>(allocation.first) + allocation.second,
        sizeof(guard), cudaMemcpyDeviceToHost));
    for (unsigned char byte : guard)
      if (byte != 0xa5) {
        fprintf(stderr, "buffer guard overwritten\n");
        exit(7);
      }
    CUDA_CHECK(cudaFree(allocation.first));
  }
}

int main() {
  CUDA_CHECK(cudaSetDevice(0));
  for (int tokens : {0, 1, 31, 32, 33, 63, 64, 65, 129, 257})
    for (int hidden : {2048, 8192})
      for (int topk : {1, 4, 8})
        for (bool fp8 : {false, true})
          for (bool split : {false, true})
            run(tokens, hidden, topk, fp8, split);
  puts("PASS local low-latency dispatch/combine: 240 cases, 2 iterations each");
}
