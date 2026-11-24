#include "local_barrier_name.hpp"
#include <cassert>
#include <cstdio>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

int main() {
  auto legacy = local_barrier_shm_name("127.0.0.1", true, 0, 7, "", "");
  assert(legacy == "/uccl_barrier_127.0.0.1_uid7_ht_th0");
  auto a = local_barrier_shm_name("127.0.0.1", true, 0, 7, "host-a", "29500");
  assert(a == local_barrier_shm_name("127.0.0.1", true, 0, 7, "host-a", "29500"));
  assert(a != local_barrier_shm_name("127.0.0.1", true, 0, 7, "host-a", "29501"));
  assert(a != local_barrier_shm_name("127.0.0.1", true, 0, 7, "host-b", "29500"));
  assert(a != local_barrier_shm_name("127.0.0.1", false, 0, 7, "host-a", "29500"));
  for (auto p : {"0", "65536", "bad", "123456"}) {
    bool rejected = false;
    try { (void)local_barrier_shm_name("127.0.0.1", true, 0, 7, "host-a", p); }
    catch (std::invalid_argument const&) { rejected = true; }
    assert(rejected);
  }
  // Only touch two names incorporating this test process's PID; unlinking one
  // must not remove the independent job's object.
  // Exercise the real generated names on Linux. macOS has a shorter POSIX shm
  // name limit, so it uses short isolated names for the unlink semantics test.
#ifdef __linux__
  std::string ip = "localtest" + std::to_string(getpid());
  auto one = local_barrier_shm_name(ip, true, 0, getuid(), "host-a", "29500");
  auto two = local_barrier_shm_name(ip, true, 0, getuid(), "host-a", "29501");
#else
  std::string prefix = "/ucclt" + std::to_string(getpid());
  auto one = prefix + "a";
  auto two = prefix + "b";
#endif
  int fd1 = shm_open(one.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd1 < 0) { std::perror("shm_open one"); return 1; }
  int fd2 = shm_open(two.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd2 < 0) { std::perror("shm_open two"); close(fd1); shm_unlink(one.c_str()); return 1; }
  close(fd1); close(fd2);
  assert(shm_unlink(one.c_str()) == 0);
  int peer = shm_open(two.c_str(), O_RDWR, 0600); assert(peer >= 0); close(peer);
  assert(shm_unlink(two.c_str()) == 0);
}
