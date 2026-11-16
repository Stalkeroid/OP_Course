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

### 5. backend-api — HTTP/WebSocket server, pipeline service, Ollama client
Date: 2026-11-16
Model: sonnet
Prompt:

```
You are the `backend-api` agent for the repository at /home/claude/op_course
(C++20 coursework: parallel search engine with LLM query expansion). English only.

READ FIRST, in this order:
1. .claude/agents/backend-api.md — your role; treat its body as instructions.
2. CLAUDE.md — project rules ("Fixed technical decisions" are binding).
3. docs/API.md — read ALL of it carefully. It is the contract; an independent
   verifier will compare your server with it field by field (names, JSON types,
   status codes, error codes, validation order, message order, defaults).
4. server/include/search/*.h — the finished core library `search_core`
   (CustomThreadPool, tokenizer + buildQueryTerms/originalQueryTokens,
   InvertedIndex, makeSnippetFromFile, IndexBuilder::buildIndex). Use it; do not
   reimplement tokenisation, scoring or snippets.
5. docs/uml/class-diagram.png and sequence-streaming-search.png — keep class and
   method names: ApiServer (handleHealthCheck, handleIndexPost, handleSearchPost,
   handleWebSocket), SearchPipelineService (buildIndex, performKeywordSearch,
   performStreamingSearch), OllamaClient (expandQuery, checkHealth).

GOAL
A working `search_server` executable implementing every endpoint and the
WebSocket of docs/API.md, plus tests.

FILES YOU OWN
- server/CMakeLists.txt: add your part in the marked section (keep the existing
  targets working). Fetch Crow, nlohmann/json and cpp-httplib with FetchContent
  at pinned release tags (check the current tags with `git ls-remote --tags`).
  Crow needs standalone Asio: fetch it with FetchContent too (header-only) so the
  Docker build needs no extra system package. No OpenSSL, no zlib features.
  Put your code in a static library (e.g. `search_service`) linked by the
  `search_server` executable and by your tests.
- server/include/search/ and server/src/: api_server, search_pipeline_service,
  ollama_client, config, main.cpp (new files only).
- server/tests/: new test files; add them to a test target (extend core_tests or
  add `service_tests`, registered with CTest).
Do NOT touch: docs/, docker-compose.yml, dataset/, web/, cli/, .gitignore.
Do not modify the existing core files; if you find a bug or a missing accessor
there, make the smallest possible change and list it in your report. If the
Dockerfile build stage would need a change because of your dependencies, say so
in the report instead of editing it.

REQUIREMENTS BEYOND THE CONTRACT TEXT
- Configuration only from the environment variables of docs/API.md 12, with the
  defaults given there. Listen on 0.0.0.0:PORT.
- Concurrency:
  * HTTP handlers run on Crow's worker threads; set Crow concurrency to
    max(16, 2 x hardware_concurrency) because /api/index (synchronous build) and
    searches with query expansion block a worker for a long time, and
    /api/health and /api/stats must still answer meanwhile.
  * No global mutex around searches. The served index is a
    std::shared_ptr<const InvertedIndex> swapped under a small mutex (or
    std::atomic<std::shared_ptr>); a search copies the pointer and works on its
    snapshot. One build at a time via an atomic flag -> 409 index_in_progress.
  * WebSocket: never run the pipeline on Crow's I/O callback thread. Use a
    CustomThreadPool (our own class, from search_core) owned by the service for
    WebSocket queries. Per connection: FIFO queue, at most one query running,
    messages of different queries never interleaved, different connections in
    parallel. A connection may close while its query is queued or running: keep
    per-connection state in a shared_ptr with a "closed" flag so that no send
    happens on a dead connection and nothing is freed while a worker uses it.
- performStreamingSearch must send `query_expansion` as soon as expansion is
  done, before the keyword search starts (docs/API.md 10.4).
- OllamaClient: cpp-httplib; connect/read timeouts from OLLAMA_TIMEOUT_MS;
  request and response handling exactly as docs/API.md 5; health check per
  docs/API.md 6 (GET /api/tags, 2000 ms, result cached for at most 5 s, cache
  shared between threads safely). Keep the prompt text in one named constant.
  Any failure -> fallback, never an error to the client.
- JSON: nlohmann/json for parsing and serialising everywhere (not Crow's json).
  Validate strictly per docs/API.md 2 and 3 (integers must be JSON integers,
  null = absent, unknown fields ignored). Serialise `score` per 2.4.
  Response header Content-Type: application/json for all /api/* responses.
- Error shape for unknown /api/* paths (404 not_found) and wrong methods
  (405 method_not_allowed): implement with a catch-all route; verify with curl
  that the body is our error JSON, not Crow's default page.
- Static files: GET / serves WEB_ROOT/index.html; other non-/api, non-/ws paths
  serve files under WEB_ROOT with correct Content-Type (html, css, js, svg, png,
  ico, json); reject path traversal; 404 plain response if missing. web/ is
  still empty — test with a temporary directory.
- Clean shutdown on SIGINT/SIGTERM (docker stop must not hang for 10 s).
- One log line to stdout per request is enough (method, path, status, ms);
  startup line with the effective configuration.

TESTS
- Unit tests for request validation (every error code of docs/API.md 3.2 that
  applies, including validation order) and for Ollama response parsing (valid
  {"terms":[...]}, extra text, wrong types, empty) — structure the code so these
  are testable without sockets.
- A test with a fake Ollama (cpp-httplib server on a random local port inside
  the test): "llm" path, timeout -> "fallback", connection refused -> "fallback",
  health true/false depending on /api/tags content.
- A pipeline test: search works on the old index while a rebuild is running and
  the swap happens (use a small temp dataset); second concurrent build -> 409
  equivalent at service level.
- All existing core tests must still pass. Build with -j2 (2 cores, 7 GB RAM).
  Run clang-format -i on your files.
- Run your new tests once under -DENABLE_TSAN=ON (build dir build/tsan) and
  report the result.

END-TO-END SMOKE TEST (do it and paste trimmed outputs in the report)
A dataset already exists: /tmp/ds100 (2000 .txt documents, ~105 MB; regenerate
with `python3 dataset/generate.py --out /tmp/ds100 --size-mb 100 --docs 2000`
if missing). Real Ollama is NOT available in this sandbox: write a small fake
Ollama (Python stdlib http.server, kept outside the repo in /tmp) that answers
/api/tags and /api/generate like the real one. Then, with the server running:
1. health with Ollama down ("ollama": false) and with the fake up (true);
2. search before index -> 503 index_not_built;
3. POST /api/index with threads 1 and 2 -> times; GET /api/stats;
4. POST /api/search (expansion "llm" with the fake, "fallback" without,
   "disabled" with expand:false), limit, a no-match query;
5. each 400 error once; 404 and 405 under /api/; 409 by starting two builds;
6. WebSocket /ws/search with a Python client (pip install websockets
   --break-system-packages is allowed, or a raw-socket script): the three-message
   sequence, a second query on the same connection, the error stage for invalid
   JSON and empty query, two queries sent back-to-back arrive un-interleaved.
7. kill -TERM and confirm fast exit.
Do NOT run git add / git commit / git push.

REPORT BACK (concise, English)
1. Files created/changed (flag any change to core files or needed Dockerfile change).
2. Dependency versions pinned.
3. The exact Ollama prompt constant (verbatim) and the request JSON you send.
4. Test results (normal + TSAN), exact commands.
5. Smoke-test evidence: commands and trimmed outputs for items 1–7.
6. Deviations from docs/API.md or places where it was ambiguous, with your choice.
```

