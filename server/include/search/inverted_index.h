// InvertedIndex — own concurrent inverted index (docs/API.md §4.3–§4.5).
//
// Locking scheme (why: many writers during a build, many readers afterwards):
//   * The term dictionary is split into kShardCount shards by term hash. Each
//     shard owns its part of indexMap and its own std::shared_mutex rwLock.
//     Writers lock one shard exclusively at a time; readers (search) take a
//     shared lock on the shard of each query term. Two writers only contend
//     when they touch the same shard at the same moment, instead of always
//     (single global lock).
//   * addDocument() tokenises the text into a thread-local term->count table
//     with NO lock held (the expensive part), then merges it shard by shard:
//     lock shard, append postings, unlock. Each writer starts its walk at a
//     different shard (docIndex % kShardCount) so writers do not convoy.
//   * docIds (internal uint32 -> doc_id string) has its own shared_mutex
//     docTableLock. A document gets its internal id before its postings are
//     published, so a reader never sees a posting whose id is unknown.
//   * Locks are never nested (a thread holds at most one of them), so no lock
//     ordering issue / deadlock is possible.
//   * documentCount is an atomic, incremented after the merge.
//
// Memory layout: postings are {uint32 doc, uint32 count} (8 bytes); doc_id
// strings are stored once in docIds; document text is never stored.
// A posting list is a chain of chunks carved from large per-shard slabs
// (chunk capacity grows 4, 4, 8, ... up to 256 postings). Appending never
// reallocates or copies a list (short exclusive sections, no heap
// fragmentation), and unused space is bounded by the last chunk of each list.
// This keeps the 2 GB / 20 000-document dataset well under 1.5 GB RSS, where
// std::vector doubling + realloc fragmentation needed ~1.6 GB.
// Slabs are mmap'ed pages (allocatePages) and are unmapped by the destructor;
// the last member to be destroyed then calls malloc_trim, so a dropped index
// gives its memory back to the OS instead of leaving it in malloc arenas
// (see memory_release.h).
//
// Determinism: internal ids depend on insertion order, but scores do not
// (each raw(d) is summed in query-term order), and ties are broken by the
// doc_id string, so observable results are independent of thread count.
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "search/tokenizer.h"

namespace search {

// DTO from the class diagram / docs/API.md §8.2. `snippet` is left empty by
// InvertedIndex::search and filled later by the caller (makeSnippet*).
struct SearchResult {
  std::string docId;
  double score = 0.0;  // rounded/clamped per §2.4
  std::string snippet;
};

// DTO from the class diagram / docs/API.md §7.2 and §9.2.
// indexReady / indexing describe server state; InvertedIndex::getStats()
// reports indexReady = true, indexing = false and the server overrides them.
struct IndexStats {
  std::uint64_t documents = 0;
  std::int64_t indexBuildTimeMs = 0;
  int threadsUsed = 0;
  std::uint64_t uniqueTerms = 0;
  std::uint64_t filesSkipped = 0;
  bool indexReady = false;
  bool indexing = false;
};

struct SearchOutput {
  std::vector<SearchResult> results;  // ordered per §4.5, at most `limit`
  std::uint64_t totalMatches = 0;     // matches before applying the limit
};

// §2.4: round to 6 decimals, clamp to [0.000001, 1.0].
double roundScore(double score);

class InvertedIndex {
 public:
  static constexpr std::size_t kShardCount = 64;

  struct Posting {
    std::uint32_t doc;    // internal document number (index into docIds)
    std::uint32_t count;  // c(t, d)
  };

  InvertedIndex() = default;
  InvertedIndex(const InvertedIndex&) = delete;
  InvertedIndex& operator=(const InvertedIndex&) = delete;

  // Tokenises `text` (§4.1) and adds it as document `docId`. Thread-safe;
  // may run concurrently with other addDocument() and search() calls.
  // An empty text is a valid document with no terms.
  void addDocument(std::string docId, std::string_view text);

  // Weighted OR search (§4.3–§4.5). Duplicate terms are ignored (first one
  // wins); terms with weight <= 0 are ignored. Thread-safe.
  SearchOutput search(const std::vector<WeightedTerm>& keywords,
                      std::size_t limit) const;

