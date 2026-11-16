#include "search/inverted_index.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <unordered_set>

#include "search/memory_release.h"

namespace search {

namespace {

// Thread-local term -> count table used while tokenising one document.
// Open addressing over a reusable arena: after warm-up a document is counted
// without any heap allocation (capacity is kept between documents).
class TermCounter {
 public:
  struct Entry {
    std::size_t hash;
    std::uint32_t offset;  // token bytes in arena
    std::uint32_t length;
    std::uint32_t count;
  };

  void clear() {
    arena.clear();
    if (!entries.empty()) {
      for (std::uint32_t slot : usedSlots) slots[slot] = kEmpty;
    }
    entries.clear();
    usedSlots.clear();
    if (slots.empty()) slots.assign(1024, kEmpty);
  }

  void add(std::string_view token) {
    const std::size_t hash = std::hash<std::string_view>{}(token);
    std::size_t mask = slots.size() - 1;
    std::size_t pos = hash & mask;
    for (;;) {
      const std::uint32_t idx = slots[pos];
      if (idx == kEmpty) break;
      Entry& e = entries[idx];
      if (e.hash == hash && e.length == token.size() &&
          std::string_view(arena.data() + e.offset, e.length) == token) {
        ++e.count;
        return;
      }
      pos = (pos + 1) & mask;
    }
    if (arena.size() + token.size() >
        std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("TermCounter: document too large");
    }
    const auto offset = static_cast<std::uint32_t>(arena.size());
    arena.append(token);
    slots[pos] = static_cast<std::uint32_t>(entries.size());
    usedSlots.push_back(static_cast<std::uint32_t>(pos));
    entries.push_back(
        {hash, offset, static_cast<std::uint32_t>(token.size()), 1});
    if (entries.size() * 2 > slots.size()) grow();
  }

  std::string_view termOf(const Entry& e) const {
    return std::string_view(arena.data() + e.offset, e.length);
  }

  const std::vector<Entry>& getEntries() const { return entries; }

 private:
  static constexpr std::uint32_t kEmpty =
      std::numeric_limits<std::uint32_t>::max();

  void grow() {
    slots.assign(slots.size() * 2, kEmpty);
    usedSlots.clear();
    const std::size_t mask = slots.size() - 1;
    for (std::uint32_t i = 0; i < entries.size(); ++i) {
      std::size_t pos = entries[i].hash & mask;
      while (slots[pos] != kEmpty) pos = (pos + 1) & mask;
      slots[pos] = i;
      usedSlots.push_back(static_cast<std::uint32_t>(pos));
    }
  }