Outcome: accepted (125 tests pass; core and service suites clean under ThreadSanitizer; smoke test rerun by the orchestrator)

## Query expansion prompt (sent to Ollama by the server)

Constant `kExpansionPromptTemplate` in `server/include/search/ollama_client.h`;
`{query}` is replaced by the user's query. Request: `POST {OLLAMA_URL}/api/generate`
with `"stream": false`, `"format": "json"`, `"options": {"temperature": 0}`.

```
You are a search query expansion engine. Given a search query, list keywords and synonyms that are closely related to it.
Rules:
- English only.
- Each term is a single word or a very short phrase.
- At most 10 terms, most relevant first.
- Do not repeat the words of the query itself.
- No explanations, no extra text.
Answer only with a JSON object of this exact form: {"terms": ["term1", "term2"]}
Query: {query}
```

### 6. clients — CLI `./search` and Web UI
Date: 2026-11-16
Model: sonnet
Prompt:

```
Repository: /home/claude/op_course (C++20 coursework: parallel search engine
with LLM query expansion). English only. Your role file was loaded as your
system prompt; this is the concrete task.

READ FIRST
1. CLAUDE.md — project rules ("Fixed technical decisions" are binding).
2. docs/API.md — ALL of it. Use only the shapes, defaults and error codes
   defined there (REST sections 3, 6–9; WebSocket section 10).
3. CMakeLists.txt (root) and server/CMakeLists.txt — the root file already adds
   `cli/` when cli/CMakeLists.txt exists; server/ defines the INTERFACE target
   `third_party_headers` (cpp-httplib + nlohmann/json include paths) that you
   link against. Do not fetch the libraries again.
4. Dockerfile and docker-compose.yml — the image builds the targets
   `search_server search_cli` with -DBUILD_TESTING=OFF and ships
   /app/search_cli and /app/web. Do not edit these two files; if something in
   them must change for your work, say so in the report.
The server is finished: target `search_server`; run it locally with
  PORT=18080 DATASET_PATH=/tmp/ds100 WEB_ROOT=$PWD/web \
  OLLAMA_URL=http://127.0.0.1:11434 build/server/search_server
(/tmp/ds100 = 2000 generated documents; recreate with
`python3 dataset/generate.py --out /tmp/ds100 --size-mb 100 --docs 2000`).
Real Ollama is not available here; a fake one may exist at
/tmp/smoke/fake_ollama.py (Python stdlib; if missing, write your own in /tmp
that answers GET /api/tags and POST /api/generate like Ollama). Words that occur
in the dataset: recipe, oven, kitchen, engine, planet, orbit, guitar, melody,
garden, invoice, budget, storm, bridge, museum.

FILES YOU OWN
- cli/CMakeLists.txt, cli/src/*.cpp (+ headers), optional cli/tests/
- search (bash, executable bit set) and search.cmd (Windows) at the repo root
- web/index.html, web/app.js, web/style.css (delete web/.gitkeep)
Do NOT touch server/, docs/, dataset/, Dockerfile, docker-compose.yml.

PART A — CLI (C++20, target `search_cli`)
- HTTP via cpp-httplib, JSON via nlohmann/json, REST only.
- Usage: search_cli [options] "query words"   (several positional words are
  joined with spaces)
  --host H (default localhost)   --port P (default: env PORT, else 8080)
  --limit N (1..100)             --no-expand   (sends "expand": false)
  --json    print the raw response body, nothing else
  --index N run POST /api/index with N threads (no N -> omit the field)
  --stats   GET /api/stats       --health  GET /api/health
  --timeout S (default 120; index builds may take minutes: use a much longer
  read timeout for --index, e.g. 3600 s)
  --help
- Default search output, plain ASCII, readable in an 80–100 column terminal:
  a header line with the query, the expanded terms and their source
  (llm/fallback/disabled), then one block per result: rank, score with 2
  decimals, doc_id, and the snippet wrapped/truncated to the terminal width;
  footer with total_matches and timings. "No results." when empty.
- Errors: print "error: <code>: <message>" to stderr using the contract's error
  shape; a clear message when the server is unreachable; for index_not_built
  add the hint to run `./search --index 4`. Exit codes: 0 ok, 1 usage error,
  2 server/API/network error.
- Unit tests are optional; if you add them, register with CTest under
  BUILD_TESTING only. The target must build with -DBUILD_TESTING=OFF.
- clang-format -i (Google style, .clang-format at the root). Naming: camelCase
  functions like the rest of the project.

PART B — root wrapper scripts
- `./search "query"` must work right after `docker compose up` from a fresh
  clone, with nothing compiled on the host:
  default path = `docker compose exec -T server /app/search_cli "$@"`, run from
  the script's own directory (so it works from any cwd).
- If the environment variable SEARCH_CLI_LOCAL is set to a binary path (or a
  locally built build/cli/search_cli exists and Docker is not available), run
  that binary instead — this is how you test here, since the Docker daemon is
  not running in this sandbox.
- Friendly message if neither Docker Compose nor a local binary is usable, or
  if the server container is not running.
- search.cmd: same default behaviour for Windows cmd/PowerShell.

PART C — Web UI (plain HTML + CSS + vanilla JS, no build step, no CDN, no
external fonts; everything served from WEB_ROOT by the server at /)
- Search form (query input, limit, "use LLM expansion" checkbox, submit).
- On submit: use one WebSocket to ws(s)://<location.host>/ws/search (open it
  lazily, reuse it for later queries, reconnect if it was closed), send the
  client message of docs/API.md 10.2.
- Pipeline visualisation: three steps "Query expansion" -> "Keyword search" ->
  "Done", each pending / active (animated) / complete / failed, driven only by
  the incoming stage messages; show per-stage time_ms; show expanded terms as
  chips, visually separating the original terms from the LLM-added ones, and
  the expansion source (llm / fallback / disabled).
- Handle the `error` stage per docs/API.md 10.3.4/10.4 (mark the current step
  failed, show code + message inline; for index_not_built point to the admin
  panel).
- Results: rank, doc_id, score (number + small bar), snippet with the expanded
  terms highlighted (escape HTML first — snippets are untrusted text; build DOM
  nodes or escape, never innerHTML with raw data), total_matches.
- Admin panel (REST): health (server, Ollama, index ready, indexing), stats
  (documents, unique terms, last build time, threads), "Build index" with a
  threads input; show progress state while the synchronous request is running
  and the result or the error (e.g. 409 index_in_progress) inline. Refresh
  health/stats on load, after a build, and every few seconds while a build runs.
- No alert(); usable keyboard-only; works at 360 px width; a restrained, clean
  look (system font stack, one accent colour, light and dark via
  prefers-color-scheme). UI text in English.

VERIFY (and report evidence)
- Build: cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j2
  (2 cores) — and a separate configure with -DBUILD_TESTING=OFF in
  build/notest building exactly `--target search_server search_cli`.
- CLI against the running server: --health, search before index (exit code 2 +
  hint), --index 2, --stats, a search with and without --no-expand, --json,
  --limit, a no-result query, server down (exit code 2), bad usage (exit 1).
  Also through the wrapper: SEARCH_CLI_LOCAL=build/cli/search_cli ./search "recipe".
- Web UI: there is no browser in this sandbox. Do what is possible:
  `node --check web/app.js` if node exists; fetch / and the assets through the
  server with curl (status + Content-Type); and exercise the exact messages
  your JS sends against /ws/search with a small script (Python `websockets`
  is installed, or Node's built-in WebSocket) to confirm the server accepts
  them. State plainly that rendering was not checked in a real browser.
- Do NOT run git add / git commit / git push.

REPORT BACK (concise, English)
1. Files created/changed.
2. CLI: --help output and two sample runs (trimmed), exit codes observed.
3. How ./search and search.cmd choose between Docker and a local binary.
4. Web UI: structure, which states exist, what was verified and what was not.
5. Anything in Dockerfile / docker-compose.yml / docs/API.md that needs a
   change or was ambiguous.
```

