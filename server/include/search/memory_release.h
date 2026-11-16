// Deterministic return of index memory to the operating system.
//
// Problem (measured on the 2.2 GB / 20 000-document dataset): every rebuild
// left the freed memory of the previous index inside glibc malloc, so RSS
// grew 382 -> 1916 MB over 10 rebuilds although one index needs ~380 MB.
// Causes: (1) posting slabs were 256 KiB malloc blocks; after the first
// munmap glibc raises its dynamic mmap threshold, so later slabs came from
// the per-thread arenas of the pool workers; (2) freed arena memory is kept
// for reuse and only rarely returned; with up to 16 worker threads (one
// arena each, up to 8 x cores) it piles up in arenas that the next build may
// not reuse.
//
// Remedy, kept inside the core library:
//   * allocatePages()/freePages(): the big posting slabs are mapped directly
//     with mmap and returned with munmap when the index dies. This is exact
//     and does not depend on the malloc version or its heuristics. (Fixing
//     M_MMAP_THRESHOLD with mallopt would be global process state that also
//     changes the service's allocations, so it is not used.)
//   * releaseFreeHeapMemory(): malloc_trim(0) after an index has been
//     destroyed and after a build's worker threads have exited. It hands the
//     free pages of ALL arenas (the many small term strings, hash nodes and
//     bucket arrays, per-thread scratch buffers) back with madvise. Unlike
//     MALLOC_ARENA_MAX=1 it costs nothing while building: workers keep their
//     own arenas and allocate without contention.
// Measured (glibc 2.39, 2 cores, builds with 1/2/4/16 threads): RSS after
// each rebuild stays at 381-396 MB over 25 rebuilds, peak during a rebuild
// ~770 MB (old + new index); build time unchanged. Slabs via mmap alone
// still crept ~8 MB per rebuild (the small allocations); malloc_trim alone
// also worked here, but would leave the bulk to malloc heuristics that differ
// between libc versions, so both are kept. ~InvertedIndex + trim: ~40 ms.
// Both are no-ops / plain heap operations on non-glibc platforms (macOS).
#pragma once

#include <cstddef>

namespace search {

// Returns `bytes` of zero-initialised, page-aligned memory (anonymous mmap on
// Linux, operator new elsewhere). Throws std::bad_alloc on failure.
void* allocatePages(std::size_t bytes);

// Releases memory from allocatePages(); `bytes` must match. nullptr is ok.
void freePages(void* address, std::size_t bytes) noexcept;

// Gives free heap memory of all malloc arenas back to the OS (glibc
// malloc_trim); no-op elsewhere. Thread-safe.
void releaseFreeHeapMemory() noexcept;

}  // namespace search