  std::string arena;
  std::vector<Entry> entries;
  std::vector<std::uint32_t> slots;      // power-of-two size
  std::vector<std::uint32_t> usedSlots;  // to reset only touched slots
};

struct MergeScratch {
  TermCounter counter;
  std::vector<std::uint32_t> byShard;  // entry indices grouped by shard
  std::array<std::uint32_t, InvertedIndex::kShardCount + 1> shardBegin{};
};

}  // namespace

double roundScore(double score) {
  double rounded = std::round(score * 1e6) / 1e6;
  return std::clamp(rounded, 0.000001, 1.0);
}

void InvertedIndex::PostingArena::SlabDeleter::operator()(
    Posting* slab) const noexcept {
  freePages(slab, sizeof(Posting) * kSlabSlots);
}

InvertedIndex::HeapTrimGuard::~HeapTrimGuard() { releaseFreeHeapMemory(); }

std::uint32_t InvertedIndex::PostingArena::allocateChunk(
    std::uint32_t capacity) {
  const std::uint32_t slots = capacity + 1;  // + header slot
  if (usedInLastSlab + slots > kSlabSlots) {
    if (slabs.size() >= (std::size_t{1} << (32 - kSlabBits)) - 1) {
      throw std::length_error("InvertedIndex: posting arena exhausted");
    }
    // Reserve first so push_back cannot throw and leak the mapping.
    slabs.reserve(slabs.size() + 1);
    slabs.emplace_back(
        static_cast<Posting*>(allocatePages(sizeof(Posting) * kSlabSlots)));
    usedInLastSlab = 0;
  }
  const auto address = static_cast<std::uint32_t>(
      ((slabs.size() - 1) << kSlabBits) | usedInLastSlab);
  usedInLastSlab += slots;
  slot(address) = Posting{kNoChunk, capacity};  // header: {next, capacity}
  return address;
}

void InvertedIndex::appendPosting(Shard& shard, PostingList& list,
                                  Posting posting) {
  constexpr std::uint32_t kMinChunk = 4;
  constexpr std::uint32_t kMaxChunk = 256;
  PostingArena& arena = shard.arena;
  if (list.size == 0) {
    list.headChunk = list.tailChunk = arena.allocateChunk(kMinChunk);
    list.tailUsed = 0;
  } else if (list.tailUsed == arena.slot(list.tailChunk).count) {
    // Tail full: next chunk as large as the list so far (geometric), capped.
    const std::uint32_t capacity = std::clamp(list.size, kMinChunk, kMaxChunk);
    const std::uint32_t chunk = arena.allocateChunk(capacity);
    arena.slot(list.tailChunk).doc = chunk;  // header.next
    list.tailChunk = chunk;
    list.tailUsed = 0;
  }
  arena.slot(list.tailChunk + 1 + list.tailUsed) = posting;
  ++list.tailUsed;
  ++list.size;
}

template <typename Visitor>
void InvertedIndex::forEachPosting(const Shard& shard, const PostingList& list,
                                   Visitor&& visit) {
  const PostingArena& arena = shard.arena;
  std::uint32_t remaining = list.size;
  std::uint32_t chunk = list.headChunk;
  while (remaining > 0) {
    const Posting& header = arena.slot(chunk);
    const std::uint32_t n = std::min(remaining, header.count);
    // A chunk never crosses a slab boundary, so its slots are contiguous.
    const Posting* data = &arena.slot(chunk) + 1;
    for (std::uint32_t i = 0; i < n; ++i) visit(data[i]);
    remaining -= n;
    chunk = header.doc;
  }
}

std::size_t InvertedIndex::shardOf(std::size_t termHash) {
  // Fibonacci hashing: take the top bits so the shard choice is independent
  // of the low bits the unordered_map uses for its buckets.
  constexpr int kShardBits = 6;
  static_assert((std::size_t{1} << kShardBits) == kShardCount);
  return static_cast<std::size_t>(
      (static_cast<std::uint64_t>(termHash) * 0x9E3779B97F4A7C15ull) >>
      (64 - kShardBits));
}

void InvertedIndex::addDocument(std::string docId, std::string_view text) {
  thread_local MergeScratch scratch;
  TermCounter& counter = scratch.counter;

  // 1. Tokenise and count with no lock held.
  counter.clear();
  forEachToken(text, [&counter](std::string_view token, std::size_t) {
    counter.add(token);
  });

  // 2. Register the document id.
  std::uint32_t docIndex = 0;
  {
    std::unique_lock<std::shared_mutex> lock(docTableLock);
    if (docIds.size() >= std::numeric_limits<std::uint32_t>::max()) {
      throw std::length_error("InvertedIndex: too many documents");
    }
    docIndex = static_cast<std::uint32_t>(docIds.size());
    docIds.push_back(std::move(docId));
  }

  // 3. Group the distinct terms by shard (counting sort).
  const auto& entries = counter.getEntries();
  auto& begin = scratch.shardBegin;
  begin.fill(0);
  for (const auto& e : entries) ++begin[shardOf(e.hash) + 1];
  for (std::size_t s = 0; s < kShardCount; ++s) begin[s + 1] += begin[s];
  scratch.byShard.resize(entries.size());
  {
    std::array<std::uint32_t, kShardCount> fill{};
    for (std::uint32_t i = 0; i < entries.size(); ++i) {
      const std::size_t s = shardOf(entries[i].hash);
      scratch.byShard[begin[s] + fill[s]++] = i;
    }
  }

  // 4. Merge shard by shard; the exclusive lock covers only the appends.
  const std::size_t startShard = docIndex % kShardCount;
  for (std::size_t step = 0; step < kShardCount; ++step) {
    const std::size_t s = (startShard + step) % kShardCount;
    if (begin[s] == begin[s + 1]) continue;
    Shard& shard = shards[s];
    std::unique_lock<std::shared_mutex> lock(shard.rwLock);
    for (std::uint32_t k = begin[s]; k < begin[s + 1]; ++k) {
      const auto& e = entries[scratch.byShard[k]];
      const std::string_view term = counter.termOf(e);
      auto it = shard.indexMap.find(term);
      if (it == shard.indexMap.end()) {
        it = shard.indexMap.emplace(std::string(term), PostingList{}).first;
      }
      appendPosting(shard, it->second, Posting{docIndex, e.count});
    }
  }

  documentCount.fetch_add(1, std::memory_order_acq_rel);
}

SearchOutput InvertedIndex::search(const std::vector<WeightedTerm>& keywords,
                                   std::size_t limit) const {
  SearchOutput output;
  const std::uint64_t n = documentCount.load(std::memory_order_acquire);
  if (n == 0 || keywords.empty()) return output;

  std::vector<double> raw;
  {
    std::shared_lock<std::shared_mutex> lock(docTableLock);
    raw.assign(docIds.size(), 0.0);
  }
  std::vector<std::uint32_t> matched;

  std::unordered_set<std::string_view> seen;
  for (const WeightedTerm& kw : keywords) {
    if (!(kw.weight > 0.0) || !seen.insert(kw.term).second) continue;
    const std::size_t hash = std::hash<std::string_view>{}(kw.term);
    const Shard& shard = shards[shardOf(hash)];
    std::shared_lock<std::shared_mutex> lock(shard.rwLock);
    auto it = shard.indexMap.find(std::string_view(kw.term));
    if (it == shard.indexMap.end() || it->second.size == 0) continue;
    // With concurrent writers df may momentarily exceed the counted N.
    const double df = static_cast<double>(it->second.size);
    const double docs = std::max(static_cast<double>(n), df);
    const double idf = std::log(1.0 + docs / df);
    const double factor = kw.weight * idf;
    forEachPosting(shard, it->second, [&](const Posting& p) {
      if (p.doc >= raw.size()) raw.resize(std::size_t{p.doc} + 1, 0.0);
      if (raw[p.doc] == 0.0) matched.push_back(p.doc);
      raw[p.doc] += factor * (1.0 + std::log(static_cast<double>(p.count)));
    });
  }

  output.totalMatches = matched.size();
  if (matched.empty() || limit == 0) return output;

  std::shared_lock<std::shared_mutex> lock(docTableLock);
  // "a better than b": higher raw, then smaller doc_id (§4.5).
  auto better = [&](std::uint32_t a, std::uint32_t b) {
    if (raw[a] != raw[b]) return raw[a] > raw[b];
    return docIds[a] < docIds[b];
  };
  // Bounded top-k: a heap of at most `limit` entries whose top is the worst.
  const std::size_t k = std::min(limit, matched.size());
  std::vector<std::uint32_t> heap;
  heap.reserve(k);
  for (std::uint32_t doc : matched) {
    if (heap.size() < k) {
      heap.push_back(doc);
      std::push_heap(heap.begin(), heap.end(), better);
    } else if (better(doc, heap.front())) {
      std::pop_heap(heap.begin(), heap.end(), better);
      heap.back() = doc;
      std::push_heap(heap.begin(), heap.end(), better);
    }
  }
  std::sort_heap(heap.begin(), heap.end(), better);  // best first

  const double maxRaw = raw[heap.front()];
  output.results.reserve(heap.size());
  for (std::uint32_t doc : heap) {
    output.results.push_back(
        SearchResult{docIds[doc], roundScore(raw[doc] / maxRaw), {}});
  }
  return output;
}

IndexStats InvertedIndex::getStats() const {
  IndexStats stats;
  stats.documents = documentCount.load(std::memory_order_acquire);
  for (const Shard& shard : shards) {
    std::shared_lock<std::shared_mutex> lock(shard.rwLock);
    stats.uniqueTerms += shard.indexMap.size();
  }
  stats.indexBuildTimeMs = buildTimeMs.load();
  stats.threadsUsed = buildThreads.load();
  stats.filesSkipped = buildFilesSkipped.load();
  stats.indexReady = true;
  stats.indexing = false;
  return stats;
}

std::uint64_t InvertedIndex::documentFrequency(std::string_view term) const {
  const Shard& shard = shards[shardOf(std::hash<std::string_view>{}(term))];
  std::shared_lock<std::shared_mutex> lock(shard.rwLock);
  auto it = shard.indexMap.find(term);
  return it == shard.indexMap.end() ? 0 : it->second.size;
}

void InvertedIndex::finalize() { finalized.store(true); }

void InvertedIndex::setBuildInfo(std::int64_t buildTimeMsValue, int threadsUsed,
                                 std::uint64_t filesSkipped) {
  buildTimeMs.store(buildTimeMsValue);
  buildThreads.store(threadsUsed);
  buildFilesSkipped.store(filesSkipped);
}

}  // namespace search
