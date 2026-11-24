// Two-node MetaX production-transport smoke. No perftest or mock provider.
#include "gpu_p2p_runtime.hpp"
#include "urma_transport.hpp"

#include <arpa/inet.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/socket.h>
#include <unistd.h>

namespace {
void exchange(int fd, void const* local, void* remote, std::size_t bytes) {
  auto* src = static_cast<char const*>(local);
  auto* dst = static_cast<char*>(remote);
  for (std::size_t n = 0; n < bytes;) {
    ssize_t got = send(fd, src + n, bytes - n, MSG_NOSIGNAL);
    if (got <= 0) throw std::runtime_error("bootstrap send failed");
    n += static_cast<std::size_t>(got);
  }
  for (std::size_t n = 0; n < bytes;) {
    ssize_t got = recv(fd, dst + n, bytes - n, 0);
    if (got <= 0) throw std::runtime_error("bootstrap recv failed or timed out");
    n += static_cast<std::size_t>(got);
  }
}

int connect_socket(char const* address, int port) {
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
  } else if (inet_pton(AF_INET, address, &addr.sin_addr) != 1 ||
             connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr))) {
    throw std::runtime_error("connect failed (start listener first; use IPv4)");
  }
  return fd;
}
}  // namespace

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << "Usage: " << argv[0]
              << " listen|PEER_IPV4 PORT METAX_DEVICE\n";
    return 2;
  }
  try {
    int const port = std::stoi(argv[2]);
    int const device = std::stoi(argv[3]);
    if (port < 1 || port > 65535) throw std::runtime_error("invalid port");
    bool const listener = std::strcmp(argv[1], "listen") == 0;
    constexpr std::size_t bytes = 64 * 1024;
    constexpr std::size_t atomic_bytes = 4096;
    auto* gpu = uccl::gpu_p2p::allocate_device(bytes, device);
    auto* control = uccl::gpu_p2p::allocate_atomic_buffer(atomic_bytes, device);
    uccl::gpu_p2p::zero_memory(gpu, bytes,
                               uccl::gpu_p2p::MemoryKind::Device, device);
    uccl::gpu_p2p::zero_memory(control, atomic_bytes,
                               uccl::gpu_p2p::MemoryKind::Device, device);

    uint32_t const sent = listener ? 0x13579bdf : 0x2468ace0;
    uint32_t const expected = listener ? 0x2468ace0 : 0x13579bdf;
    std::vector<uint32_t> pattern(1024, sent);
    uccl::gpu_p2p::copy_host_to_device(gpu, pattern.data(), 4096, device);

    ProxyCtx ctx{};
    ctx.local_rank = device;
    UrmaTransport transport;
    transport.init(ctx);
    transport.register_main_buffer(gpu, bytes - 17, device);
    transport.register_atomic_buffer(control, atomic_bytes, device);
    {
      UrmaTransport duplicate;
      duplicate.init(ctx);
      duplicate.register_main_buffer(gpu, bytes - 17, device);
      duplicate.register_atomic_buffer(control, atomic_bytes, device);
      duplicate.destroy();
    }

    int fd = connect_socket(argv[1], port);
    auto local = transport.local_info();
    RemoteConnInfo remote{};
    exchange(fd, &local, &remote, sizeof(local));
    transport.connect_peer(0, remote);
    char ready = 1, peer_ready = 0;
    exchange(fd, &ready, &peer_ready, 1);
    transport.post_write(0, 1, 0, 16384, 4096, false);
    transport.post_atomic_add(0, 2, 0, 1, true);

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    int complete = 0;
    while (complete < 2) {
      TransportCompletion completions[2];
      int const n = transport.poll(completions, 2);
      for (int i = 0; i < n; ++i)
        if (completions[i].status)
          throw std::runtime_error("URMA completion error");
      complete += n;
      if (std::chrono::steady_clock::now() > deadline)
        throw std::runtime_error("URMA completion timeout");
      std::this_thread::yield();
    }

    exchange(fd, &ready, &peer_ready, 1);
    std::vector<uint32_t> received(1024);
    uint64_t atomic_value = 0;
    uccl::gpu_p2p::copy_device_to_host(
        received.data(), static_cast<unsigned char*>(gpu) + 16384, 4096,
        device);
    uccl::gpu_p2p::copy_device_to_host(&atomic_value, control,
                                       sizeof(atomic_value), device);
    transport.destroy();
    close(fd);
    uccl::gpu_p2p::release_memory(control,
                                  uccl::gpu_p2p::MemoryKind::Device, device);
    uccl::gpu_p2p::release_memory(gpu, uccl::gpu_p2p::MemoryKind::Device,
                                  device);
    for (auto value : received)
      if (value != expected)
        throw std::runtime_error("MetaX P2P WRITE payload mismatch");
    if (atomic_value != 1)
      throw std::runtime_error("MetaX P2P FADD result mismatch");
    std::cout << "URMA MetaX smoke passed: duplicate registration, "
                 "bidirectional WRITE + FADD\n";
  } catch (std::exception const& e) {
    std::cerr << "URMA MetaX smoke failed: " << e.what() << '\n';
    return 1;
  }
}
