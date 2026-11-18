#pragma once

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <new>
#include <unistd.h>

// Host segments must start at a system-page boundary on the tested UMDK.
// Reserve whole pages so small registrations do not share their page with an
// unrelated heap allocation. The caller still registers its logical length.
template <class T>
class UrmaHostPageAllocator {
 public:
  using value_type = T;

  UrmaHostPageAllocator() noexcept = default;
  template <class U>
  UrmaHostPageAllocator(UrmaHostPageAllocator<U> const&) noexcept {}

  [[nodiscard]] T* allocate(std::size_t count) {
    constexpr std::size_t max = std::numeric_limits<std::size_t>::max();
    if (count > max / sizeof(T)) throw std::bad_array_new_length();
    std::size_t bytes = count * sizeof(T);
    if (!bytes) bytes = 1;
    long const configured_page = sysconf(_SC_PAGESIZE);
    if (configured_page <= 0) throw std::bad_alloc();
    std::size_t const page = static_cast<std::size_t>(configured_page);
    if ((page & (page - 1)) != 0 || page < sizeof(void*) || page < alignof(T))
      throw std::bad_alloc();
    if (bytes > max - (page - 1)) throw std::bad_array_new_length();
    std::size_t const allocation_bytes = (bytes + page - 1) & ~(page - 1);
    void* memory = nullptr;
    if (posix_memalign(&memory, page, allocation_bytes) != 0)
      throw std::bad_alloc();
    return static_cast<T*>(memory);
  }

  void deallocate(T* memory, std::size_t) noexcept { std::free(memory); }
};

template <class T, class U>
bool operator==(UrmaHostPageAllocator<T> const&,
                UrmaHostPageAllocator<U> const&) noexcept {
  return true;
}

template <class T, class U>
bool operator!=(UrmaHostPageAllocator<T> const& a,
                UrmaHostPageAllocator<U> const& b) noexcept {
  return !(a == b);
}
