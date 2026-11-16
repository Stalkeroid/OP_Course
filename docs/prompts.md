# Prompt log

Every delegation prompt sent by the orchestrator (Claude Fable) to a subagent,
verbatim, plus the LLM prompt used for query expansion. Required for the
explanatory note.

Delivery note: prompt 1 was passed to the subagent inline. From prompt 2 on, the
full task text is written here first and the subagent is started with a one-line
bootstrap ("read section N of docs/prompts.md and execute it"), so the text below
is exactly what the subagent worked from.

### 1. api-contract — write docs/API.md
Date: 2026-11-16
Model: opus
Prompt:

```
You are the `api-contract` agent for the repository at /home/claude/op_course (a C++20 coursework project: parallel search engine with LLM query expansion). Work only in English.

FIRST read these files, in this order, and follow them:
1. /home/claude/op_course/.claude/agents/api-contract.md — your role definition and the full list of what the document must contain. Treat its body as your instructions.
2. /home/claude/op_course/CLAUDE.md — project rules, fixed technical decisions, repository layout.
3. The UML diagrams in /home/claude/op_course/docs/uml/ (class-diagram.png, use-case-diagram.png, sequence-streaming-search.png) — view them with the Read tool; keep names consistent with them where the contract mentions components.

GOAL
Create /home/claude/op_course/docs/API.md — the single source of truth for every REST endpoint and WebSocket message. Backend, CLI, Web UI, README and an independent verifier will all be checked against this file field by field, so it must be precise and unambiguous.

MANDATORY SHAPES (fixed by the university assignment; an automated grader checks them — do NOT rename, retype or restructure anything here; you may only ADD optional fields):
- GET /api/health → 200 { "status": "ok", "ollama": true|false }
- POST /api/index, body { "threads": 4 } → { "status": "ok", "documents_indexed": 2000, "time_ms": 1234 }
- POST /api/search, body { "query": "text", "mode": "keyword" } → { "results": [ { "doc_id": "...", "score": 0.95, "snippet": "..." } ], "time_ms": 56 }
- GET /api/stats → { "documents": 2000, "index_build_time_ms": 1234, "threads_used": 4 }
- WebSocket /ws/search: client sends { "query": "search text" }; server sends one JSON text message per stage, in this order:
  { "stage": "query_expansion", "data": { "original": "...", "expanded_terms": [...] } }
  { "stage": "keyword_search", "data": { "results": [...], "time_ms": 12 } }
  { "stage": "done" }
Pipeline for both POST /api/search and the WebSocket: query expansion through Ollama /api/generate (keywords + synonyms) → keyword search in the inverted index.

CONTEXT YOU NEED
- Server listens on port 8080 inside Docker Compose; Ollama is a separate compose service. The server must stay healthy (health 200, "ollama": false) while Ollama or its model is not ready, and search must still work then (fallback: expanded_terms = tokens of the original query).
- Dataset: mounted read-only at /data/dataset (env DATASET_PATH), one UTF-8 .txt file per document, >= 10 000 English documents, >= 1 GB total. doc_id = file path relative to the dataset root. POST /api/index indexes that directory; the dataset path is NOT a request parameter in the mandatory shape (you may add an optional one only if you see a strong reason; default must be DATASET_PATH).
- Index construction uses a custom thread pool; `threads` controls the worker count. The grader calls POST /api/index repeatedly with different `threads` values and expects build time to decrease, so re-indexing must be allowed and must fully rebuild. Searches may run concurrently with a rebuild (old index stays served until the new one is swapped in).
- The grader may send requests with only the mandatory fields; every optional field you add needs a default. Decide what happens when `threads` is omitted (suggest: default = hardware concurrency) and when `mode` is omitted (suggest: default "keyword").
- Benchmarks need a way to measure keyword search throughput without the LLM dominating latency. Consider an optional boolean request field in POST /api/search (e.g. "expand", default true) and, if useful, optional response fields such as "expanded_terms". Keep the mandatory response shape intact.
- Scoring: define it exactly (recommended TF-IDF summed over query terms, normalised so the top result's score lies in (0, 1]; state the formula), result ordering (score desc, tie-break by doc_id asc), default and maximum `limit`.
- Snippet: define length and how it is produced (window around the first matched term, whitespace collapsed, valid UTF-8).
- Tokenisation rules that affect observable behaviour (lowercasing, split on non-alphanumerics, stop words if any) — state them, since they determine which queries match.

REQUIRED CONTENT (see your role file for the full list): for every endpoint and WS message — field tables (name, JSON type, C++ type, required/optional, constraints, default), full request/response examples, curl / websocat examples, one uniform error shape with exact HTTP status codes per situation (malformed JSON, missing/wrong-typed field, threads out of range, unknown mode, empty query, search before the index is built, index build already running, dataset directory missing/empty, internal error), WebSocket rules (message ordering guarantee, error stage shape, behaviour on invalid JSON, whether the connection stays open for further queries — pick one and state it, behaviour when the index is not built), a dataset section, and a short "Conformance checklist" at the end that a verifier can walk through.

CONSTRAINTS
- Write only docs/API.md. Do not create or modify any other file. Do NOT run git commit.
- Be decisive: where the assignment is silent, make a concrete decision and write it as a rule, not as an option. No "TBD".
- Everything must be implementable in C++20 with Crow + nlohmann/json without heroics.
- Keep it well structured Markdown; aim for completeness over brevity, but no filler.

REPORT BACK (in English, concise)
1. The list of sections in the file.
2. Every decision you made that is not dictated by the assignment (optional fields, status codes, limits, scoring formula, WS connection lifetime, etc.), one line each.
3. Open questions or risks the project owner should decide on.
```