Outcome: accepted (CLI checked against a live server; Web UI logic exercised in jsdom by the orchestrator; not viewed in a real browser)

### 7. benchmark — scalability scripts, charts, analysis draft
Date: 2026-11-16
Model: sonnet
Prompt:

```
Repository: /home/claude/op_course (C++20 coursework: parallel search engine
with LLM query expansion). English only. Your role file was loaded as your
system prompt; this is the concrete task.

READ FIRST
1. CLAUDE.md — project rules.
2. docs/API.md — sections 6–9 (REST shapes), 4 (matching/scoring), 7.3/7.4
   (what the build time measures, full rebuild per call) and the `expand` and
   `limit` request fields of POST /api/search.
3. server/include/search/inverted_index.h, index_builder.h,
   search_pipeline_service.h and server/src/api_server.cpp (header comments and
   structure only) — so the analysis describes the real architecture: 64 index
   shards with one shared_mutex each, one pool task per document, per-thread
   term counting then a short merge under the shard lock, snippets read from
   disk at query time, HTTP worker count max(16, 2 x cores), WebSocket queries
   on our own thread pool, blocking Ollama call inside the search request.
4. docker-compose.yml — the stack the user will measure (server on port 8080).

SITUATION
The real measurements will be taken by the student on a laptop after
`docker compose up` (2.1 GB dataset, 20 000 documents, real Ollama). This
sandbox has 2 cores, no Docker daemon and no Ollama, so here you build and
validate the tooling on a small dataset. Never write sandbox numbers into the
repository as if they were results.

FILES YOU OWN: everything under bench/. You may also ADD one service named
`bench` with `profiles: ["bench"]` at the end of the services in
docker-compose.yml (do not change anything else there) so that the student can
run everything without Python on the host:
  docker compose --profile bench run --rm bench
(image python:3.12-slim, mounts ./bench, installs bench/requirements.txt at
start, talks to http://server:8080, writes into bench/results and bench/charts
on the host). Do NOT touch server/, cli/, web/, docs/, dataset/, Dockerfile.
Another agent is working in cli/ and web/ and rebuilding in build/ right now:
copy build/server/search_server to /tmp/bench/search_server once and run that
copy on port 18090; do not run cmake yourself.

DELIVERABLES
1. bench/run_all.py — one entry point, options: --host, --port, --quick,
   --out bench/results, --threads LIST, --clients LIST, --repeats N,
   --duration S, --skip-index, --skip-load, --skip-llm. Steps:
   a. Environment record (bench/results/environment.json): server health and
      stats, CPU count seen by the client, platform, timestamp, parameters.
      Ask the user in the README to add CPU model / RAM / Docker limits by hand.
   b. Index build time vs threads: default thread list 1,2,3,4,6,8,12,16 capped
      at 2 x os.cpu_count(); one untimed warm-up build first (page cache), then
      --repeats (default 3) per point in an interleaved order (so drift does not
      favour one thread count); use the server-reported time_ms; client timeout
      generous (1 h). CSV: threads, run, time_ms, documents_indexed.
   c. Throughput vs concurrent clients, keyword search only ("expand": false):
      clients 1,2,4,8,16,32,64; fixed duration per level (default 15 s, quick
      3 s) after a 2 s warm-up; closed-loop clients with keep-alive connections;
      queries drawn round-robin from a fixed list of words that exist in the
      dataset (read the vocab files in dataset/vocab/ to pick 30–40 words across
      topics; do not hard-code a list that may not match). The load generator
      must not be the bottleneck: spread clients over several processes
      (multiprocessing), standard library http.client is fine. CSV: clients,
      requests, errors, duration_s, throughput_rps, p50_ms, p95_ms, p99_ms,
      mean_server_time_ms (from the response's time_ms).
   d. Same with query expansion ("expand": true) at clients 1,2,4,8 and a short
      duration, recording how many responses had expansion "llm" vs "fallback"
      (if Ollama is not ready the run is still valid but must be labelled so).
   e. Search quality table: 10 fixed queries built from the vocab (single words
      and 2–3 word queries, one that cannot match), expected = the topic folder
      the top result should come from (derived from which vocab file holds the
      word), actual = top 3 doc_ids and scores, PASS/FAIL. Markdown + CSV.
2. bench/plot.py (called by run_all.py, also runnable alone on existing CSVs):
   matplotlib, Agg backend, PNG 150 dpi into bench/charts/:
   index_time_vs_threads.png (mean with min–max error bars),
   speedup_efficiency.png (measured speedup vs ideal line, efficiency on a
   second axis), throughput_vs_clients.png (keyword-only and with-LLM series,
   log-2 x axis), latency_vs_clients.png (p50/p95). Axis labels with units,
   titles, legends, grid. Follow sensible chart practice: no 3D, no clutter.
3. bench/report.py (called by run_all.py): writes bench/results/REPORT.md from
   the CSVs only — tables of mean/std per thread count, speedup S(p)=T1/Tp,
   efficiency E(p)=S(p)/p, the serial fraction estimated from Amdahl's law per
   point (Karp–Flatt metric), best thread count, peak throughput and the client
   count where it saturates, LLM vs keyword-only throughput ratio. Every number
   computed, none typed by hand.
4. bench/ANALYSIS.md — the qualitative bottleneck analysis, written for the
   explanatory note: what limits index-build scaling in THIS implementation
   (sequential directory walk and task submission on the coordinating thread,
   file reading / page cache and disk, memory bandwidth, shard lock contention
   during merges, allocator, hyper-threading, Docker CPU limits) and what
   limits search throughput (snippet file reads, HTTP worker count, JSON
   serialisation, the LLM call dominating when expansion is on, single Ollama
   instance serialising generations), how to tell these apart using the
   produced tables (e.g. Karp–Flatt trend, efficiency drop past physical
   cores), and concrete improvement ideas. No measured numbers in this file:
   refer to REPORT.md and the charts; mark clearly the two or three sentences
   the student must complete after the local run.
5. bench/README.md — how to run (Docker profile and plain Python/venv), options,
   expected duration, where outputs go, how to record the hardware.
6. bench/requirements.txt (matplotlib only, pinned major). bench/results/ and
   bench/charts/ keep a .gitkeep; outputs themselves ARE meant to be committed
   later by the student, so do not git-ignore them.
- Python: standard library + matplotlib only; format with black (install with
  pip --break-system-packages if missing) or keep black-compatible style.

VALIDATE HERE (sandbox sample only)
- Start /tmp/bench/search_server with PORT=18090 DATASET_PATH=/tmp/ds100
  (2000 documents; recreate with `python3 dataset/generate.py --out /tmp/ds100
  --size-mb 100 --docs 2000` if missing), OLLAMA_URL pointing at
  /tmp/smoke/fake_ollama.py if you start it (or at a closed port).
- Run `python3 bench/run_all.py --port 18090 --quick --out /tmp/bench/out`
  (charts to /tmp/bench/out too — make the chart directory follow --out or add
  --charts) and confirm all CSVs, the four PNGs, REPORT.md and the quality table
  are produced and sane. Look at the PNGs yourself (Read tool) to check labels
  and legibility. Nothing from this run may be left inside the repository.
- Do NOT run git add / git commit / git push.

REPORT BACK (concise, English)
1. Files created/changed (quote the compose service you added).
2. How to run, expected duration of a full run on the 2.1 GB dataset.
3. Sandbox sample numbers (clearly labelled as such) and what the charts look like.
4. Load-generator ceiling: evidence that the client is not the bottleneck.
5. Limitations and anything you could not validate.
```