  // Documents, unique terms and the build info set by setBuildInfo().
  IndexStats getStats() const;

  // df(t); 0 if the term is unknown.
  std::uint64_t documentFrequency(std::string_view term) const;

  std::uint64_t getDocumentCount() const {
    return documentCount.load(std::memory_order_acquire);
  }

  // Called once by the builder after all addDocument() calls; marks the index
  // complete (isFinalized()). Chunked posting lists have no growth slack to
  // trim, so this is cheap.
  void finalize();
  bool isFinalized() const { return finalized.load(); }

  // Build metadata reported by getStats() (set by IndexBuilder).
  void setBuildInfo(std::int64_t buildTimeMs, int threadsUsed,
                    std::uint64_t filesSkipped);

 private:
  struct TermHash {
    using is_transparent = void;
    std::size_t operator()(std::string_view s) const noexcept {
      return std::hash<std::string_view>{}(s);
    }
  };

  // Chain of chunks inside the shard's slabs. A chunk is a header slot
  // {next chunk, capacity} followed by `capacity` postings.
  struct PostingList {
    std::uint32_t headChunk = 0;
    std::uint32_t tailChunk = 0;
    std::uint32_t tailUsed = 0;  // postings used in the tail chunk
    std::uint32_t size = 0;      // df(t)
  };

  // Bump allocator of posting chunks; slabs are never moved or freed until
  // the index is destroyed. Guarded by the owning shard's rwLock.
  class PostingArena {
   public:
    static constexpr std::uint32_t kSlabBits = 15;  // 32768 slots = 256 KiB
    static constexpr std::uint32_t kSlabSlots = 1u << kSlabBits;
    static constexpr std::uint32_t kNoChunk = 0xFFFFFFFFu;

    // Returns the address of a new chunk with `capacity` posting slots.
    std::uint32_t allocateChunk(std::uint32_t capacity);
    Posting& slot(std::uint32_t address) {
      return slabs[address >> kSlabBits][address & (kSlabSlots - 1)];
    }
    const Posting& slot(std::uint32_t address) const {
      return slabs[address >> kSlabBits][address & (kSlabSlots - 1)];
    }

   private:
    // Slab memory comes straight from the OS (mmap) and goes back on
    // destruction (munmap), independent of malloc's arenas and thresholds.
    struct SlabDeleter {
      void operator()(Posting* slab) const noexcept;
    };
    std::vector<std::unique_ptr<Posting[], SlabDeleter>> slabs;
    std::uint32_t usedInLastSlab = kSlabSlots;
  };

  using PostingMap =
      std::unordered_map<std::string, PostingList, TermHash, std::equal_to<>>;

  struct alignas(64) Shard {
    mutable std::shared_mutex rwLock;
    PostingMap indexMap;
    PostingArena arena;
  };

  static void appendPosting(Shard& shard, PostingList& list, Posting posting);
  template <typename Visitor>
  static void forEachPosting(const Shard& shard, const PostingList& list,
                             Visitor&& visit);

  static std::size_t shardOf(std::size_t termHash);

  // Declared FIRST so it is destroyed LAST: once the term dictionaries,
  // doc-id strings and slabs are gone, it calls releaseFreeHeapMemory().
  // A member (not ~InvertedIndex body) because the body runs before the
  // members are freed. Runs in whichever thread drops the last shared_ptr.
  struct HeapTrimGuard {
    HeapTrimGuard() = default;
    HeapTrimGuard(const HeapTrimGuard&) = delete;
    HeapTrimGuard& operator=(const HeapTrimGuard&) = delete;
    ~HeapTrimGuard();
  };
  HeapTrimGuard heapTrimGuard;

  std::array<Shard, kShardCount> shards;

  mutable std::shared_mutex docTableLock;
  std::vector<std::string> docIds;

  std::atomic<std::uint64_t> documentCount{0};

  std::atomic<std::int64_t> buildTimeMs{0};
  std::atomic<int> buildThreads{0};
  std::atomic<std::uint64_t> buildFilesSkipped{0};
  std::atomic<bool> finalized{false};
};

}  // namespace search
