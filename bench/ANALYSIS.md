# Bottleneck analysis (draft)

This file explains the shape of the curves qualitatively. It contains no
measured numbers on purpose: all numbers are in `results/REPORT.md` and the
charts in `charts/`. Passages marked **[TO COMPLETE]** must be filled in by the
student after the local run, together with the hardware description.

## 1. What is being measured

`POST /api/index {threads: p}` rebuilds the whole index from scratch (API.md 7.4).
The coordinating (HTTP request) thread walks the directory tree and enqueues one
task per document into a fresh `CustomThreadPool` with `p` workers. A worker reads
the file, tokenises it, counts terms in a thread-local map without holding any
lock, and then merges the counts into the shared `InvertedIndex`. The index has 64
shards, each with its own `std::shared_mutex`; a worker locks one shard at a time,
appends the postings, and unlocks. `time_ms` covers traversal, reading,
tokenisation, merging, pool shutdown and finalisation (document frequencies).

Search (`POST /api/search`) is served by the HTTP server's worker threads
(`max(16, 2 x cores)`). A request tokenises the query, optionally calls Ollama
(blocking, inside the request), takes shared locks on the shards of the query terms,
scores the postings, and reads the top documents from disk to cut snippets. The
WebSocket path runs queries on our own thread pool but shares the same index.

## 2. Index build: speedup and efficiency

Terms used: S(p) = T(1)/T(p), E(p) = S(p)/p. The Karp-Flatt metric
e(p) = (1/S - 1/p) / (1 - 1/p) gives, for each measured point, the serial fraction
that Amdahl's law would need to explain that speedup. It is the main diagnostic:

- If e(p) is roughly constant as p grows, the loss is a true serial part
  (Amdahl); the speedup limit is about 1/e.
- If e(p) grows with p, the loss comes from overhead that grows with the number
  of threads (lock contention, memory-bandwidth saturation, scheduling, cache
  thrashing) or from running out of hardware parallelism.
- If E(p) falls sharply when p passes the number of physical cores and then
  stays flat or slightly improves, the cause is the hardware (cores, SMT), not the
  code.

Candidate causes in this implementation, with the evidence to look for in
`REPORT.md`:

1. **Serial part on the coordinating thread.** Directory traversal and task
   submission are sequential. With 20 000 files this is small compared with
   reading and tokenising 2 GB, but it is a constant that appears as a
   non-zero e(p) already at p = 2 and caps the speedup at about 1/e. The final
   `finalize()` step and pool start-up / shutdown are also not parallel in the
   same way.
2. **Disk and page cache.** The build reads the whole dataset. The warm-up build
   fills the page cache, so the timed runs normally read from RAM; if the dataset
   does not fit in the memory available to Docker (see the hardware record), runs
   hit the disk, the 1-thread time is inflated by waiting, and the curve then
   looks flat for a different reason (I/O bound, not CPU bound). Evidence: high
   run-to-run std, and times that depend on what ran before. Inside Docker
   Desktop the dataset volume lives in a VM; file I/O there is slower than native.
3. **Memory bandwidth.** Tokenising and counting is memory-intensive (hash maps,
   string handling). When all cores stream through memory at once the bandwidth
   of a laptop is shared, E(p) drops gradually with p even with no lock
   contention, and e(p) rises smoothly.
4. **Shard lock contention in the merge.** The expensive work (read, tokenise,
   count) is lock-free; only the short merge takes a shard lock, and workers
   start at different shards (`docIndex % 64`) to avoid convoys. With 64 shards
   and at most 16 writers, collisions should be rare, so a lock effect would show
   up only as a late rise of e(p) at the highest thread counts. The posting
   slabs are per shard, so there is also no global allocator lock for postings;
   temporary strings and per-thread maps still go through the C++ allocator,
   whose arena behaviour (glibc malloc arenas, musl in a slim image) can limit
   scaling and is worth testing (section 5).
5. **Hyper-threading and oversubscription.** Logical cores share execution
   units. Expect near-linear speedup up to the physical core count, a smaller
   gain up to the logical thread count, and no gain (or a slight loss from
   context switching) beyond it. The default thread list goes up to twice the
   logical core count so this edge is visible.
6. **Docker CPU limits.** If the container (or Docker Desktop VM) gets fewer
   CPUs than the host has, the curve flattens at the limit, not at the physical
   core count. Compare the flattening point with the CPU limit in
   `environment.json` (filled in by hand).
7. **Task granularity.** One task per document is fine-grained enough for load
   balance (document sizes vary), but each task submission goes through the
   pool's queue lock; with very small documents this queue is a contention point
   at high p.

**[TO COMPLETE after the local run]**: state at which thread count the speedup
flattens, the Karp-Flatt trend (constant or growing), the physical vs logical
core count of the laptop, and which of causes 1-7 the evidence supports. Do not
claim a cause without pointing at a table row or a chart.

## 3. Search throughput

Keyword-only (`expand=false`) throughput typically rises with the number of
clients until the server saturates, then stays flat while latency grows
linearly (a closed-loop system: latency = clients / throughput once saturated).
The client count where 95% of peak is reached (reported in `REPORT.md`) is the
saturation point. What sets the ceiling:

1. **CPU cores.** Each request is CPU work (JSON parsing, scoring, JSON
   serialisation). Saturation near the number of logical cores is the normal
   case. Note the load generator and the server run on the same machine, so the
   generator takes cores too; `loadgen_ceiling.csv` and `client_cpu_pct` show
   how much.
2. **Snippet file reads.** Every returned result costs one file open, read and
   UTF-8/whitespace processing at query time (limit results per request). This
   is I/O and syscall-heavy; it is a likely dominant per-request cost for
   keyword search. Evidence: `mean_server_time_ms` is much higher than a pure
   index lookup would need, and throughput rises when `limit` is small. A
   snippet cache or storing snippets at build time would remove it.
3. **HTTP worker count.** The server uses `max(16, 2 x cores)` worker threads. When
   the number of clients exceeds the worker count, additional clients only queue,
   so throughput must stay flat and p95 grows; if throughput stays flat already
   below that point the limit is CPU or I/O, not the worker count.
4. **Lock contention on shards.** Searches take shared (reader) locks, which
   do not exclude each other, so readers should scale; the contended case is
   a search that runs while an index build is merging (not the case in this
   benchmark, which does not rebuild while measuring).
5. **JSON serialisation and the response size**: the results are small, so this
   is a minor part compared with file reads; it grows with `limit`.

**With query expansion** (`expand=true`) the request blocks on one Ollama call of
hundreds of milliseconds to seconds, orders of magnitude longer than the
keyword search itself. Consequences visible in the tables: latency is almost
entirely `time_ms` of the LLM call (compare `mean_server_time_ms` with the
keyword-only value), and the throughput ratio keyword/LLM in `REPORT.md` is
large. A single Ollama instance generates one request at a time per model slot
(by default it processes few requests in parallel and queues the rest), so
throughput with expansion does not grow with the number of clients beyond the
Ollama parallelism; instead p50 grows roughly linearly with clients (queueing).
Also, a request in the HTTP worker blocks its thread for the whole call, so with
many clients the worker pool can be exhausted by waiting requests, which would
also delay keyword-only requests sent at the same time. Expansion calls that hit
the timeout (`OLLAMA_TIMEOUT_MS`) end as `expansion: "fallback"`; the run
labels (`llm` vs `fallback` counts) show whether the LLM was really in the path.
On a laptop without GPU a 1B-parameter model also competes with the server for
the same CPU cores.

**[TO COMPLETE after the local run]**: peak keyword-only throughput and the
client count where it saturates, the keyword/LLM ratio, and whether the LLM was
CPU-bound on the same cores as the server.

## 4. How to tell the causes apart (checklist)

| Observation in the produced tables / charts | Points to |
|---|---|
| Karp-Flatt e(p) constant for p = 2..max | real serial fraction (traversal, finalise) |
| e(p) rising with p, efficiency drops smoothly | memory bandwidth, lock or queue contention |
| Efficiency collapses after the physical core count | SMT / hardware limit |
| Curve flat at a value below the core count | Docker CPU limit, or I/O bound |
| Large std / min-max spread, first repeats slower | disk / page cache effects |
| Keyword throughput flat while `client_cpu_pct` is high | load generator limit (invalid point) |
| Keyword throughput flat, latency linear in clients | server saturated (CPU, snippet I/O) |
| Health-check req/s much higher than search req/s | client is not the bottleneck |
| `mean_server_time_ms` close to p50 | time is spent in the server, not in network or queue |
| p50 far above `mean_server_time_ms` | queueing before the handler (worker count) |
| With expansion: `expansion_llm` low, `fallback` high | Ollama not ready or too slow: result describes timeouts |

## 5. Improvement ideas

Index build:

- Walk the directory in a separate producer thread (or in parallel by topic
  folder) so that submission overlaps with processing and the serial part
  shrinks; batch several documents per task if queue contention shows.
- Read files with `mmap` or large aligned buffers and avoid copies; tokenise
  from a `string_view`.
- Use a scalable allocator (mimalloc / jemalloc) or per-thread arenas for the
  temporary maps; reserve hash-map capacity.
- Build per-thread partial indexes and merge them in a final parallel step
  (by shard) instead of merging every document into the shared shards.
- Increase the shard count or use per-shard buffering of postings to shorten
  critical sections if the Karp-Flatt trend shows contention.
- Pin threads to physical cores and cap the default `threads` at the physical
  core count.

Search:

- Cache snippets (or store a short preview at build time) so the hot path does
  no file I/O; cache full results for repeated queries.
- Early termination (top-k with block-max / WAND-style scoring) for very common
  terms.
- Cache query expansions (LRU keyed by the normalised query) so repeated queries
  skip the LLM; use a shorter timeout, a smaller model, or run expansion
  asynchronously (WebSocket already streams `query_expansion` before the keyword
  results).
- Run Ollama with `OLLAMA_NUM_PARALLEL` > 1 or a GPU; separate the HTTP workers
  that wait for the LLM from those that serve keyword requests, so slow
  expansions cannot starve cheap searches.