Outcome: accepted (scripts validated on a 2000-document sample; real measurements are taken by the student locally)

### 8. concurrency-core — early-exit snippet extraction
Date: 2026-11-16
Model: opus
Prompt:

```
Repository: /home/claude/op_course. English only. Your role file was loaded as
your system prompt; this is a focused follow-up task on your own component.

CONTEXT
Load tests show that keyword search throughput is limited by snippet creation:
for each of the top `limit` results the server reads the whole document file
(~100 KB), decodes and tokenises all of it, although the first matching token
is usually near the beginning. docs/API.md 4.6 defines the snippet; its
observable result must not change by a single byte.

TASK
Make snippet extraction stop as early as possible, in server/src/snippet.cpp and
server/include/search/snippet.h (keep the existing function signatures
makeSnippet(fileBytes, expandedTerms) and makeSnippetFromFile(path, terms);
internal helpers are yours to reshape):
- Process the input incrementally: stop once the first matching token is found,
  the 200-code-point window after `s` is complete, and it is known whether any
  further non-whitespace content follows (that decides the trailing "...",
  because T is trimmed). Do not materialise the whole normalised text.
- makeSnippetFromFile must not read the whole file up front: read in chunks
  (e.g. 16-64 KiB) and stop reading when the snippet is complete. Handle a
  token, a UTF-8 sequence, a whitespace run and the BOM split across chunk
  boundaries correctly; tokens are at most 64 characters but an over-long run of
  token characters (discarded by the tokenizer) can span many chunks.
- Term lookup should not be linear in the number of expanded terms per token
  (up to 48 terms): use a small hash set or sorted vector of string_views.
- No-match case (4.6 as you implemented it: window from p = 0) must keep working
  and must also stop early.

TESTS
- All existing snippet tests must pass unchanged.
- Add an equivalence test: keep the previous straightforward implementation as
  a reference function inside the test file only, and compare outputs on many
  generated inputs (random mixes of ASCII words, multi-byte characters, invalid
  bytes, long whitespace runs, BOM, match at start / middle / end / absent,
  texts shorter and longer than the window) and, for the file variant, with
  tiny chunk sizes (e.g. 1, 2, 3, 7 bytes via an internal/testing hook) so that
  every boundary case is hit.
- Run the whole suite in the normal build and core_tests under -DENABLE_TSAN=ON
  (build dir build/tsan), -j2. clang-format -i your files.
- Measure before/after with a small throwaway program or the existing
  index_bench pattern: time for 10 000 makeSnippetFromFile calls on ~100 KB
  documents from /tmp/ds100 (recreate with `python3 dataset/generate.py --out
  /tmp/ds100 --size-mb 100 --docs 2000` if missing) with a term that appears
  early and a term that does not appear at all.

Touch only server/src/snippet.cpp, server/include/search/snippet.h and
server/tests/snippet_test.cpp (plus server/CMakeLists.txt only if a new test
file is really needed). Another agent is writing documentation in docs/ and
README.md at the same time. Do NOT run git add / git commit / git push.

REPORT BACK (concise): what changed, test results (normal + TSAN), before/after
timings, any case where the old and new behaviour could differ.
```

