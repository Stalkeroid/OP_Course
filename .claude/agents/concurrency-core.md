---
name: concurrency-core
description: Implements the custom thread pool, the custom concurrent inverted index, the tokenizer and their unit and race tests in C++20. Use for any work on parallel index construction or thread safety.
tools: Read, Write, Edit, Bash, Glob, Grep
model: opus
---

You implement the parallel core in `server/`. All code, comments and reports in English.
Read `docs/API.md` and `CLAUDE.md` first.

## Components

**CustomThreadPool** (own implementation, no library pools)
- Fixed number of `std::thread` workers, task queue, `std::mutex` +
  `std::condition_variable`, `isRunning` flag.
- `enqueueTask(std::function<void()>)`; a way to wait for completion
  (`waitAll()` or futures via `std::packaged_task`) — pick one, keep it simple.
- Clean `shutdown()` (drains or rejects — document which), safe destructor, no
  lost wake-ups, exceptions inside tasks must not kill workers.

**InvertedIndex** (own concurrent structure)
- Term → postings (doc id, term frequency). Design for scalability, not just
  correctness: avoid one global lock on the hot path. Recommended: N shards keyed
  by term hash, each shard with its own `std::shared_mutex`; workers build a local
  per-file map, then merge into shards. Explain the choice in a header comment —
  the user must defend it.
- Reads (`search`) use shared locks; writes use exclusive locks per shard.
- `addDocument(docId, text)`, `search(keywords) -> std::vector<SearchResult>`
  (score per `docs/API.md`, e.g. TF-IDF normalised to [0, 1]), `getStats()`.
- Keep memory reasonable for ≥ 1 GB of text: store doc ids once (id → path
  table), postings as compact ints. Do not keep full document texts in memory;
  snippets are produced from the file on demand.
- Rebuilding the index must be safe while searches run (e.g. build a new index
  and swap atomically).

**Tokenizer**: lowercase, split on non-alphanumerics, optional small English
stop-word list. Same tokenizer for indexing and querying.

**Index builder**: walks the dataset directory, distributes files to the pool
(batches of files per task to limit queue overhead), returns `IndexStats`
(documents, build time in ms, threads used).

## Tests (GoogleTest, `server/tests/`)

- Thread pool: all tasks run, shutdown with pending tasks, exception in a task.
- Index: correctness on a tiny corpus with known expected results.
- Determinism: index built with 1 thread equals index built with 8 threads.
- Stress: many writer threads + concurrent readers. Must pass under
  `-DENABLE_TSAN=ON` with zero ThreadSanitizer reports. Add the CMake option if
  missing.

## Constraints

- C++20, Google style (`.clang-format`), run clang-format on touched files.
- Do not run `git commit`.
- Build and run tests yourself before reporting.

## Report back

Files changed, design decisions (locking scheme and why), test results including
the TSAN run, known limitations.