Outcome: accepted

### 2. concurrency-core — thread pool, inverted index, tokenizer, builder, tests
Date: 2026-11-16
Model: opus
Prompt:

```
You are the `concurrency-core` agent for the repository at /home/claude/op_course
(C++20 coursework: parallel search engine with LLM query expansion). English only.

READ FIRST, in this order:
1. .claude/agents/concurrency-core.md — your role; treat its body as instructions.
2. CLAUDE.md — project rules ("Fixed technical decisions" are binding).
3. docs/API.md — the contract. Sections 2.3, 2.4, 4 (all), 7.3, 7.4, 9 and 11
   define behaviour you implement. Where your role file and docs/API.md differ
   (e.g. unit of work submitted to the pool), docs/API.md wins.
4. docs/uml/class-diagram.png — class, method and field names to keep.

GOAL
Deliver the parallel core as a static library `search_core` with tests. No HTTP,
no JSON, no Ollama here: another agent builds the server on top of your headers.

FILES YOU OWN (create them; nobody else is editing them now)
- CMakeLists.txt (repo root): project, C++20, options BUILD_TESTING (default ON)
  and ENABLE_TSAN (default OFF), `add_subdirectory(server)`, and
  `add_subdirectory(cli)` only `if(EXISTS .../cli/CMakeLists.txt)`.
- .clang-format (BasedOnStyle: Google).
- server/CMakeLists.txt: target `search_core` (static lib) and, when
  BUILD_TESTING, target `core_tests` (GoogleTest via FetchContent, pinned tag,
  registered with CTest). Leave a clearly marked place where the next agent adds
  the `search_server` executable; do not fetch Crow/httplib/json yourself.
- server/include/search/*.h, server/src/*.cpp, server/tests/*.cpp.
Do NOT touch: docs/, docker-compose.yml, Dockerfile, dataset/, .gitignore, web/, cli/.
Another agent is working in parallel on Docker files in the same checkout; build
only inside /home/claude/op_course/build* directories (already git-ignored).

COMPONENTS
1. CustomThreadPool — own implementation (std::thread, std::mutex,
   std::condition_variable, task queue, isRunning). Constructor
   CustomThreadPool(numThreads), enqueueTask(task), a way to wait until all
   submitted tasks finished, shutdown(). Exceptions thrown by a task must not kill
   a worker; the first one must be observable by the caller of the wait function.
2. Tokenizer — exactly docs/API.md 4.1 (ASCII alnum runs, lowercase, length 2..64,
   the 33 stop words). One implementation used for documents and queries. It is on
   the hot path of the build: avoid per-token heap allocations where practical
   (std::string_view over the file buffer).
3. InvertedIndex — own concurrent structure, no library concurrent containers.
   Sharded by term hash, one std::shared_mutex per shard; a document is tokenised
   into a thread-local term->count map and then merged into the shards, so the
   lock is held only for the merge. Keep UML members/methods: indexMap, rwLock
   (per shard), documentCount, addDocument(docId, text), search(...), getStats().
   - Store doc ids once (uint32 internal id -> doc_id string table); postings as
     compact {uint32 doc, uint32 count}. Never keep document text in memory.
   - search takes the weighted query terms of docs/API.md 4.2 (term + weight) and
     a limit, and returns the top results ordered per 4.5 with scores per 4.4 and
     2.4 (rounding/clamping), plus total_matches. Use a bounded top-k selection,
     not a full sort of all matches. SearchResult keeps docId, score, snippet
     (snippet is filled later by the caller).
   - Searches must be safe concurrently with each other and with addDocument.
   - Result must be independent of thread count and insertion order (document
     numbering may differ between builds, observable results may not).
   - Memory target: a 2.1 GB dataset of ~20 000 documents (~100 KB each) must
     index within roughly 1.5 GB peak RSS. Mention measured numbers in the report.
4. Snippet builder — pure function implementing docs/API.md 4.6 exactly
   (UTF-8 decoding with U+FFFD, BOM strip, whitespace collapse, 60/200 code point
   window, "..." markers), given file bytes and the expanded terms.
5. IndexBuilder (or a method set) — implements docs/API.md 7.3, 7.4 and 11:
   recursive scan for *.txt, doc_id = relative generic path, unreadable files
   skipped and counted, a new CustomThreadPool(threads) per build, documents are
   the unit of work, the coordinating thread does not tokenise. Returns a
   std::shared_ptr<InvertedIndex> plus IndexStats (documents, indexBuildTimeMs,
   threadsUsed, uniqueTerms, filesSkipped) or a typed failure for
   dataset_not_found / dataset_empty. The atomic swap of the served index is the
   server agent's job, but your types must make it easy (immutable-after-build
   index behind shared_ptr<const InvertedIndex> is fine for searching).
   Use std::chrono::steady_clock for time_ms.

TESTS (server/tests, GoogleTest)
- Thread pool: all tasks run; wait works when tasks enqueue slowly; shutdown with
  pending tasks; exception in a task.
- Tokenizer: the three examples from docs/API.md 4.1, stop words, length limits,
  non-ASCII bytes as separators.
- Index: tiny corpus with hand-computed scores (check the formula numerically),
  ordering and tie-break by doc_id, limit, total_matches, weights 1.0 vs 0.5,
  no match -> empty.
- Snippet: window at start/middle/end of text, BOM, invalid UTF-8, multi-byte
  characters counted as code points, max 206 code points.
- Builder: temp directory with nested folders, non-.txt files ignored, empty file
  counted as a document, missing directory and empty directory failures.
- Determinism: build with 1 thread and with 8 threads -> identical search results
  for a set of queries.
- Stress: several writer threads calling addDocument while reader threads search.
All tests must pass in a normal build AND in a build with -DENABLE_TSAN=ON with
zero ThreadSanitizer reports. Run both yourself.

ALSO
- Naming: classes, methods and fields follow the UML (camelCase methods like
  addDocument, enqueueTask); clang-format Google style governs layout only. Run
  clang-format -i on every file you write.
- Short header comments explaining the locking scheme and why (the student must
  defend it orally). No long essays.
- Quick performance sanity check: generate a synthetic corpus of ~200 MB in /tmp
  (a throwaway script is fine, do not commit it), build with 1 and 2 threads
  (this machine has 2 cores), report times, speedup and peak RSS. You may add a
  tiny optional executable target for this (not part of `all`).
- The machine has 2 cores and 7 GB RAM: build with -j2.
- Do NOT run git add / git commit / git push.

REPORT BACK (concise, English)
1. Files created.
2. Public API for the server agent: headers, class/function signatures, and a
   10-line usage sketch (build an index, search with weighted terms, make a
   snippet, read stats).
3. Design decisions (shard count, locking, memory layout) in a few lines.
4. Test results: normal build and TSAN build (counts, pass/fail), exact commands.
5. Performance numbers from the sanity check.
6. Anything in docs/API.md you found ambiguous or could not implement as written.
```