Outcome: accepted (129 tests pass; output byte-identical to the previous implementation in an equivalence test; about 40x faster when the match is near the start, as measured by the subagent)

### 9. docs-writer — README, missing diagrams, explanatory note draft
Date: 2026-11-16
Model: sonnet
Prompt:

```
Repository: /home/claude/op_course (C++20 coursework "Parallel Computing", KPI:
parallel search engine with LLM query expansion). Your role file was loaded as
your system prompt; this is the concrete task. Describe only what exists: check
the code, docs/API.md, docker-compose.yml, Dockerfile, dataset/README.md,
bench/README.md and the CLI help (build/cli/search_cli --help) before writing.

READ FIRST: CLAUDE.md; docs/API.md; docs/assignment/coursework-2026-2027.pdf
(Ukrainian; Appendix A lists the required sections of the explanatory note and
the grading table); the three diagrams in docs/uml/*.png; docker-compose.yml;
Dockerfile; server/include/search/*.h (header comments explain the design);
server/src/search_pipeline_service.cpp and api_server.cpp; cli/src/main.cpp;
web/; bench/README.md and bench/ANALYSIS.md; docs/prompts.md.

LANGUAGES
- README.md and everything under docs/uml/: English.
- The explanatory note (docs/report/): UKRAINIAN — the course is taught in
  Ukrainian. Natural, plain academic Ukrainian; keep identifiers, endpoint
  names, JSON and commands in their original form. No filler, no marketing tone.

DELIVERABLE 1 — README.md (repo root)
Sections: what it is (3–4 sentences) and an architecture sketch (components and
compose services, a small Mermaid diagram is fine); requirements (Docker with
Compose v2 only; disk and RAM needs); quick start (git clone, docker compose up)
with what happens on first start (image build, dataset generation ~2.1 GB,
model pull in the background) and how to see that it is ready; first steps
(build the index, search — with curl, with ./search, in the browser at
http://localhost:8080); dataset (container path /data/dataset, the `dataset`
volume, format: one UTF-8 .txt per document, doc_id = relative path, how it is
generated, size/doc-count/seed variables, how to use a smaller one for
development and how to plug in your own corpus); API summary table of the four
REST endpoints and the WebSocket with one curl example each and one websocat
example, linking to docs/API.md for the field-level contract (do not duplicate
its tables); CLI usage: every option of ./search with sample output (take real
output by running the binary: start build/server/search_server with
PORT=18095 DATASET_PATH=/tmp/ds100 and use SEARCH_CLI_LOCAL=build/cli/search_cli
./search --port 18095 …; /tmp/ds100 can be recreated with
`python3 dataset/generate.py --out /tmp/ds100 --size-mb 100 --docs 2000`), and
the Windows note about search.cmd; Web UI; configuration (all environment
variables with defaults, from docker-compose.yml and docs/API.md 12);
development (local CMake build, running tests, ThreadSanitizer build,
clang-format); benchmarks (the Docker profile command, where results go; the
first index build after start may be slower because files are not yet in the
page cache — the benchmark does a warm-up run); project structure;
troubleshooting (Ollama model still downloading -> "ollama": false and
fallback expansion; search returns 503 before the first index build; port 8080
busy -> PORT; regenerating the dataset; removing volumes).
Do not state timings or sizes you cannot derive from the files; where a number
is an estimate, say so.

DELIVERABLE 2 — docs/uml/README.md (English, Mermaid diagrams GitHub can render)
The assignment requires: use case, class, sequence for EVERY use case, and a
deployment diagram. Present as PNG: use case, class, one sequence diagram
(streaming search). Add as Mermaid:
- deployment diagram of the Docker Compose topology (services, volumes, ports,
  who talks to whom, one-shot init services);
- sequence diagrams for the remaining use cases of the use-case diagram:
  keyword search via REST (including query expansion and the fallback branch),
  build the index (thread pool, per-document tasks, shard merge, atomic swap),
  view index statistics, check service health (cached Ollama probe);
- an up-to-date class diagram matching the code (the PNG predates the
  implementation), followed by a short table "PNG diagram vs code" listing every
  difference (added classes such as IndexBuilder, Config, WebSocketSession;
  changed signatures; extra IndexStats fields) so the student can redraw the PNG.
Keep names exactly as in the code.

DELIVERABLE 3 — docs/report/explanatory-note.md (Ukrainian), one file with the
sections demanded by Appendix A:
1. Вступ — мета, постановка задачі, обрані технології та чому.
2. Модель системи — посилання на діаграми (PNG та docs/uml/README.md) з
   коротким поясненням кожної.
3. Опис реалізації — архітектура; інвертований індекс (шардування, блокування,
   зберігання постингів, чому немає deadlock, атомарна заміна індексу); пул
   потоків; побудова індексу; токенізація, оцінювання TF-IDF, сніпети;
   інтеграція з Ollama (промпт, розбір відповіді, fallback); REST API і
   WebSocket (порядок стадій, черга на з'єднання); клієнти; контейнеризація.
   Explain design decisions and their reasons at a level the student can defend
   orally; include short code excerpts only where they clarify a mechanism.
4. Тестування та результати — unit and race tests that exist (what they check,
   ThreadSanitizer), then the measurement methodology from bench/, with clearly
   marked placeholders «[ЗАПОВНИТИ після локального запуску: …]» for the two
   required charts, the search-result table and the bottleneck analysis; say
   which bench output file each placeholder is filled from. No invented numbers.
5. Висновки — a skeleton with the points to make and placeholders for the
   measured values.
Додаток А — використані промпти: explain that development was orchestrated
with AI agents and that docs/prompts.md holds every prompt verbatim; list the
entries (number, agent, model, purpose, outcome) in a table and include the
query-expansion prompt verbatim. Do not paste the full prompts.
Also create docs/report/README.md (Ukrainian, short): how to turn the note into
a PDF and the checklist of what the student still has to add (hardware record,
charts, tables, redrawn diagrams, repository link).

RULES
- Touch only README.md, docs/uml/README.md and docs/report/*. Do not edit
  docs/API.md, docs/prompts.md or any code. Another agent is changing
  server/src/snippet.cpp internals right now (behaviour unchanged) — describe
  snippets by the rule in docs/API.md 4.6, not by implementation details.
- If the code and docs/API.md disagree anywhere, do not paper over it: list it
  in your report.
- Do NOT run git add / git commit / git push.

REPORT BACK (concise, English): files written; gaps you could not fill and why;
any place where code, API.md, compose and README facts disagree.
```

