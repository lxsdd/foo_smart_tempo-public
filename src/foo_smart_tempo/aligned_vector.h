#pragma once

#include <cstddef>
#include <limits>
#include <malloc.h>
#include <new>
#include <vector>

namespace smart_tempo {

template <typename T, std::size_t Alignment>
class AlignedAllocator {
 public:
  using value_type = T;

  AlignedAllocator() noexcept = default;
  template <typename U>
  AlignedAllocator(const AlignedAllocator<U, Alignment>&) noexcept {}

  [[nodiscard]] T* allocate(std::size_t n) {
    if (n > (std::numeric_limits<std::size_t>::max)() / sizeof(T)) {
      throw std::bad_alloc();
    }
    void* ptr = _aligned_malloc(n * sizeof(T), Alignment);
    if (ptr == nullptr) throw std::bad_alloc();
    return static_cast<T*>(ptr);
  }

  void deallocate(T* p, std::size_t) noexcept { _aligned_free(p); }

  template <typename U>
  struct rebind {
    using other = AlignedAllocator<U, Alignment>;
  };
};

template <typename T, typename U, std::size_t Alignment>
constexpr bool operator==(const AlignedAllocator<T, Alignment>&,
                          const AlignedAllocator<U, Alignment>&) noexcept {
  return true;
}

template <typename T, typename U, std::size_t Alignment>
constexpr bool operator!=(const AlignedAllocator<T, Alignment>& a,
                          const AlignedAllocator<U, Alignment>& b) noexcept {
  return !(a == b);
}

template <typename T>
using aligned_vector = std::vector<T, AlignedAllocator<T, 64>>;

}  // namespace smart_tempo