Outcome: accepted (51 tests pass, also under ThreadSanitizer; rerun by the orchestrator)

### 3. devops — Docker Compose, Ollama services, dataset generator
Date: 2026-11-16
Model: sonnet
Prompt:

```
You are the `devops` agent for the repository at /home/claude/op_course
(C++20 coursework: parallel search engine with LLM query expansion). English only.

READ FIRST, in this order:
1. .claude/agents/devops.md — your role; treat its body as instructions.
2. CLAUDE.md — project rules ("Fixed technical decisions" are binding).
3. docs/API.md — sections 6 (health), 11 (dataset) and 12 (environment
   variables). Variable names and defaults there are the contract; use them
   exactly.

GOAL
On a machine with only Docker installed:
`git clone … && cd … && docker compose up` must start the whole system with no
manual step: Ollama as a separate service, its model pulled automatically, the
dataset generated automatically, the search server healthy on port 8080.

FILES YOU OWN
- docker-compose.yml, Dockerfile (repo root, multi-stage), .dockerignore
- dataset/ (generator + a short dataset/README.md)
- .gitignore (extend the existing one; keep what is there)
Do NOT touch: CMakeLists.txt, server/, cli/, web/, docs/, .clang-format.
Another agent is writing the C++ core in the same checkout right now.

BUILD FACTS (decided; the C++ sources are still being written by other agents)
- Root CMakeLists.txt, C++20, dependencies via FetchContent (needs git and
  network at build time), Ninja available.
- Build command for the image:
  cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=OFF
  cmake --build build --target search_server search_cli
  Binaries appear somewhere under build/ — locate them robustly (e.g. `find` or
  `cmake --install` is NOT available; copy by `find build -name search_server`).
- Runtime image must contain /app/search_server, /app/search_cli and /app/web
  (copied from ./web; the directory may be empty today, the Dockerfile must still
  build once web/ exists — do not create web content yourself, at most a
  .gitkeep). Server starts as `/app/search_server` and reads its config from the
  environment (docs/API.md 12). It listens on 0.0.0.0:8080.
- Compiler: needs full C++20 (GCC 13 or newer). Use an image available for both
  amd64 and arm64.
- Layer caching: copy CMake files and fetch/build dependencies as early as you
  reasonably can, but correctness beats cleverness.

SERVICES
- ollama: official image, named volume for models, healthcheck (the image has no
  curl; use `ollama list`). Do not publish its port by default, or publish it on
  11434 only if harmless — decide and comment.
- ollama-init: one-shot, same image, waits for ollama healthy, pulls
  ${OLLAMA_MODEL} (default exactly as in docs/API.md 12), exits 0. Idempotent.
  A failed pull (no internet) must not prevent the server from starting.
- dataset-init: one-shot. If the dataset volume already holds a complete dataset
  (marker file written last), exit immediately; otherwise generate. Deterministic
  (fixed seed). Defaults: DATASET_SIZE_MB=2100, DATASET_DOCS=20000 (the assignment
  demands >= 2 GB and >= 10 000 English documents). Both overridable from the
  host environment, e.g. `DATASET_SIZE_MB=100 docker compose up` for development.
  If the marker says a different size/doc count than requested, regenerate.
- server: built from the root Dockerfile; dataset volume mounted read-only at
  /data/dataset; depends_on dataset-init (service_completed_successfully) and
  ollama (service_started is enough — the server must be healthy while Ollama or
  the model is not ready; never wait for ollama-init). Port 8080:8080.
  Healthcheck on GET /api/health (install curl in the runtime image).
  Set every variable from docs/API.md 12 explicitly with its default.
- No .env file may be required. `docker compose up` without flags must work.
- The CLI will be invoked by a root script as
  `docker compose exec -T server /app/search_cli …` (another agent writes that
  script); just make sure the binary is in the runtime image.

DATASET GENERATOR (dataset/)
- Python 3, standard library only, runnable both in the dataset-init container
  (python:3.x-slim) and directly:
  python3 dataset/generate.py --out DIR --size-mb N --docs M --seed S
- Output layout per docs/API.md 11: one UTF-8 .txt file per document, lowercase
  .txt extension, grouped into topic subdirectories (e.g. science/doc_000123.txt),
  so doc_id looks like "science/doc_000123.txt".
- Content must be searchable in a meaningful way, not random letters:
  ~15–20 topics (science, medicine, history, sports, technology, finance, travel,
  cooking, law, music, environment, space, education, …), each with its own
  vocabulary of real English words (a few hundred per topic) plus a large shared
  general vocabulary; words drawn with a Zipf-like distribution; sentences with
  capitalisation and punctuation; paragraphs separated by blank lines; a title
  line. Synonym-rich vocabularies (car/automobile/vehicle, doctor/physician, …)
  help the LLM query-expansion demo. Document sizes vary around the mean
  (size-mb / docs), e.g. +-50%. Total vocabulary in the tens of thousands of
  distinct words is ideal (it can include generated compound/derived forms and
  numbers), embedded in the script or a data file in dataset/ — no downloads.
- Speed: 2100 MB must generate in a few minutes on a laptop. Use multiprocessing,
  build text from pre-sampled word arrays (random.choices in bulk, str.join),
  write large chunks. Report measured throughput (MB/s) on this machine (2 cores).
- Writes a marker file (e.g. .complete with size/docs/seed) as the last step;
  the server ignores it because it is not *.txt.

VERIFY
- Run the generator directly with --size-mb 50 --docs 500 into /tmp and report
  file count, total size, a sample of one document's first lines, throughput.
- The Docker daemon is not running in this sandbox. Validate what you can
  without it (`docker compose config` if the compose plugin is present; careful
  review of the Dockerfile). Say explicitly what could not be run.
- Do NOT run git add / git commit / git push.

REPORT BACK (concise, English)
1. Files created/changed.
2. Services, start order, volumes, published ports, env vars with defaults.
3. Base images chosen (build and runtime) and why.
4. Generator: measured throughput, estimated time for 2100 MB, vocabulary size.
5. What was verified and how; what was not.
6. First-start expectations for the user (downloads, disk space, rough time).
```