Outcome: accepted (README samples taken from a live local run; measured results left as marked placeholders)

### 10. verifier — independent acceptance check
Date: 2026-11-16
Model: opus
Prompt:

```
Repository under test: https://github.com/Stalkeroid/op_course (branch main;
a local checkout with the same commits is at /home/claude/op_course — do not
modify it, and do not build inside it). Your role file was loaded as your
system prompt. English only. You did not build this system: trust nothing,
check everything, report evidence.

SANDBOX LIMITS (they change HOW you verify, not WHAT)
- A Docker daemon is not running and Docker Hub pulls are blocked here, so
  `docker compose up` cannot be executed. Instead:
  a. `docker compose config -q` must pass (the compose plugin is installed).
  b. Reproduce the image build natively in the fresh clone with exactly the RUN
     commands of the Dockerfile's build stage (this machine has the same
     toolchain: GCC 13, CMake, Ninja, git), then lay out /tmp/verify/app like
     the runtime stage (search_server, search_cli, web/).
  c. Review Dockerfile and docker-compose.yml line by line for anything that
     would fail or hang on a real `docker compose up` from a fresh clone
     (missing packages for the build, COPY paths vs .dockerignore, healthcheck
     commands available in each image, depends_on conditions, quoting of
     entrypoint/command, env var names vs docs/API.md 12, volume paths and
     read-only flags, services that would block `up` from finishing or make it
     exit, arm64 compatibility, the `bench` profile service, the `./search`
     wrapper's `docker compose exec` call). Report each risk with the line.
- Real Ollama is not available. Use the fake at /tmp/smoke/fake_ollama.py
  (python3 fake_ollama.py PORT; answers /api/tags with model llama3.2:1b and
  /api/generate after 0.3 s). If it is missing, write an equivalent in /tmp.
- Dataset: generate with the repository's own generator into /tmp/verify/ds
  using --size-mb 100 --docs 2000 (same code path as the dataset-init service).
- Machine: 2 cores, 7 GB RAM. Build with -j2. Python `websockets` is installed.

PROCEDURE
1. Fresh clone into /tmp/verify/repo (git clone --depth 50 from GitHub; if that
   fails, `git -C /home/claude/op_course archive HEAD | tar -x -C …` and say so).
   Check that HEAD equals the local checkout's HEAD.
2. Build as in SANDBOX LIMITS b. Then a second build dir with tests:
   run all tests; then -DENABLE_TSAN=ON and run both test executables, counting
   "WARNING: ThreadSanitizer" lines.
3. Start the server exactly as the compose `server` service would (same env var
   names and values, adjusted paths/ports: PORT=18100, DATASET_PATH, WEB_ROOT,
   OLLAMA_URL of the fake). Read docs/API.md completely and check EVERY
   endpoint, field, type, default, status code, error code, validation-order
   rule and WebSocket rule against the running server — walk the document's
   own "Conformance checklist" (section 14) item by item and add checks of your
   own for anything the checklist misses. Include: health with Ollama down vs
   up and its 3-second bound; search before index; index with threads 1 and 2
   (does time drop?), omitted threads, empty body; stats before/after/during a
   build; searches during a rebuild; 409 on concurrent builds; every 400 code;
   404/405 JSON bodies; limit and expand; expansion llm/fallback/disabled;
   scoring rules of section 4 (recompute the score of at least two results by
   hand from the documents with a small script: tokenisation, tf, idf, weights,
   normalisation, rounding, ordering and tie-break); snippet rule 4.6
   (recompute two snippets independently); doc_id format; time_ms semantics
   (REST total vs search_time_ms vs WS time_ms); WebSocket: message order,
   exact `done` shape, error stage, several queries per connection, FIFO and
   no interleaving, parallel connections, binary frame, oversized frame (1009),
   connection stays open; static files and path traversal; SIGTERM exit time.
4. Clients: `./search` wrapper with SEARCH_CLI_LOCAL pointing at the built
   binary — every documented option, exit codes, error hints. Web UI: assets
   served with correct types; `node --check web/app.js`; read app.js and confirm
   every request/message it sends and every field it reads exists in
   docs/API.md (list any field it reads that the contract does not define).
5. Benchmarks: run `python3 bench/run_all.py --port 18100 --quick --out
   /tmp/verify/bench` (matplotlib is installed, or pip install
   --break-system-packages -r bench/requirements.txt) and confirm the outputs
   named in bench/README.md are produced. Nothing may be written into the clone.
6. Documentation vs reality: every command, option, environment variable,
   default, path, file name and JSON example in README.md, dataset/README.md
   and bench/README.md must match the code and compose file. Check that
   docs/report/explanatory-note.md and docs/uml/README.md make no factual claim
   about the implementation that the code contradicts (sample at least 15
   concrete claims: shard count, lock types, pool behaviour, limits, timeouts,
   prompt text, defaults, test counts) and contain no invented measurements.
7. Hygiene: clang-format --dry-run -Werror on all C++ files; black --check on
   Python; `git log` — Conventional Commits, subject <= 50 chars, lowercase, no
   trailing period; no dataset, build output, binaries or secrets committed;
   .gitignore sanity; executable bit on `search`.
8. Grading table of the assignment (docs/assignment/coursework-2026-2027.pdf,
   "Критерії перевірки", Ukrainian): give a verdict per criterion — verified
   here / verified by substitute (say which) / cannot be verified in this
   sandbox — with the evidence.
Stop every process you started. Do not edit, commit or push anything.

REPORT BACK
- Summary line: counts of PASS / FAIL / NOT VERIFIABLE HERE.
- Table of FAILs and RISKs only (not the passes): id, area (contract, core,
  backend, devops, clients, bench, docs, hygiene), severity (blocker / major /
  minor), what is wrong, exact evidence (command and trimmed output, or file
  and line), and what the fix should achieve (not how to code it).
- The grading-criteria table from step 8.
- A short list of what a human must still check on a real machine.
```

