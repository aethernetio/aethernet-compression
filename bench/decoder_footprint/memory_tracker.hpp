#ifndef AE_COMPRESSION_BENCH_DECODER_MEMORY_TRACKER_HPP_
#define AE_COMPRESSION_BENCH_DECODER_MEMORY_TRACKER_HPP_

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

extern "C" void* __real_malloc(std::size_t size);
extern "C" void* __real_calloc(std::size_t count, std::size_t size);
extern "C" void* __real_realloc(void* pointer, std::size_t size);
extern "C" void __real_free(void* pointer);

namespace decoder_memory {

inline constexpr std::uint64_t kHeaderMagic = 0xA37EDEC0DEC0A37EULL;

struct Header {
  std::uint64_t magic = kHeaderMagic;
  void* base = nullptr;
  std::size_t size = 0;
};

inline std::size_t current_bytes = 0;
inline std::size_t peak_bytes = 0;
inline std::size_t allocation_count = 0;

inline bool IsPowerOfTwo(std::size_t value) {
  return value != 0 && (value & (value - 1)) == 0;
}

inline void* Allocate(std::size_t size,
                      std::size_t alignment = alignof(std::max_align_t)) {
  alignment = std::max(alignment, alignof(void*));
  if (!IsPowerOfTwo(alignment)) {
    return nullptr;
  }
  if (size > std::numeric_limits<std::size_t>::max() - sizeof(Header) -
                 (alignment - 1)) {
    return nullptr;
  }
  auto const total = size + sizeof(Header) + alignment - 1;
  auto* base = static_cast<std::uint8_t*>(__real_malloc(total == 0 ? 1 : total));
  if (base == nullptr) {
    return nullptr;
  }
  auto const raw = reinterpret_cast<std::uintptr_t>(base + sizeof(Header));
  auto const aligned = (raw + alignment - 1) & ~(alignment - 1);
  auto* header = reinterpret_cast<Header*>(aligned - sizeof(Header));
  header->magic = kHeaderMagic;
  header->base = base;
  header->size = size;
  current_bytes += size;
  peak_bytes = std::max(peak_bytes, current_bytes);
  ++allocation_count;
  return reinterpret_cast<void*>(aligned);
}

inline Header* GetHeader(void* pointer) {
  if (pointer == nullptr) {
    return nullptr;
  }
  auto* header = reinterpret_cast<Header*>(
      static_cast<std::uint8_t*>(pointer) - sizeof(Header));
  return header->magic == kHeaderMagic ? header : nullptr;
}

inline void Deallocate(void* pointer) {
  if (pointer == nullptr) {
    return;
  }
  auto* header = GetHeader(pointer);
  if (header == nullptr) {
    __real_free(pointer);
    return;
  }
  current_bytes -= header->size;
  auto* base = header->base;
  header->magic = 0;
  __real_free(base);
}

inline void* Reallocate(void* pointer, std::size_t size) {
  if (pointer == nullptr) {
    return Allocate(size);
  }
  if (size == 0) {
    Deallocate(pointer);
    return nullptr;
  }
  auto* header = GetHeader(pointer);
  if (header == nullptr) {
    return __real_realloc(pointer, size);
  }
  auto const old_size = header->size;
  auto* replacement = Allocate(size);
  if (replacement == nullptr) {
    return nullptr;
  }
  std::memcpy(replacement, pointer, std::min(old_size, size));
  Deallocate(pointer);
  return replacement;
}

inline void* Callocate(std::size_t count, std::size_t size) {
  if (size != 0 && count > std::numeric_limits<std::size_t>::max() / size) {
    return nullptr;
  }
  auto const bytes = count * size;
  auto* pointer = Allocate(bytes);
  if (pointer != nullptr) {
    std::memset(pointer, 0, bytes);
  }
  return pointer;
}

struct Snapshot {
  std::size_t current = 0;
  std::size_t allocations = 0;
};

inline Snapshot BeginMeasurement() {
  peak_bytes = current_bytes;
  allocation_count = 0;
  return Snapshot{current_bytes, 0};
}

inline std::size_t PeakDelta(Snapshot snapshot) {
  return peak_bytes >= snapshot.current ? peak_bytes - snapshot.current : 0;
}

inline std::size_t LiveDelta(Snapshot snapshot) {
  return current_bytes >= snapshot.current ? current_bytes - snapshot.current
                                           : 0;
}

inline std::size_t AllocationCount() { return allocation_count; }

}  // namespace decoder_memory

extern "C" void* __wrap_malloc(std::size_t size) {
  return decoder_memory::Allocate(size);
}

extern "C" void* __wrap_calloc(std::size_t count, std::size_t size) {
  return decoder_memory::Callocate(count, size);
}

extern "C" void* __wrap_realloc(void* pointer, std::size_t size) {
  return decoder_memory::Reallocate(pointer, size);
}

extern "C" void __wrap_free(void* pointer) {
  decoder_memory::Deallocate(pointer);
}

extern "C" void* __wrap_aligned_alloc(std::size_t alignment,
                                       std::size_t size) {
  return decoder_memory::Allocate(size, alignment);
}

extern "C" int __wrap_posix_memalign(void** result, std::size_t alignment,
                                      std::size_t size) {
  if (result == nullptr || alignment < sizeof(void*) ||
      !decoder_memory::IsPowerOfTwo(alignment)) {
    return EINVAL;
  }
  auto* pointer = decoder_memory::Allocate(size, alignment);
  if (pointer == nullptr) {
    return ENOMEM;
  }
  *result = pointer;
  return 0;
}

inline void* operator new(std::size_t size) {
  auto* pointer = decoder_memory::Allocate(size);
  if (pointer == nullptr) {
    std::abort();
  }
  return pointer;
}

inline void* operator new[](std::size_t size) {
  return ::operator new(size);
}

inline void operator delete(void* pointer) noexcept {
  decoder_memory::Deallocate(pointer);
}

inline void operator delete[](void* pointer) noexcept {
  decoder_memory::Deallocate(pointer);
}

inline void operator delete(void* pointer, std::size_t) noexcept {
  decoder_memory::Deallocate(pointer);
}

inline void operator delete[](void* pointer, std::size_t) noexcept {
  decoder_memory::Deallocate(pointer);
}

inline void* operator new(std::size_t size, std::align_val_t alignment) {
  auto* pointer = decoder_memory::Allocate(
      size, static_cast<std::size_t>(alignment));
  if (pointer == nullptr) {
    std::abort();
  }
  return pointer;
}

inline void* operator new[](std::size_t size, std::align_val_t alignment) {
  return ::operator new(size, alignment);
}

inline void operator delete(void* pointer, std::align_val_t) noexcept {
  decoder_memory::Deallocate(pointer);
}

inline void operator delete[](void* pointer, std::align_val_t) noexcept {
  decoder_memory::Deallocate(pointer);
}

inline void operator delete(void* pointer, std::size_t,
                            std::align_val_t) noexcept {
  decoder_memory::Deallocate(pointer);
}

inline void operator delete[](void* pointer, std::size_t,
                              std::align_val_t) noexcept {
  decoder_memory::Deallocate(pointer);
}

#endif  // AE_COMPRESSION_BENCH_DECODER_MEMORY_TRACKER_HPP_