Outcome: failed (subagent stopped by an API output filter before writing any file; rerun as prompt 4)

### 4. devops — rerun of prompt 3 with amendments
Date: 2026-11-16
Model: sonnet
Prompt:

```
Execute the assignment in section "### 3. devops — Docker Compose, Ollama
services, dataset generator" of docs/prompts.md, with these amendments, which
override section 3 where they differ. A previous attempt was cut off while it
was emitting a very large word list, so:

1. Work in small steps and write files early. Order: docker-compose.yml,
   Dockerfile, .dockerignore, .gitignore first; then the generator code; then
   vocabulary data. Never produce one huge output: no single file write or edit
   larger than about 150 lines.
2. Vocabulary: keep it modest and neutral.
   - 14–16 everyday topics only: science, technology, space, environment,
     travel, cooking, sports, music, education, finance, architecture,
     gardening, transport, weather, art, business. No medicine, law, crime,
     war, politics, religion or other sensitive subject matter.
   - About 80–120 plain common English words per topic, stored as short
     data files dataset/vocab/<topic>.txt (one word per line), written one file
     at a time. Include natural synonym groups (car, automobile, vehicle; fast,
     quick, rapid; …).
   - One shared general vocabulary file of roughly 600–800 common words
     (written in 3–4 chunks appended to the same file).
   - Reach a larger distinct-term count programmatically in generate.py:
     derived forms (plural, -ing, -ed, -er, -ly where plausible), compounds of
     two topic words joined by a hyphen or space, numbers, years, simple codes
     like "model x42". Do not hand-write long lists.
3. Everything else in section 3 stays: services, env vars from docs/API.md 12,
   marker file, multiprocessing, verification run with --size-mb 50 --docs 500,
   no git commands, same report format.
```

Outcome: accepted (Docker files reviewed, generator run locally; image build not runnable in the sandbox)