Outcome: accepted (about 150 checks; contract checklist 29/29 pass; 10 findings routed to prompts 11-13 and to small fixes by the orchestrator)

### 11. api-contract — contract amendments after verification
Date: 2026-11-16
Model: opus
Prompt:

```
Repository: /home/claude/op_course. English only. Your role file was loaded as
your system prompt. Edit docs/API.md only.

An independent verification of the finished server found three places where the
contract promises more than a fixed-size server can deliver, or says nothing.
Amend docs/API.md so that it states the following rules precisely, in the
sections where a reader would look for them, keeping the document's style
(tables, MUST/ALWAYS wording). Do not change any mandatory shape, field, code
or existing default.

1. Bounded query expansion (sections 5.3/5.4, and wherever the fallback reasons
   are listed, including the WebSocket section's situations table):
   the server runs at most 8 Ollama expansion calls at the same time, counted
   across REST and WebSocket together. A request that needs expansion while 8
   are already in flight does NOT wait: it proceeds at once with the fallback
   rule (`expansion` = "fallback", expanded_terms = original tokens). This is
   load shedding, not an error. State the reason: a slow or hanging Ollama must
   never occupy all request workers.
2. Health guarantee (section 6.2): thanks to rule 1, GET /api/health (and
   GET /api/stats) answer within 3 seconds even when Ollama hangs and many
   searches are waiting on it. Keep the existing rules.
3. WebSocket capacity (section 10.1): queries of all connections are executed
   by a fixed pool of max(16, 2 x hardware_concurrency) workers. While fewer
   queries than that are running, connections are independent; when all workers
   are busy, further queries (from any connection) wait and are started in
   arrival order. Additionally: a client frame that fails validation
   (invalid_json, invalid_body, invalid_query, empty_query, invalid_mode,
   invalid_limit, invalid_expand) on a connection that has no query running or
   queued is answered immediately, without waiting for a pool worker; if that
   connection has earlier queries pending, the error keeps its FIFO position.
4. HEAD (sections 2.1 and 13): the HEAD method is not supported. The response
   to a HEAD request on any path is produced by the HTTP library (currently
   404 with a short non-JSON body) and is outside this contract; the
   405 method_not_allowed rule applies to the other methods (e.g. PUT, DELETE,
   PATCH, GET on a POST-only path).
5. Conformance checklist (section 14): add or adjust items so that rules 1-4
   are checkable (e.g. "with a hanging Ollama and 16 concurrent searches,
   exactly the excess over 8 return at once with expansion=fallback and health
   answers within 3 s").

Report back: the list of sections changed with a one-line summary each, and
anything in these rules that conflicts with existing text and how you resolved it.
```

Outcome: accepted (rules added, no mandatory shape changed)

### 12. concurrency-core — bound memory across index rebuilds
Date: 2026-11-16
Model: opus
Prompt:

