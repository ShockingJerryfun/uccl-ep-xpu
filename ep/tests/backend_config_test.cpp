#include "ep_backend.hpp"
#include <cassert>
#include <cstdlib>
#include <iostream>
int main() {
  unsetenv("UCCL_EP_TRANSPORT");
  assert(!ep_uses_urma());
  setenv("UCCL_EP_TRANSPORT", "verbs", 1);
  assert(!ep_uses_urma());
  setenv("UCCL_EP_TRANSPORT", "unknown", 1);
  assert(!ep_uses_urma());
  setenv("UCCL_EP_TRANSPORT", "cxi", 1);
  assert(!ep_uses_urma());
  setenv("UCCL_EP_TRANSPORT", "urma", 1);
#if defined(USE_URMA) && !defined(USE_SUBSET_BARRIER) && \
    (!defined(__HIPCC__) || defined(USE_METAX))
  assert(ep_uses_urma());
#else
  bool rejected = false;
  try {
    (void)ep_uses_urma();
  } catch (...) {
    rejected = true;
  }
  assert(rejected);
#endif
  std::cout << "backend config tests passed\n";
}
