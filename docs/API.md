# API Contract — Parallel Search Engine with LLM Query Expansion

This document is the **single source of truth** for every REST endpoint and WebSocket
message exposed by the search server. Server code, the CLI (`./search`), the Web UI,
the README and the verifier are all checked against it field by field. If code and
this document disagree, the code is wrong.

Keywords **MUST**, **MUST NOT** and **ALWAYS** are normative. There are no optional
behaviours: wherever the assignment is silent, this document fixes one rule.

---

## Table of contents

1. [Overview](#1-overview)
2. [General conventions](#2-general-conventions)
3. [Error model](#3-error-model)
4. [Text processing: tokenisation, query terms, scoring, snippets](#4-text-processing-tokenisation-query-terms-scoring-snippets)
5. [Query expansion (Ollama)](#5-query-expansion-ollama)
6. [GET /api/health](#6-get-apihealth)
7. [POST /api/index](#7-post-apiindex)
8. [POST /api/search](#8-post-apisearch)
9. [GET /api/stats](#9-get-apistats)
10. [WebSocket /ws/search](#10-websocket-wssearch)
11. [Dataset](#11-dataset)
12. [Configuration (environment variables)](#12-configuration-environment-variables)
13. [Other routes](#13-other-routes)
14. [Conformance checklist](#14-conformance-checklist)

---

## 1. Overview

| Item | Value |
|---|---|
| Base URL (inside Docker Compose network / host port mapping) | `http://localhost:8080` |
| REST prefix | `/api` |
| WebSocket URL | `ws://localhost:8080/ws/search` |
| Data format | JSON (UTF-8) for every request and response body |
| Search pipeline (REST and WS) | query expansion via Ollama `/api/generate` → keyword search in the inverted index |

Endpoints:

| Method | Path | Purpose | Server component (see `docs/uml/class-diagram.png`) |
|---|---|---|---|
| `GET` | `/api/health` | Liveness + Ollama availability | `ApiServer::handleHealthCheck` → `OllamaClient::checkHealth` |
| `POST` | `/api/index` | (Re)build the inverted index in parallel | `ApiServer::handleIndexPost` → `SearchPipelineService::buildIndex` |
| `POST` | `/api/search` | Expanded keyword search | `ApiServer::handleSearchPost` → `SearchPipelineService::performKeywordSearch` |
| `GET` | `/api/stats` | Statistics of the last completed build | `InvertedIndex::getStats` → `IndexStats` |
| `WS` | `/ws/search` | Streaming search pipeline | `ApiServer::handleWebSocket` → `SearchPipelineService::performStreamingSearch` |

Field classification used in every table below:

- **Mandatory** — fixed by the assignment; the automated grader checks it. Never renamed,
  retyped or removed.
- **Extension** — added by this project. In *requests* an extension field is optional and
  has a default, so a request with only mandatory fields is always valid. In *responses*
  our server ALWAYS emits extension fields; clients and the grader may ignore them.

---

## 2. General conventions

### 2.1 HTTP

- Every API response (success and error) has header `Content-Type: application/json`
  (responses to the unsupported `HEAD` method are the only exception, see §13).
- Request `Content-Type` is **not** checked: a body is parsed as JSON regardless of the
  header (the grader may send `curl -d` without a header).
- Request bodies MUST be a JSON **object**. Unknown fields are ignored.
- A field whose value is JSON `null` is treated exactly as if it were absent (its default applies).
- `POST /api/index` with an **empty body** (zero bytes or only whitespace) is treated as `{}`.
  `POST /api/search` with an empty body fails with `invalid_query` (see §3), because `query` is required.
- Servers MUST NOT require authentication. No CORS headers are sent (the Web UI is served
  from the same origin, see §13).
- All requests are processed concurrently (Crow multithreaded mode; how requests map to
  threads, and the latency limitations that follow from it, are in §2.5). Searches are
  never blocked by the index rebuild itself: they keep using the previous index
  snapshot (§7.4). A search whose connection is bound to the I/O thread that runs the
  build still waits for that thread (known limitation L2, §2.5). At most 8 expansion
  calls to Ollama are in flight at a time; a search whose handler starts while 8 are in
  flight skips expansion instead of waiting (§5.4).
- Supported methods are `GET` and `POST` (one per `/api/*` path, see §1). The
  `HEAD` method is **not supported**: the response to a `HEAD` request on any path is
  produced by the HTTP library (currently `404` with a short non-JSON body) and is
  outside this contract. See §13 for `405 method_not_allowed`.

### 2.2 JSON types and C++ types

| JSON type in this document | Meaning | C++ type (nlohmann/json side) |
|---|---|---|
| `string` | UTF-8 string | `std::string` |
| `integer` | JSON number without fraction/exponent, parsed by nlohmann as `number_integer` or `number_unsigned` | `int` / `std::int64_t` / `std::uint64_t` (stated per field) |
| `number` | JSON floating-point number | `double` |
| `boolean` | `true` / `false` | `bool` |
| `array<T>` | JSON array | `std::vector<T>` |
| `object` | JSON object | struct / `nlohmann::json` |

Integer request fields MUST be JSON integers: `4.0`, `"4"` and `true` are rejected
(nlohmann: `is_number_integer()` must be true; `is_boolean()` values are rejected).

### 2.3 Time measurements

All `*_ms` fields are integers: elapsed wall-clock milliseconds measured with
`std::chrono::steady_clock` and truncated with
`std::chrono::duration_cast<std::chrono::milliseconds>`. A value of `0` is legal.

### 2.4 Score serialisation

`score` is a JSON number rounded to 6 decimal places (`std::round(x * 1e6) / 1e6`),
then clamped to `[0.000001, 1.0]`. The top result always has `score` `1.0`.

### 2.5 Request threads and known limitations

**Threading model.** Clients observe it only through latency.

- The HTTP server runs max(16, 2 × `std::thread::hardware_concurrency()`) I/O threads.
  Every TCP connection (including one later upgraded to WebSocket) is bound to one of
  them for its whole lifetime; the HTTP library chooses the thread when it accepts the
  connection. Every REST route handler runs on the I/O thread of its connection.
- Exactly two kinds of handler keep their I/O thread busy for a long time:

  | Operation | Holds its I/O thread for | At most at once |
  |---|---|---|
  | `POST /api/search` that calls Ollama (it holds an expansion slot, §5.4) | up to `OLLAMA_TIMEOUT_MS`, plus the keyword search | 8 (the §5.4 bound, shared with WS) |
  | `POST /api/index` | the whole build | 1 (a concurrent call gets `409` at once) |

- Every other handler is short: `GET /api/health` takes at most about 2 seconds (only
  the single in-flight Ollama check can wait, §6.2), `GET /api/stats` never waits on
  anything external, searches that do not call Ollama take only the keyword-search time.
  WebSocket queries run on the WS pool (§10.1), not on I/O threads.
- Hence at most 9 of at least 16 I/O threads are occupied by long operations at any time.

**Known limitations.**

- **L1 — health/stats latency behind a long operation.** `GET /api/health` and
  `GET /api/stats` answer within 3 seconds whenever their connection is bound to an I/O
  thread that is not occupied by one of the two long operations above. This is the
  normal case. A connection that the HTTP library happens to bind to an occupied thread
  first waits until the long operation(s) already queued ahead of it on that thread end
  (normally one), and then its own handler runs. The total is therefore:
  - behind an expansion call: up to `OLLAMA_TIMEOUT_MS` plus the rest of that search
    (its keyword search), plus the request's own handler time — at most about 2 s more
    for a health request that has to run the Ollama check because the cached result
    expired (§6.2); about 17 s with the defaults;
  - behind `POST /api/index`: the remaining build duration plus, for health, up to 2 s.

  `GET /api/stats` adds no Ollama check of its own. Measured: with Ollama hanging on
  every endpoint, 16 concurrent searches and 24 concurrent health requests, one health
  request took 16.998 s (about 15 s queued behind a search plus its own 2 s Ollama
  check) while the others answered in milliseconds; with Ollama answering `/api/tags`,
  40 concurrent health requests took at most 0.01 s while 16 searches waited on
  `/api/generate`.
- **L2 — other requests on such a connection.** The same wait applies to any request on
  a connection bound to an occupied thread (static files, searches with or without
  expansion, `404`/`405` answers), and to WebSocket connections bound to it: their
  frames are read and their messages written on that I/O thread, so the fast error path
  of §10.1 and the delivery of stage messages are delayed in the same way. Two
  consequences for searches:
  - a search bound to the thread running `POST /api/index` waits for the build to end
    although the old index could serve it (measured once: 2.8 s during a 3.1 s build,
    other searches ≤ 0.011 s);
  - a search bound to a thread occupied by an expansion call starts its handler only
    after that call ends. If a slot is free by then, it makes its own expansion call, so
    it can take about 2 × `OLLAMA_TIMEOUT_MS` in total, and the number of
    `/api/generate` calls Ollama receives for a burst of searches can exceed 8 (never
    more than 8 **at the same time**, §5.4). Measured with 16 concurrent searches and a
    hanging `/api/generate`, 10 runs: 7 runs gave 8 fast responses / 8 slow / 8 calls,
    3 runs gave 7 fast / 9 slow / 9 calls with one search taking 30 s.
- Recommendation for clients (non-normative): a client that polls health should use a
  per-request timeout and retry on a new connection rather than treat one slow answer
  as a failure.
- Why it is not removed: running search and index on a separate request pool and
  completing the response from that pool was tried. With the HTTP library in use (Crow)
  it needed access to a private member of the library to avoid a use-after-free and
  cost about 14% of keyword-search throughput, so it was not adopted.

---

## 3. Error model

### 3.1 Error shape (REST)

Every non-2xx response from an `/api/*` route has exactly this body (except responses
to the unsupported `HEAD` method, §13):

```json
{ "status": "error", "code": "invalid_threads", "error": "threads must be an integer in [1, 64]" }
```

| Field | JSON type | C++ type | Required | Constraints |
|---|---|---|---|---|
| `status` | string | `std::string` | yes | always `"error"` |
| `code` | string | `std::string` | yes | one of the codes in §3.2 (machine-readable, stable) |
| `error` | string | `std::string` | yes | non-empty, human-readable English message; exact wording is **not** part of the contract |

Clients MUST branch on the HTTP status and `code`, never on `error` text.

### 3.2 Error codes and HTTP statuses

| HTTP | `code` | When | Endpoints |
|---|---|---|---|
| 400 | `invalid_json` | Body is non-empty and is not valid JSON (includes invalid UTF-8) | `POST /api/index`, `POST /api/search` |
| 400 | `invalid_body` | Body is valid JSON but not an object (e.g. `[]`, `"x"`, `5`) | `POST /api/index`, `POST /api/search` |
| 400 | `invalid_threads` | `threads` present and not an integer, or outside `[1, 64]` | `POST /api/index` |
| 400 | `invalid_query` | `query` missing, not a string, or longer than 1000 bytes (UTF-8) | `POST /api/search`, WS |
| 400 | `empty_query` | `query` is a string that is empty or contains only whitespace | `POST /api/search`, WS |
| 400 | `invalid_mode` | `mode` present and not a string, or a string other than `"keyword"` | `POST /api/search`, WS |
| 400 | `invalid_limit` | `limit` present and not an integer, or outside `[1, 100]` | `POST /api/search`, WS |
| 400 | `invalid_expand` | `expand` present and not a boolean | `POST /api/search`, WS |
| 404 | `not_found` | Unknown path under `/api/` | any |
| 405 | `method_not_allowed` | Known `/api/*` path, wrong HTTP method (`GET`, `POST`, `PUT`, `DELETE`, `PATCH`; `HEAD` is outside the contract, §13) | any |
| 409 | `index_in_progress` | `POST /api/index` while another build is running | `POST /api/index` |
| 500 | `dataset_not_found` | `DATASET_PATH` does not exist or is not a readable directory | `POST /api/index` |
| 500 | `dataset_empty` | `DATASET_PATH` contains zero `.txt` files that could be read | `POST /api/index` |
| 500 | `internal_error` | Any unexpected exception (caught at the handler boundary) | any |
| 503 | `index_not_built` | Search requested but no index build has ever completed | `POST /api/search`, WS |

Ollama being unavailable, slow or busy is **never** an error, and neither is the
expansion concurrency limit being reached (see §5.4).

### 3.3 Validation order

Validation stops at the first failure and reports that one error:

1. Parse body → `invalid_json`; check it is an object → `invalid_body`.
2. Fields in this order: `/api/index`: `threads`. `/api/search` and WS: `query`
   (`invalid_query`, then `empty_query`), `mode`, `limit`, `expand`.
3. State checks: `/api/index` → `index_in_progress`, then dataset checks
   (`dataset_not_found`, `dataset_empty`). Search → `index_not_built`.

Consequence: an invalid search request gets `400` even when the index is not built.

---

## 4. Text processing: tokenisation, query terms, scoring, snippets

These rules are observable (they decide which queries match and in what order) and
are shared by index construction and query processing. They are implemented once in
the tokenizer used by `InvertedIndex` and `SearchPipelineService`.

### 4.1 Tokenisation

Input: a UTF-8 byte string. Output: an ordered list of tokens.

1. A **token character** is an ASCII letter `A–Z`, `a–z` or ASCII digit `0–9`.
   Every other byte (punctuation, whitespace, `_`, `'`, `-`, and every byte ≥ 0x80,
   i.e. all non-ASCII characters) is a separator.
2. A token is a maximal run of token characters, converted to lowercase ASCII.
3. Tokens with length < 2 or > 64 characters are discarded.
4. Tokens in the stop-word list below are discarded.
5. No stemming, no lemmatisation. `running` and `run` are different terms.

Examples: `"Hello, World!"` → `["hello", "world"]`; `"C++20 and e-mail"` →
`["20", "mail"]` (`c`, `e` too short; `and` stop word); `"don't"` → `["don"]`
(`t` too short).

Stop words (exact, complete list, 33 words):

```
an and are as at be but by for from has have he her his in is it its
of on or she that the their they this to was were will with
```

(`a` and `i` need no entry because single-character tokens are already discarded.)

### 4.2 Query term set

For a query string `q`:

1. `O` = tokens of `q` (§4.1), deduplicated keeping first occurrence, truncated to the
   first 32 tokens.
2. `L` = LLM expansion tokens (§5.2): each term returned by Ollama is tokenised with
   §4.1; resulting tokens are deduplicated, tokens already in `O` are removed, and the
   list is truncated to the first 16 tokens. `L` is empty when expansion is disabled
   or falls back.
3. **`expanded_terms` = `O` followed by `L`** (an ordered array of unique single tokens).
   This is the exact array returned in REST and WS responses.
4. Term weight: `w(t) = 1.0` if `t ∈ O`, `w(t) = 0.5` if `t ∈ L`.

If `O` is empty (e.g. query `"the of !!"`), Ollama is **not** called, `L` is empty,
`expansion` is `"disabled"`, `expanded_terms` is `[]` and the search succeeds with
`results: []` (it is not an error; `empty_query` is only for empty/whitespace strings).

### 4.3 Matching

Boolean **OR**: a document matches if it contains at least one term of
`expanded_terms`.

### 4.4 Scoring

Let `N` = number of documents in the served index, `df(t)` = number of documents
containing term `t`, `c(t,d)` = number of occurrences of `t` in document `d`.

```
tf(t,d)  = 1 + ln(c(t,d))                 for c(t,d) >= 1
idf(t)   = ln(1 + N / df(t))              (natural log; always > 0)
raw(d)   = Σ_{t ∈ expanded_terms, c(t,d) >= 1}  w(t) · tf(t,d) · idf(t)
score(d) = raw(d) / max_{d' matching} raw(d')
```

Computed in `double`. `score` is then rounded/clamped per §2.4, so every returned
`score` lies in `(0, 1]` and the first result has `score` `1.0`.

### 4.5 Ordering and limit

- Sort by `raw(d)` descending (unrounded); ties broken by `doc_id` ascending
  (byte-wise `std::string` comparison, i.e. `operator<`).
- Return the first `limit` documents (default 10, maximum 100).
- No matches → `"results": []` (HTTP 200).

### 4.6 Snippet

For each returned document, `snippet` is produced at query time from the document file
(`DATASET_PATH` + `/` + `doc_id`):

1. Decode the file as UTF-8; invalid byte sequences become U+FFFD. A leading BOM
   (EF BB BF) is removed.
2. Collapse every maximal run of whitespace (space, `\t`, `\n`, `\r`, `\f`, `\v`) into a
   single space `U+0020`; trim leading and trailing spaces. Call the result `T`
   (positions below are counted in Unicode **code points** of `T`).
3. Tokenise `T` with §4.1 and find the **first** token (lowest position) that is in
   `expanded_terms`. Let `p` be the code-point position of its first character.
4. Window: `s = max(0, p − 60)`, `e = min(len(T), s + 200)`.
5. `snippet = (s > 0 ? "..." : "") + T[s, e) + (e < len(T) ? "..." : "")`.

Therefore `snippet` is valid UTF-8, has no newlines/tabs and at most 206 code points.
If the file can no longer be read at query time, `snippet` is `""` (the result is still
returned).

---

## 5. Query expansion (Ollama)

Implemented by `OllamaClient::expandQuery(originalQuery)`; used by both
`POST /api/search` and `/ws/search`.

### 5.1 Call

`POST {OLLAMA_URL}/api/generate` with body:

```json
{
  "model": "<OLLAMA_MODEL>",
  "prompt": "<expansion prompt containing the original query>",
  "stream": false,
  "format": "json",
  "options": { "temperature": 0 }
}
```

Timeout: `OLLAMA_TIMEOUT_MS` (default 15000 ms) for connect + read combined.
The prompt asks the model for keywords **and synonyms** of the query and requires the
answer as a JSON object `{"terms": ["...", "..."]}`. The exact prompt text is owned by
`backend-api` and recorded in `docs/prompts.md`; it is not part of this contract.

### 5.2 Parsing

The Ollama response field `response` (a string) is parsed as JSON. The expansion terms
are the string elements of its `terms` array (non-string elements ignored). Each term
goes through §4.2 step 2.

### 5.3 Expansion source

Every pipeline run reports how `expanded_terms` was produced in the extension field
`expansion`:

| Value | Meaning |
|---|---|
| `"llm"` | Ollama answered and parsing succeeded (even if `L` turned out empty) |
| `"fallback"` | Ollama call failed (connection refused, timeout, non-200, model missing, unparsable answer) **or** was not attempted because 8 expansion calls were already in flight (§5.4) → `L` empty |
| `"disabled"` | Request had `"expand": false`, or the query has no tokens (`O` empty, §4.2) → Ollama not called, `L` empty |

### 5.4 Fallback rule and concurrency bound

**Fallback.** On any Ollama failure the request **still succeeds** with HTTP 200 (or the
normal WS sequence): `expanded_terms` = `O` (tokens of the original query), `expansion` =
`"fallback"`. Ollama failures are never reported as errors.

**Concurrency bound (load shedding).** The server runs **at most 8** Ollama expansion
calls (`POST {OLLAMA_URL}/api/generate`) at the same time, counted across
`POST /api/search` and `/ws/search` together (one server-wide counter; C++ constant
`kMaxConcurrentExpansions = 8`).

| Situation when a query needs expansion (`expand` = `true` and `O` non-empty) | Behaviour |
|---|---|
| Fewer than 8 expansion calls in flight | takes a slot, calls Ollama; the slot is released when the call ends (success, failure, timeout or exception) |
| 8 expansion calls already in flight | does **not** wait and does **not** call Ollama: proceeds at once with the fallback rule above (`expansion` = `"fallback"`, `expanded_terms` = `O`) |

Rules:

- This is load shedding, **not an error**: HTTP 200 / normal WS sequence, no error code.
- The slot check happens when the query's handler (REST) or pool task (WS) starts, not
  when the request arrives at the socket. The invariant is "never more than 8 calls in
  flight at the same moment"; the **total** number of calls for a burst of queries is
  not bounded by 8 (a REST search delayed by known limitation L2, §2.5, may find a free
  slot and call Ollama after earlier calls ended).
- Queries with `expansion` = `"disabled"` never take a slot. The health probe
  (`GET {OLLAMA_URL}/api/tags`, §6.2) is not an expansion call and is not counted.
- Reason: an expansion call blocks the thread that runs it (the HTTP I/O thread of a
  REST search, a WS pool worker for a WS query) for up to `OLLAMA_TIMEOUT_MS` (15 s by
  default). A slow or hanging Ollama MUST NEVER occupy all of them; with the bound, at
  most 8 threads are ever blocked on Ollama, out of at least 16 I/O threads and at
  least 16 WS pool workers, so health, stats, index and non-expanded searches keep
  being served on the threads that are not blocked. Requests on a connection bound to
  a blocked I/O thread wait until that call ends (known limitations L1 and L2, §2.5).

---

## 6. GET /api/health

Reports that the server process is up and whether Ollama is usable.

### 6.1 Request

No body, no parameters.

### 6.2 Response 200

| Field | Class | JSON type | C++ type | Constraints |
|---|---|---|---|---|
| `status` | Mandatory | string | `std::string` | always `"ok"` |
| `ollama` | Mandatory | boolean | `bool` | `true` iff `GET {OLLAMA_URL}/api/tags` returns 200 within 2000 ms **and** its `models[].name` list contains `OLLAMA_MODEL` (exact match, or match after appending `":latest"` to a tag-less `OLLAMA_MODEL`) |
| `index_ready` | Extension | boolean | `bool` | `true` iff at least one index build has completed (searches will not return 503) |
| `indexing` | Extension | boolean | `bool` | `true` iff an index build is running right now |

Rules:

- ALWAYS HTTP 200 while the process runs, regardless of Ollama state, model download
  state or index state. It never returns the error shape (except `internal_error` 500
  on a programming bug).
- The health handler itself takes at most about 2 seconds, even if Ollama hangs: its
  only blocking step is the Ollama check (2000 ms timeout), and only the request that
  runs the check waits for it (single-check rule below).
- Latency bound: a health request is answered within 3 seconds whenever its connection
  is bound to an HTTP I/O thread that is not occupied by a long-running
  `POST /api/search` expansion or `POST /api/index` build (§2.5). This is the normal
  case: at most 9 of at least 16 I/O threads can be occupied. A connection bound to an
  occupied thread waits until that operation ends, and then its own handler runs
  (known limitation L1, §2.5: up to `OLLAMA_TIMEOUT_MS` plus about 2 s).
  `GET /api/stats` has the same bound under the same condition (§9.2).
- **Ollama check and its result (cache + single check).** The server keeps the result
  of the last *completed* Ollama check and the time it completed. When the health
  handler runs:
  1. if a completed result exists and is less than 5 seconds old, it is returned;
  2. otherwise, if a check is already in progress (started by another health request),
     the handler does not start another one and does not wait for it: it returns the
     last completed result at once (`false` if no check has completed yet);
  3. otherwise this request runs the check (up to 2000 ms), stores and returns its
     result.

  Consequences: at most one Ollama check runs at a time, and health requests never
  wait for each other's checks, under any number of concurrent health requests, also
  when Ollama hangs on every endpoint. The returned `ollama` value is fresh (< 5 s)
  except in case 2, where it can be older: up to the 5 s cache age plus the running
  check's duration, i.e. about 7 seconds when measured at the time the handler runs
  (more, measured from the client, if the request itself was delayed by L1).

### 6.3 Example

```json
{ "status": "ok", "ollama": false, "index_ready": false, "indexing": false }
```

```bash
curl -s http://localhost:8080/api/health
```

---

## 7. POST /api/index

(Re)builds the inverted index from all documents under `DATASET_PATH` using a
`CustomThreadPool` with `threads` workers. **Synchronous**: the response is sent when the
build has finished and the new index is being served.

### 7.1 Request body

| Field | Class | JSON type | C++ type | Required | Constraints | Default |
|---|---|---|---|---|---|---|
| `threads` | Mandatory | integer | `int` | no | `1 ≤ threads ≤ 64` | `std::thread::hardware_concurrency()` clamped to `[1, 64]` (if it returns 0, use 1) |

The dataset path is **not** a request parameter; it is always `DATASET_PATH` (§11).

```json
{ "threads": 4 }
```

### 7.2 Response 200

| Field | Class | JSON type | C++ type | Constraints |
|---|---|---|---|---|
| `status` | Mandatory | string | `std::string` | always `"ok"` |
| `documents_indexed` | Mandatory | integer | `std::uint64_t` | number of documents in the new index (≥ 1) |
| `time_ms` | Mandatory | integer | `std::int64_t` | wall time of the build, see §7.3 |
| `threads_used` | Extension | integer | `int` | worker count actually used (= effective `threads`) |
| `files_skipped` | Extension | integer | `std::uint64_t` | `.txt` files found but not readable (I/O error), not indexed |

```json
{ "status": "ok", "documents_indexed": 12500, "time_ms": 8421, "threads_used": 4, "files_skipped": 0 }
```

```bash
curl -s -X POST http://localhost:8080/api/index \
     -H 'Content-Type: application/json' -d '{"threads": 4}'
```

### 7.3 What `time_ms` measures

From the moment the build starts (before directory traversal) until the new index is
fully built and finalised (all document frequencies known), **including** directory
traversal, file reading, tokenisation, insertion and pool shutdown, **excluding** request
parsing and the atomic swap. This same value is reported later as
`index_build_time_ms` in `GET /api/stats`.

### 7.4 Build semantics

- **Full rebuild every time.** Each call builds a brand-new index from scratch
  (no incremental update, no caching of file contents between builds). Calling it
  repeatedly with different `threads` values is the intended benchmarking use.
- A new `CustomThreadPool` with exactly `threads` workers is created for the build and
  shut down after it. Documents are the unit of work submitted to the pool; the
  coordinating (request) thread does not tokenise documents itself.
- **Snapshot swap.** While building, the previous index (if any) keeps serving searches
  and stats. The new index replaces it atomically (`std::shared_ptr` swap under a mutex
  or `std::atomic<std::shared_ptr>`); a search that started before the swap finishes on
  the old index.
- **One build at a time.** If a build is already running, the call returns
  `409 index_in_progress` immediately and the running build is unaffected.
- **Failure keeps the old index.** On `dataset_not_found`, `dataset_empty` or
  `internal_error` the previously served index (if any) and its stats stay unchanged.
- The server does **not** build an index on startup. Until the first successful
  `POST /api/index`, searches return `503 index_not_built` and stats return zeros.

### 7.5 Errors

| HTTP | `code` | Cause |
|---|---|---|
| 400 | `invalid_json`, `invalid_body` | malformed body |
| 400 | `invalid_threads` | `threads` not an integer or outside `[1, 64]` (e.g. `0`, `65`, `4.5`, `"4"`, `true`) |
| 409 | `index_in_progress` | another build is running |
| 500 | `dataset_not_found` | `DATASET_PATH` missing / not a directory / not readable |
| 500 | `dataset_empty` | zero readable `.txt` documents found |
| 500 | `internal_error` | unexpected failure |

```json
{ "status": "error", "code": "index_in_progress", "error": "an index build is already running" }
```

---

## 8. POST /api/search

Runs the full pipeline: query expansion (§5) → keyword search (§4).

### 8.1 Request body

| Field | Class | JSON type | C++ type | Required | Constraints | Default |
|---|---|---|---|---|---|---|
| `query` | Mandatory | string | `std::string` | **yes** | 1..1000 bytes UTF-8; not empty/whitespace-only | — |
| `mode` | Mandatory | string | `std::string` | no | exactly `"keyword"` | `"keyword"` |
| `limit` | Extension | integer | `int` | no | `1 ≤ limit ≤ 100` | `10` |
| `expand` | Extension | boolean | `bool` | no | — | `true` (call Ollama, subject to the concurrency bound of §5.4); `false` skips Ollama (`expansion` = `"disabled"`), used by benchmarks to measure pure keyword-search throughput |

```json
{ "query": "climate change effects", "mode": "keyword" }
```

### 8.2 Response 200

| Field | Class | JSON type | C++ type | Constraints |
|---|---|---|---|---|
| `results` | Mandatory | array<SearchResult> | `std::vector<SearchResult>` | 0..`limit` items, ordered per §4.5 |
| `time_ms` | Mandatory | integer | `std::int64_t` | total server pipeline time: from validated request to built response, **including** query expansion and keyword search |
| `expanded_terms` | Extension | array<string> | `std::vector<std::string>` | exactly as defined in §4.2 |
| `expansion` | Extension | string | `std::string` | `"llm"` \| `"fallback"` \| `"disabled"` (§5.3) |
| `search_time_ms` | Extension | integer | `std::int64_t` | keyword-search part only (lookup + scoring + ranking + snippets); same quantity as WS `keyword_search.data.time_ms` |
| `total_matches` | Extension | integer | `std::uint64_t` | number of matching documents before applying `limit` |

**SearchResult** object (C++ DTO `SearchResult { docId, score, snippet }` from the class diagram):

| Field | Class | JSON type | C++ member | C++ type | Constraints |
|---|---|---|---|---|---|
| `doc_id` | Mandatory | string | `docId` | `std::string` | path relative to dataset root, `/` separators, no leading `./` or `/` (§11) |
| `score` | Mandatory | number | `score` | `double` | in `(0, 1]`, 6 decimals, first result = `1.0` (§4.4) |
| `snippet` | Mandatory | string | `snippet` | `std::string` | ≤ 206 code points, see §4.6 |

No other fields appear in a SearchResult.

### 8.3 Example

Request:

```bash
curl -s -X POST http://localhost:8080/api/search \
     -H 'Content-Type: application/json' \
     -d '{"query": "climate change effects", "mode": "keyword"}'
```

Response:

```json
{
  "results": [
    {
      "doc_id": "environment/doc_004211.txt",
      "score": 1.0,
      "snippet": "...rising sea levels are among the most visible effects of climate change on coastal cities, where flooding has become a seasonal threat to infrastructure and housing. Researchers estimate that..."
    },
    {
      "doc_id": "science/doc_000987.txt",
      "score": 0.734512,
      "snippet": "Global warming alters rainfall patterns across the continent, and the change in precipitation affects agriculture in ways that farmers have only begun to measure..."
    }
  ],
  "time_ms": 1840,
  "expanded_terms": ["climate", "change", "effects", "global", "warming", "impact", "weather"],
  "expansion": "llm",
  "search_time_ms": 14,
  "total_matches": 3120
}
```

Benchmark variant (no LLM):

```bash
curl -s -X POST http://localhost:8080/api/search \
     -H 'Content-Type: application/json' \
     -d '{"query": "climate change", "mode": "keyword", "expand": false, "limit": 20}'
```

### 8.4 Errors

| HTTP | `code` | Cause |
|---|---|---|
| 400 | `invalid_json`, `invalid_body` | malformed body |
| 400 | `invalid_query` | `query` missing / not a string / > 1000 bytes |
| 400 | `empty_query` | `query` is `""` or whitespace only |
| 400 | `invalid_mode` | `mode` not a string or not `"keyword"` |
| 400 | `invalid_limit` | `limit` not an integer or outside `[1, 100]` |
| 400 | `invalid_expand` | `expand` not a boolean |
| 503 | `index_not_built` | no build has completed yet (a build in progress does not count) |
| 500 | `internal_error` | unexpected failure |

```json
{ "status": "error", "code": "index_not_built", "error": "index is not built; call POST /api/index first" }
```

---

## 9. GET /api/stats

Statistics of the **last successfully completed** build (`IndexStats` DTO from the class diagram).

### 9.1 Request

No body, no parameters.

### 9.2 Response 200

| Field | Class | JSON type | C++ member (`IndexStats`) | C++ type | Constraints |
|---|---|---|---|---|---|
| `documents` | Mandatory | integer | `documents` | `std::uint64_t` | documents in the served index; `0` before the first build |
| `index_build_time_ms` | Mandatory | integer | `indexBuildTimeMs` | `std::int64_t` | `time_ms` of the last completed build; `0` before the first build |
| `threads_used` | Mandatory | integer | `threadsUsed` | `int` | `threads` of the last completed build; `0` before the first build |
| `unique_terms` | Extension | integer | `uniqueTerms` | `std::uint64_t` | distinct terms in the served index; `0` before the first build |
| `index_ready` | Extension | boolean | `indexReady` | `bool` | same meaning as in `/api/health` |
| `indexing` | Extension | boolean | `indexing` | `bool` | same meaning as in `/api/health` |

Rules:

- ALWAYS HTTP 200 (also before the first build and during a build).
- Never contacts Ollama and never waits on anything external. Answered within 3 seconds
  whenever its connection is bound to an HTTP I/O thread that is not occupied by a
  long-running expansion or build, also when Ollama hangs and many searches are waiting
  on it; a connection bound to an occupied thread waits until that operation ends —
  up to `OLLAMA_TIMEOUT_MS` behind an expansion call, the remaining build duration
  behind a build — and then answers at once (same condition as §6.2, known limitation
  L1 in §2.5; stats runs no Ollama check, so the extra 2 s of health does not apply).
- During a rebuild the values describe the **previous** (still served) index; they change
  only when the new index is swapped in.

### 9.3 Example

```json
{ "documents": 12500, "index_build_time_ms": 8421, "threads_used": 4, "unique_terms": 184233, "index_ready": true, "indexing": false }
```

```bash
curl -s http://localhost:8080/api/stats
```

---

## 10. WebSocket /ws/search

Streams the pipeline stages of a search, one JSON text message per stage
(sequence diagram `docs/uml/sequence-streaming-search.png`).

### 10.1 Connection lifetime

- URL: `ws://localhost:8080/ws/search`. No subprotocol, no authentication.
- **The connection stays open for further queries.** A client may send any number of
  queries over one connection. The server never closes the connection after `done` or
  `error`; only the client closes it (or server shutdown, or a protocol violation per
  §10.6).
- Queries on one connection are processed **strictly sequentially in arrival order
  (FIFO)**. If a query arrives while a previous one is still running, it is queued; the
  messages of different queries are never interleaved.
- **Capacity.** Queries of all connections are executed by one fixed pool
  (`CustomThreadPool`) of max(16, 2 × `std::thread::hardware_concurrency()`) workers,
  separate from the HTTP workers. At most one query per connection runs at a time
  (rule above), so:
  - while fewer queries than the pool size are running, different connections are
    processed concurrently and independently (reading their frames and writing their
    messages still happens on each connection's I/O thread, subject to L2, §2.5);
  - when all workers are busy, further queries (from any connection) wait and are
    started in arrival order — precisely, in the order in which they became ready to
    run, a query being ready once it has arrived and every earlier query of its own
    connection has finished.
- **Fast error path.** A client frame that fails validation (`invalid_json` — including a
  binary frame —, `invalid_body`, `invalid_query`, `empty_query`, `invalid_mode`,
  `invalid_limit`, `invalid_expand`) on a connection that has **no query running or
  queued** is answered immediately, without waiting for a pool worker, even when the
  pool is saturated (the connection's own HTTP I/O thread must be free, see L2 in
  §2.5). If that connection has earlier queries pending, the `error` message
  keeps its FIFO position: it is sent after the earlier queries' sequences, never before
  or between them. `index_not_built` is a state check, not validation, and is answered
  through the pool like any valid query.
- If the client disconnects mid-pipeline, the server stops sending and discards the
  remaining work's output; this is not an error.

### 10.2 Client message

One WebSocket **text** frame containing a JSON object:

| Field | Class | JSON type | C++ type | Required | Constraints | Default |
|---|---|---|---|---|---|---|
| `query` | Mandatory | string | `std::string` | **yes** | same as REST `query` (§8.1) | — |
| `mode` | Extension | string | `std::string` | no | exactly `"keyword"` | `"keyword"` |
| `limit` | Extension | integer | `int` | no | `1 ≤ limit ≤ 100` | `10` |
| `expand` | Extension | boolean | `bool` | no | — | `true` |

```json
{ "query": "search text" }
```

Validation is identical to REST (§3.3), with the same `code` values.

### 10.3 Server messages

Every server message is a text frame containing one JSON object with a `stage` field.

#### 10.3.1 `query_expansion`

```json
{ "stage": "query_expansion", "data": { "original": "climate change effects", "expanded_terms": ["climate", "change", "effects", "global", "warming", "impact"], "expansion": "llm", "time_ms": 1790 } }
```

| Field | Class | JSON type | C++ type | Constraints |
|---|---|---|---|---|
| `stage` | Mandatory | string | `std::string` | `"query_expansion"` |
| `data.original` | Mandatory | string | `std::string` | the client's `query` exactly as received (not trimmed, not lowercased) |
| `data.expanded_terms` | Mandatory | array<string> | `std::vector<std::string>` | §4.2; equals `O` on fallback/disabled |
| `data.expansion` | Extension | string | `std::string` | `"llm"` \| `"fallback"` \| `"disabled"` |
| `data.time_ms` | Extension | integer | `std::int64_t` | time spent in query expansion (including the Ollama call) |

#### 10.3.2 `keyword_search`

```json
{ "stage": "keyword_search", "data": { "results": [ { "doc_id": "environment/doc_004211.txt", "score": 1.0, "snippet": "...effects of climate change on coastal cities..." } ], "time_ms": 12, "total_matches": 3120 } }
```

| Field | Class | JSON type | C++ type | Constraints |
|---|---|---|---|---|
| `stage` | Mandatory | string | `std::string` | `"keyword_search"` |
| `data.results` | Mandatory | array<SearchResult> | `std::vector<SearchResult>` | identical objects and ordering to REST (§8.2, §4.5); `[]` when nothing matches |
| `data.time_ms` | Mandatory | integer | `std::int64_t` | keyword-search time only (lookup + scoring + ranking + snippets), excludes expansion |
| `data.total_matches` | Extension | integer | `std::uint64_t` | matches before `limit` |

#### 10.3.3 `done`

```json
{ "stage": "done" }
```

Exactly this object: one field `stage` with value `"done"`, **no** `data` field.

#### 10.3.4 `error`

```json
{ "stage": "error", "data": { "code": "index_not_built", "error": "index is not built; call POST /api/index first" } }
```

| Field | JSON type | C++ type | Constraints |
|---|---|---|---|
| `stage` | string | `std::string` | `"error"` |
| `data.code` | string | `std::string` | a code from §3.2 |
| `data.error` | string | `std::string` | non-empty human-readable message (wording not contractual) |

`error` is **terminal for that query**: no `done` follows it. The connection stays open.

### 10.4 Message ordering guarantee

For each client query the server sends exactly one of these sequences, contiguously:

| Sequence | When |
|---|---|
| `query_expansion` → `keyword_search` → `done` | success (including Ollama fallback and `nothing matched`) |
| `error` | invalid message (§10.5) or index not built |
| `query_expansion` → `error` | unexpected failure during keyword search (`internal_error`) |

`query_expansion` is sent as soon as expansion finishes, **before** the keyword search
starts (so the client can display it while searching).

### 10.5 Error situations

| Situation | Server sends | Connection |
|---|---|---|
| Frame is not valid JSON | `error` with `invalid_json` | stays open |
| JSON but not an object | `error` with `invalid_body` | stays open |
| Binary frame | `error` with `invalid_json` | stays open |
| `query` missing/wrong type/too long, empty, bad `mode`/`limit`/`expand` | `error` with the matching code (§3.2) | stays open |
| No index built yet | `error` with `index_not_built` (no `query_expansion` is sent) | stays open |
| Index rebuild in progress but an old index exists | normal sequence on the old index | stays open |
| Ollama down / model not ready | normal sequence, `expansion` = `"fallback"` | stays open |
| 8 expansion calls already in flight (REST + WS together, §5.4) | normal sequence at once, `expansion` = `"fallback"` (load shedding, not an error) | stays open |
| All WS pool workers busy (§10.1) | valid query: waits, then normal sequence; invalid frame on an idle connection: `error` at once | stays open |

### 10.6 Protocol limits

- Maximum client frame payload: 65 536 bytes. Larger frames: the server closes the
  connection with close code 1009 (message too big).
- The server does not close idle connections.

### 10.7 Example session

```bash
websocat ws://localhost:8080/ws/search
```

```text
> {"query": "climate change"}
< {"stage":"query_expansion","data":{"original":"climate change","expanded_terms":["climate","change","global","warming"],"expansion":"llm","time_ms":1650}}
< {"stage":"keyword_search","data":{"results":[{"doc_id":"environment/doc_004211.txt","score":1.0,"snippet":"...effects of climate change on coastal cities, where flooding has become a seasonal threat..."}],"time_ms":11,"total_matches":2875}}
< {"stage":"done"}
> {"query": ""}
< {"stage":"error","data":{"code":"empty_query","error":"query must not be empty"}}
> not json
< {"stage":"error","data":{"code":"invalid_json","error":"message is not valid JSON"}}
```

One-shot from a script:

```bash
echo '{"query": "climate change"}' | timeout 60 websocat -n ws://localhost:8080/ws/search
```

(`-n` keeps the connection open after stdin EOF so the three replies arrive; `timeout`
ends the session because the server never closes the connection itself.)

---

## 11. Dataset

| Item | Rule |
|---|---|
| Location in the server container | `/data/dataset`, mounted **read-only** from a Docker volume |
| Env var | `DATASET_PATH` (default `/data/dataset`); `POST /api/index` always indexes this directory |
| Size requirements (assignment) | ≥ 10 000 English documents; total size: the assignment states ≥ 2 GB in one place and ≥ 1 GB in another. The default generated dataset (`DATASET_SIZE_MB=2100`, 20 000 documents: 2100 MiB of text, about 2.2 GB on disk) satisfies both |
| Document | one regular file whose name ends in `.txt` (lowercase, case-sensitive) = one document |
| Encoding | UTF-8 (invalid sequences tolerated: they act as separators during tokenisation and become U+FFFD in snippets) |
| Discovery | recursive traversal of `DATASET_PATH` (`std::filesystem::recursive_directory_iterator`); symbolic links are **not** followed; hidden files/directories are included; files with any other extension are ignored |
| Empty files | indexed as documents with no terms (counted in `documents_indexed`, never match) |
| Unreadable files | skipped, counted in `files_skipped`, not counted in `documents_indexed` |
| `doc_id` | path relative to `DATASET_PATH`, using `/` as separator, no leading `./` or `/`, e.g. `science/doc_000123.txt` or `doc_000123.txt`; obtained via `std::filesystem::relative(path, root).generic_string()` |
| Uniqueness | `doc_id` is unique within an index build |
| File content changes | changes are visible only after the next `POST /api/index` (snippets are read at query time and may reflect a newer file — acceptable) |

---

## 12. Configuration (environment variables)

| Variable | Default | Used for |
|---|---|---|
| `PORT` | `8080` | HTTP + WebSocket listen port |
| `DATASET_PATH` | `/data/dataset` | dataset root (§11) |
| `OLLAMA_URL` | `http://ollama:11434` | base URL of the Ollama compose service |
| `OLLAMA_MODEL` | `llama3.2:1b` | model used for `/api/generate` and checked by `/api/health` |
| `OLLAMA_TIMEOUT_MS` | `15000` | timeout of one expansion call (§5.1) |
| `WEB_ROOT` | `/app/web` | directory with static Web UI files (§13) |

---

## 13. Other routes

- `GET /` serves `WEB_ROOT/index.html`; `GET /<path>` for any path not starting with
  `/api/` or `/ws/` serves the matching static file from `WEB_ROOT` (Web UI), or
  `404` with a plain-text body if absent. These routes are not part of the JSON contract.
- Any unknown path starting with `/api/` returns `404` with the error shape
  (`not_found`); a known `/api/*` path with the wrong method returns `405`
  (`method_not_allowed`), e.g. `GET /api/search`, `POST /api/health`,
  `PUT /api/index`, `DELETE /api/stats`, `PATCH /api/search`.
- **`HEAD` is not supported.** The response to a `HEAD` request on any path (API,
  static or WebSocket) is produced by the HTTP library — currently `404` with a short
  non-JSON body — and is outside this contract; clients MUST NOT rely on it. The
  `405 method_not_allowed` rule above applies to `GET`, `POST`, `PUT`, `DELETE` and
  `PATCH`.

---

## 14. Conformance checklist

A verifier walks through these in order against a freshly started stack
(`docker compose up`). "Shape" means: exactly the listed mandatory fields with the
listed JSON types, plus only the listed extension fields. Items 2a, 22a, 22b and 27a
need a stand-in Ollama at `OLLAMA_URL` that answers `/api/tags` normally and accepts
`/api/generate` but never replies (a "hanging Ollama"); the fake should record each
`/api/generate` call and the number in flight. The known limitations of §2.5 are
documented behaviour, not checklist items: where they can change what a client
observes, the item states which outcomes are expected.

**Health**
1. `GET /api/health` → 200, shape `{status:"ok", ollama:bool, index_ready:bool, indexing:bool}`.
2. With Ollama stopped (`docker compose stop ollama`) → still 200, `ollama: false`, answered within 3 s.
2a. (Run after item 6, index built.) With a hanging Ollama (answers `/api/tags`
    normally, never replies to `/api/generate`), start 16 concurrent
    `POST /api/search {"query":"<common word>"}` requests; while some of them are still
    waiting on Ollama (§5.4), send 20 `GET /api/health` and 20 `GET /api/stats`
    requests concurrently, each on a new connection, with a client timeout of at least
    3 × `OLLAMA_TIMEOUT_MS` (L2 can chain waits). Record each request's completion time and the
    completion times of the slow searches. Pass if all of:
    - every health and stats request answers 200 with the shape of items 1 / 3;
    - at least one health request and at least one stats request answer within 3 s;
    - every request that took longer than 3 s completed within 3 s after one of the
      slow searches completed (it was queued behind that search on its I/O thread,
      known limitation L1, §2.5 — documented behaviour, not a violation).

    Expected normal outcome: all 40 answer within 3 s (5 of 5 runs in the final
    verification). A health or stats request that is slow without any search completing
    shortly before it is a failure (it would mean the handler itself waits on Ollama
    or on the searches).

**Before indexing**
3. `GET /api/stats` → 200, `documents`, `index_build_time_ms`, `threads_used` all `0`, `index_ready: false`.
4. `POST /api/search {"query":"test","mode":"keyword"}` → 503, `code: "index_not_built"`, error shape.
5. WS: send `{"query":"test"}` → single message `{"stage":"error","data":{"code":"index_not_built",...}}`; connection still open.

**Indexing**
6. `POST /api/index {"threads":1}` → 200, `status:"ok"`, `documents_indexed` ≥ 10 000 (integer), `time_ms` integer > 0, `threads_used: 1`.
7. `POST /api/index {"threads":4}` → 200, same `documents_indexed`, `time_ms` lower than step 6 (on a machine with ≥ 4 cores).
8. `POST /api/index {}` and an empty body → 200, `threads_used` = hardware concurrency clamped to [1, 64].
9. `{"threads":0}`, `{"threads":65}`, `{"threads":"4"}`, `{"threads":4.5}`, `{"threads":true}` → 400 `invalid_threads`.
10. Body `{bad json` → 400 `invalid_json`; body `[1]` → 400 `invalid_body`.
11. Two concurrent `POST /api/index` → one 200, the other 409 `index_in_progress`.
12. `GET /api/stats` → `documents` = last `documents_indexed`, `index_build_time_ms` = last `time_ms`, `threads_used` = last threads.
13. Every `doc_id` seen anywhere is a relative path with `/` separators ending in `.txt`, and `DATASET_PATH/doc_id` exists.

**Search (REST)**
14. `POST /api/search {"query":"<common word>","mode":"keyword"}` → 200; `results` is an array of ≤ 10 objects each with exactly `doc_id` (string), `score` (number), `snippet` (string); `time_ms` integer; extension fields `expanded_terms`, `expansion`, `search_time_ms`, `total_matches` present.
15. First result `score` = 1.0; all scores in (0, 1]; order is score desc, ties by `doc_id` asc.
16. `expanded_terms` begins with the query's own tokens (§4.2); with Ollama stopped, `expanded_terms` = query tokens exactly and `expansion: "fallback"`, still 200.
17. `{"query":"x","expand":false}` → `expansion: "disabled"`; mode omitted is accepted.
18. `{"query":"zzqxjv","expand":false}` (no match) → 200 `results: []`,
    `total_matches: 0`. (With expansion on, LLM-added terms may match other documents,
    so a non-empty result is then not a failure.)
19. `{"query":"the of"}` → 200 `results: []`, `expanded_terms: []`, `expansion: "disabled"`.
20. `{"query":""}` / `{"query":"   "}` → 400 `empty_query`; `{}` / `{"query":5}` → 400 `invalid_query`; `{"query":"a","mode":"semantic"}` → 400 `invalid_mode`; `{"query":"a","limit":0}` / `101` → 400 `invalid_limit`; `{"query":"a","expand":"no"}` → 400 `invalid_expand`.
21. `limit: 100` returns at most 100 results; snippets contain no `\n`/`\t`, are ≤ 206 code points, valid UTF-8, contain a query term (case-insensitive) unless `""`.
22. A search issued during a running rebuild returns 200 (served by the old index).
    Usually it answers at once; one whose connection is bound to the build's I/O thread
    answers only after the build ends (L2, §2.5) — still 200, not a failure.
22a. Expansion bound (§5.4): with a hanging Ollama (index built, no other request in
     flight), send 16 concurrent `POST /api/search {"query":"<common word>"}`, each on
     its own connection, with a client timeout of at least 3 × `OLLAMA_TIMEOUT_MS`.
     Let the fake count `/api/generate` calls and the maximum number in flight at once.
     Pass if all of:
     - at no moment are more than 8 `/api/generate` calls in flight at the fake, and
       it receives at least 8 calls in total;
     - every one of the 16 requests returns 200 with `expansion: "fallback"` and
       `expanded_terms` = the query tokens;
     - no request waits for Ollama without having called it: a request whose handler
       starts while 8 calls are in flight returns at once. Observable as: every
       response time (from the start of the burst) lies within 1 s of
       k × `OLLAMA_TIMEOUT_MS` for some integer k ≥ 0 (k = 0: fast; k = 1: its own
       call timed out, or it queued behind one; k ≥ 2: L2 chaining), and
       (number of responses with k = 0) + (total `/api/generate` calls) ≤ 16;
     - afterwards, with Ollama answering normally, a new search gets
       `expansion: "llm"` (slots were released).

     Expected counts: usually 8 fast / 8 slow / 8 calls. Because of known limitation L2
     (§2.5), a request whose connection shares an I/O thread with a waiting search
     starts only after that search ends and may then take a freed slot, giving e.g.
     7 fast / 9 slow / 9 calls with one request taking about 2 × `OLLAMA_TIMEOUT_MS`.
     Such counts are expected behaviour, not a failure.
22b. The bound is shared: with a hanging Ollama, no REST search in flight, open 8 WS
     connections that each send one expanding query and wait until the fake reports
     8 `/api/generate` calls in flight; then a `POST /api/search` needing expansion,
     on a new connection, returns at once (well under 1 s) with
     `expansion: "fallback"`, and the fake still has at most 8 calls in flight. (WS
     queries run on the WS pool, not on I/O threads, so L2 does not delay this
     request.)

**WebSocket**
23. Send `{"query":"climate change"}` → exactly three messages in order `query_expansion`, `keyword_search`, `done`; `done` is exactly `{"stage":"done"}`.
24. `query_expansion.data.original` equals the sent query; for a WS query sent with `"expand": false`, `keyword_search.data.results` equals (same `doc_id` order, scores and snippets) a REST search with the same query, `"expand": false` and the same `limit`.
25. Send two queries back-to-back on one connection → two complete, non-interleaved sequences in order.
26. Send `not json` → `error` / `invalid_json`; then a valid query on the same connection → full sequence.
27. Send `{"query":""}` → single `error` / `empty_query`, no `done`.
27a. Fast error path (§10.1): with a hanging Ollama (index built, **no REST request in
     flight**, earlier expansion calls ended), open 16 connections that each send one
     expanding query. WS queries run on the WS pool (≥ 16 workers) and no I/O thread
     is occupied by a long REST operation, so L2 (§2.5) does not apply here and the
     counts are exact: 8 connections receive their full sequence at once with
     `expansion: "fallback"` (§5.4), the other 8 receive theirs (also `"fallback"`)
     only after `OLLAMA_TIMEOUT_MS`; the fake receives 8 `/api/generate` calls and
     never more than 8 at once. Meanwhile a 17th connection sending `not json` gets
     `error` / `invalid_json` within 1 s. If REST searches are waiting on Ollama at
     the same time, only the weaker assertions of item 22a hold. (Full pool
     saturation, which a hanging Ollama alone can no longer cause, is covered by the
     service unit tests.)
27b. On one connection, send a valid query immediately followed by `not json` → the
     complete sequence of the first query, then `error` / `invalid_json` (FIFO
     position kept, no interleaving).

**General**
28. All `/api/*` responses have `Content-Type: application/json`; all errors use `{status:"error", code, error}` with the status codes of §3.2.
29. `GET /api/unknown` → 404 `not_found`; `GET /api/search` → 405 `method_not_allowed`.
30. `PUT /api/index`, `DELETE /api/stats`, `PATCH /api/search`, `POST /api/health` → 405
    `method_not_allowed` with the error shape. `HEAD` requests are not checked (outside
    the contract, §13).