```
Repository: /home/claude/op_course. English only. Your role file was loaded as
your system prompt; this is a follow-up on your own component.

FINDING (independent verifier, full dataset 2.2 GB / 20 000 documents)
Resident memory of search_server grows on every POST /api/index rebuild:
VmRSS 393 MB -> 749 -> 1077 -> 1453 -> 1821 -> 2124 -> 2393 -> 2418 MB over
builds with threads cycling 1-4, although one index needs ~390 MB. It is not a
true leak: with MALLOC_ARENA_MAX=1 it levels off at ~770 MB (old + new index
during the swap). So freed index memory stays inside glibc malloc arenas
(per-thread arenas, raised dynamic mmap threshold, fragmentation) and is not
reused or returned. The benchmark performs ~25 rebuilds with up to 16 threads;
under a Docker Desktop memory limit the server could be killed.

GOAL
After any number of rebuilds (any thread counts, including 16 on this 2-core
machine) RSS returns to roughly one index and never exceeds roughly
(old index + new index + per-thread working buffers) during a build — without
slowing the build down measurably and without setting MALLOC_ARENA_MAX=1
(a single arena would serialise allocations of all workers).

DIRECTIONS (choose what the measurements support; explain the choice briefly
in a code comment, the student has to defend it)
- The index already allocates postings from 256 KiB slabs: make the big
  blocks come from, and go back to, the OS deterministically (e.g. allocate
  slabs with mmap/munmap directly on Linux, or fix M_MMAP_THRESHOLD with
  mallopt so that glibc does not raise it dynamically), instead of from
  per-thread arenas.
- Per-thread scratch memory of pool workers (file buffer, term-count table)
  dies with the pool after each build: make sure it is really released.
- Return freed arena memory after an index is destroyed and after a build
  (malloc_trim) if that is still needed once the above is done.
- Keep everything guarded so the code still compiles on non-glibc platforms
  (macOS local builds): #if defined(__GLIBC__) / __linux__.
- Keep the change inside the core library (server/include/search/*.h,
  server/src/inverted_index.cpp, index_builder.cpp, custom_thread_pool.cpp and
  a new small file if useful). The served index is destroyed wherever the last
  shared_ptr is dropped, so cleanup tied to index destruction belongs in the
  index's own destructor, not in the service. Do NOT edit
  search_pipeline_service.*, api_server.*, main.cpp, docs/, README.md or
  docker-compose.yml.

MEASURE (before and after, same procedure; report a table)
- Dataset: generate once with
  python3 dataset/generate.py --out /tmp/dsfull --size-mb 2100 --docs 20000
  (about one minute, 2.2 GB; delete it when you are done).
- Use the real server so that the old-index/new-index swap is included: build
  in your own build directory build/core-mem (cmake -S . -B build/core-mem
  -DCMAKE_BUILD_TYPE=Release, -j2; nobody else is building right now), run
  search_server with PORT=18110 DATASET_PATH=/tmp/dsfull OLLAMA_URL=http://127.0.0.1:1,
  then 10 rebuilds with threads cycling 1,2,4,16, recording VmRSS and VmHWM
  from /proc/<pid>/status after each build and the reported time_ms.
- Build-time check: 3 builds each with 1 and 2 threads before and after; the
  change must not make them slower beyond noise.
- All tests must pass (ctest in your build dir), and core_tests and
  service_tests must stay clean under -DENABLE_TSAN=ON (build/tsan).
  clang-format -i touched files. Do NOT run git add / git commit / git push.

REPORT BACK (concise): what you changed and why it works; the before/after RSS
table and build times; peak RSS (VmHWM) observed during a rebuild; test results;
one sentence the README can state about memory use, with the measured numbers.
```

Outcome: accepted (RSS after 10 rebuilds 395 MB instead of 1916 MB, build time unchanged, as measured by the subagent; tests rerun by the orchestrator)

### 13. backend-api — bounded expansions, WebSocket fast error path
Date: 2026-11-16
Model: sonnet
Prompt:

```
Repository: /home/claude/op_course. English only. Your role file was loaded as
your system prompt; this is a follow-up on your own component after an
independent verification. docs/API.md has just been amended: re-read sections
5.3, 5.4, 6.2, 10.1, 10.5, 13 and the conformance checklist (14) before coding
— they now define the behaviour below; implement exactly what they say.

FINDINGS TO FIX
F3. With an Ollama that accepts connections but never answers, 16 concurrent
    POST /api/search requests occupy all 16 HTTP workers for the 15 s timeout;
    GET /api/health then takes 16 s instead of <= 3 s.
F4. With 16 WebSocket connections waiting on such an Ollama, a 17th connection
    sending invalid JSON gets its error stage only after ~14.5 s.

REQUIRED BEHAVIOUR (as now written in docs/API.md)
1. At most 8 Ollama expansion calls in flight at once, counted across REST and
   WebSocket together (one limiter owned by SearchPipelineService or
   OllamaClient; a named constant, e.g. kMaxConcurrentExpansions = 8). A request
   that needs expansion while 8 are in flight does not wait: it continues at
   once with the fallback (expansion "fallback"). Release the slot on every
   path (RAII), including exceptions and timeouts.
2. Consequently health and stats answer within 3 s under the F3 scenario.
3. WebSocket: a frame that fails validation on a connection with no query
   running or queued is answered immediately on the calling (I/O) thread,
   without a pool worker. If that connection has queries pending, the error
   keeps its FIFO position exactly as today. No message may ever be reordered
   or interleaved; keep the closed-connection guarantees.
4. In SearchPipelineService::buildIndex the previous index is currently
   destroyed inside the `servedIndex_ = std::move(built)` assignment while
   indexMutex_ is held; destroying a 380 MB index takes ~40 ms and blocks every
   snapshot() meanwhile. Move the old pointer out under the lock and let it
   die after the lock is released.
Do not change anything else in behaviour. HEAD handling stays as it is (the
contract now says HEAD is unsupported).

FILES: server/include/search/{search_pipeline_service,ollama_client}.h,
server/src/{search_pipeline_service,ollama_client,api_server}.cpp and the
service tests. Do not touch the core library files (inverted_index, snippet,
index_builder, custom_thread_pool, tokenizer), docs/, README.md, web/, cli/.

TESTS
- Limiter: with a fake Ollama that blocks until released, start 12 concurrent
  expansions -> exactly 8 reach the fake, 4 return immediately as fallback;
  after release, a new expansion reaches the fake again (slots were returned).
  Same counter shared by REST-style and streaming calls.
- WebSocket fast path: invalid frame on an idle session is answered without the
  pool (make it observable, e.g. the pool is saturated with blocked tasks and
  the error still arrives promptly); invalid frame behind a pending query keeps
  its position.
- Whole suite passes in the normal build; service_tests clean under
  -DENABLE_TSAN=ON. Use your own build directories build/backend-fix and
  build/backend-tsan (cmake -S . -B … ; -j2). clang-format -i touched files.
- End-to-end evidence with the real binary and a hanging fake Ollama (a Python
  script in /tmp that accepts /api/generate and sleeps 30 s, answers /api/tags
  normally): 16 concurrent POST /api/search -> 8 return within ~100 ms with
  expansion "fallback", 8 after the timeout; GET /api/health during that time
  -> < 3 s (report the measured time); a WebSocket connection sending
  `not json` while 16 other connections wait -> error stage promptly.
  Dataset: /tmp/ds100 (recreate with `python3 dataset/generate.py --out
  /tmp/ds100 --size-mb 100 --docs 2000` if missing).
Do NOT run git add / git commit / git push.

REPORT BACK (concise): files changed; how the limiter and the fast path work
(3-4 lines each); test results (normal + TSAN); the measured end-to-end
numbers; one paragraph (plain English) that the explanatory note can use to
describe the limiter.
```

Outcome: accepted (133 tests pass; core and service suites clean under ThreadSanitizer, rerun by the orchestrator; health answers in milliseconds with a hanging Ollama, as measured by the subagent)
