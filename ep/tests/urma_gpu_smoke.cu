// Two-node production-transport smoke. No perftest or mock provider is used.
#include "urma_transport.hpp"
#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cuda_runtime.h>
#include <sys/socket.h>
#include <unistd.h>

static void cuda_check(cudaError_t rc) {
  if (rc != cudaSuccess) throw std::runtime_error(cudaGetErrorString(rc));
}
static void exchange(int fd, void const* local, void* remote, size_t bytes) {
  // The small descriptor fits normal TCP socket buffers. Both peers send first.
  auto* src = static_cast<char const*>(local);
  auto* dst = static_cast<char*>(remote);
  for (size_t n = 0; n < bytes;) {
    ssize_t got = send(fd, src + n, bytes - n, MSG_NOSIGNAL);
    if (got <= 0) throw std::runtime_error("bootstrap send failed");
    n += static_cast<size_t>(got);
  }
  for (size_t n = 0; n < bytes;) {
    ssize_t got = recv(fd, dst + n, bytes - n, 0);
    if (got <= 0)
      throw std::runtime_error("bootstrap recv failed or timed out");
    n += static_cast<size_t>(got);
  }
}
static int connect_socket(char const* address, int port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) throw std::runtime_error("socket failed");
  timeval timeout{30, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (std::strcmp(address, "listen") == 0) {
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ||
        listen(fd, 1))
      throw std::runtime_error("bind/listen failed");
    int peer = accept(fd, nullptr, nullptr);
    close(fd);
    if (peer < 0) throw std::runtime_error("accept failed or timed out");
    fd = peer;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  } else {
    if (inet_pton(AF_INET, address, &addr.sin_addr) != 1 ||
        connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)))
      throw std::runtime_error(
          "connect failed (start listener first; use IPv4)");
  }
  return fd;
}

// Keep a GPU consumer active before the remote write. This checks that
// observing a mapped-host FADD flag also exposes the preceding device-memory
// payload. A passing CPU copy after cudaDeviceSynchronize alone would not test
// that.
__global__ void consume(uint32_t const* data, unsigned long long volatile* flag,
                        uint32_t expected, int* result,
                        unsigned long long cycles) {
  flag[1] = 1;
  __threadfence_system();
  unsigned long long start = clock64();
  while (*flag == 0) {
    if (clock64() - start > cycles) {
      *result = 1;
      return;
    }
  }
  __threadfence_system();
  for (int i = 0; i < 1024; ++i) {
    uint32_t value;
    asm volatile("ld.global.cv.u32 %0, [%1];"
                 : "=r"(value)
                 : "l"(data + i)
                 : "memory");
    if (value != expected) {
      *result = 2;
      return;
    }
  }
  *result = 0;
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0] << " listen|PEER_IPV4 PORT CUDA_DEVICE\n";
    return 2;
  }
  try {
    int port = std::stoi(argv[2]), device = std::stoi(argv[3]);
    if (port < 1 || port > 65535) throw std::runtime_error("invalid port");
    bool listener = std::strcmp(argv[1], "listen") == 0;
    cuda_check(cudaSetDevice(device));
    uint32_t* gpu = nullptr;
    unsigned long long* control = nullptr;
    int* result = nullptr;
    constexpr size_t bytes = 65536;
    cuda_check(cudaMalloc(reinterpret_cast<void**>(&gpu), bytes));
    cuda_check(cudaHostAlloc(reinterpret_cast<void**>(&control), 64,
                             cudaHostAllocMapped));
    cuda_check(cudaMalloc(reinterpret_cast<void**>(&result), sizeof(int)));
    std::memset(control, 0, 64);
    cuda_check(cudaMemset(gpu, 0, bytes));
    uint32_t sent = listener ? 0x13579bdf : 0x2468ace0;
    uint32_t expected = listener ? 0x2468ace0 : 0x13579bdf;
    std::vector<uint32_t> pattern(1024, sent);
    cuda_check(cudaMemcpy(gpu, pattern.data(), 4096, cudaMemcpyHostToDevice));
    ProxyCtx ctx{};
    ctx.local_rank = device;
    UrmaTransport transport;
    transport.init(ctx);
    transport.register_main_buffer(gpu, bytes - 17, device);
    transport.register_atomic_buffer(control, 64, device);
    // Verify exact repeated registration shares the pin and survives one owner.
    {
      UrmaTransport duplicate;
      duplicate.init(ctx);
      duplicate.register_main_buffer(gpu, bytes - 17, device);
      duplicate.register_atomic_buffer(control, 64, device);
      duplicate.destroy();
    }
    int fd = connect_socket(argv[1], port);
    auto local = transport.local_info();
    RemoteConnInfo remote{};
    exchange(fd, &local, &remote, sizeof(local));
    transport.connect_peer(0, remote);
    cudaDeviceProp props{};
    cuda_check(cudaGetDeviceProperties(&props, device));
    consume<<<1, 1>>>(
        gpu + 16384 / sizeof(uint32_t), control, expected, result,
        static_cast<unsigned long long>(props.clockRate) * 1000 * 20);
    cuda_check(cudaGetLastError());
    auto ready_deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (__atomic_load_n(control + 1, __ATOMIC_ACQUIRE) == 0) {
      if (std::chrono::steady_clock::now() > ready_deadline)
        throw std::runtime_error("GPU consumer did not start");
      std::this_thread::yield();
    }
    char ready = 1, peer_ready = 0;
    exchange(fd, &ready, &peer_ready, 1);
    transport.post_write(0, 1, 0, 16384, 4096, false);
    transport.post_atomic_add(0, 2, 0, 1, true);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int complete = 0;
    while (complete < 2) {
      TransportCompletion completions[2];
      int n = transport.poll(completions, 2);
      for (int i = 0; i < n; ++i)
        if (completions[i].status)
          throw std::runtime_error("URMA completion error");
      complete += n;
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("URMA completion timeout");
      std::this_thread::yield();
    }
    cuda_check(cudaDeviceSynchronize());
    int outcome = -1, peer_outcome = -1;
    cuda_check(
        cudaMemcpy(&outcome, result, sizeof(outcome), cudaMemcpyDeviceToHost));
    // Both GPU consumers finished before either side unimports/unregisters.
    exchange(fd, &outcome, &peer_outcome, sizeof(outcome));
    transport.destroy();
    close(fd);
    cuda_check(cudaFree(result));
    cuda_check(cudaFreeHost(control));
    cuda_check(cudaFree(gpu));
    if (outcome || peer_outcome)
      throw std::runtime_error(
          "GPU validation failed: 1=flag timeout, 2=payload mismatch; local=" +
          std::to_string(outcome) + " peer=" + std::to_string(peer_outcome));
    std::cout << "URMA GPU smoke passed: duplicate registration, bidirectional "
                 "WRITE + FADD, GPU visibility\n";
  } catch (std::exception const& e) {
    std::cerr << "URMA GPU smoke failed: " << e.what() << '\n';
    return 1;
  }
}
