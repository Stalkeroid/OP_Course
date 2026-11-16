#include "search/memory_release.h"

#include <new>

#if defined(__linux__)
#include <sys/mman.h>
#endif
#if defined(__GLIBC__)
#include <malloc.h>
#endif

namespace search {

void* allocatePages(std::size_t bytes) {
#if defined(__linux__)
  void* address = ::mmap(nullptr, bytes, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (address == MAP_FAILED) throw std::bad_alloc();
  return address;  // pages are zero and only become resident when touched
#else
  return ::operator new(bytes);
#endif
}

void freePages(void* address, std::size_t bytes) noexcept {
  if (address == nullptr) return;
#if defined(__linux__)
  ::munmap(address, bytes);
#else
  (void)bytes;
  ::operator delete(address);
#endif
}

void releaseFreeHeapMemory() noexcept {
#if defined(__GLIBC__)
  ::malloc_trim(0);
#endif
}

}  // namespace search
