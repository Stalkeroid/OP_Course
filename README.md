# Parallel Search Engine with LLM Query Expansion

A C++20 search engine for the "Parallel Computing" coursework. It builds an inverted
index of a text corpus in parallel with its own thread pool, answers keyword searches
(TF-IDF, boolean OR), expands queries with keywords and synonyms through a local LLM
(Ollama, with a fallback to the plain query), and exposes REST, WebSocket, a CLI and a Web UI.

## Quick start

Prerequisite: Docker with Compose v2.

```bash
git clone https://github.com/Stalkeroid/OP_Course.git
cd OP_Course
docker compose up
```

This builds and starts `server` (port 8080) and `ollama` (separate service), and runs two
one-shot jobs: `ollama-init` pulls the model, `dataset-init` generates the dataset. The Web UI is at
<http://localhost:8080>; `GET /api/health` returns 200 when the server is up. Search before the first index build returns `503 index_not_built`, so build it first:

```bash
./search --index 4                 # or: curl -X POST localhost:8080/api/index -d '{"threads": 4}'
./search "climate change effects"
```

## Dataset

| Item | Value |
|---|---|
| Path in the server container | `/data/dataset` (`DATASET_PATH`), read-only; Docker volume `dataset` |
| Format | one UTF-8 `.txt` file per document: `<topic>/doc_NNNNNN.txt` (16 topic folders) |
| `doc_id` | path relative to the dataset root, `/` separators, e.g. `cooking/doc_000963.txt` |
| Default size | 20 000 documents, 2100 MiB (about 2.2 GB), generated automatically on first start |

Generator options, smaller datasets, own corpus: [docs/GUIDE.md](docs/GUIDE.md).

## CLI

`./search [options] "query words"` (Windows: `search.cmd "query"`). Runs inside the `server` container.

| Option | Meaning |
|---|---|
| `--index [N]` | build the index (`POST /api/index`), optionally with `N` threads (1..64) |
| `--stats` | show index statistics (`GET /api/stats`) |
| `--health` | show server health (`GET /api/health`) |
| `--limit N` | number of results, 1..100 (server default 10) |
| `--no-expand` | skip LLM expansion (sends `"expand": false`) |
| `--json` | print the raw response body and nothing else |
| `--host H` / `--port P` | server host (default `localhost`) / port (default `$PORT`, else `8080`) |
| `--timeout S` / `--help` | read timeout in seconds (default 120; 3600 for `--index`) / show help |

Give a query or exactly one of `--index`, `--stats`, `--health`. Exit codes: 0 ok, 1 usage, 2 server/API/network error.

```text
$ ./search "recipe oven" --limit 1
Query: recipe oven
Expanded terms (fallback): recipe, oven
 1. [1.00] cooking/doc_000963.txt
      ... doughs knife sugar-pot. 245 bread-bowl carrot-herb gooding oven if cream-onion model v36 ...
total_matches: 899 | search 1 ms | total 1 ms
```

## API

All bodies are UTF-8 JSON. Unknown request fields are ignored; `null` equals absent.

| Method | Path | Purpose |
|---|---|---|
| `GET` | `/api/health` | liveness, Ollama availability, index state |
| `POST` | `/api/index` | (re)build the index in parallel (synchronous) |
| `POST` | `/api/search` | query expansion + keyword search |
| `GET` | `/api/stats` | statistics of the last completed build |
| `WS` | `/ws/search` | streaming pipeline |

**POST /api/index** (empty body = `{}`), synchronous:

| Dir | Field | Type | Notes |
|---|---|---|---|
| in | `threads` | integer, optional | 1..64; default: hardware concurrency clamped to [1, 64] |
| out | `status` | string | `"ok"` |
| out | `documents_indexed` | integer | documents in the new index |
| out | `time_ms` | integer | build wall time |
| out | `threads_used` | integer | workers used |
| out | `files_skipped` | integer | `.txt` files that could not be read |

**POST /api/search**:

| Dir | Field | Type | Notes |
|---|---|---|---|
| in | `query` | string, required | 1..1000 bytes UTF-8, not blank |
| in | `mode` | string, optional | only `"keyword"` (default) |
| in | `limit` | integer, optional | 1..100, default 10 |
| in | `expand` | boolean, optional | default `true`; `false` skips Ollama |
| out | `results` | array<SearchResult> | 0..`limit` items, best first |
| out | `time_ms` | integer | total pipeline time incl. expansion |
| out | `expanded_terms` | array<string> | query tokens, then terms added by the LLM |
| out | `expansion` | string | `"llm"`, `"fallback"` or `"disabled"` |
| out | `search_time_ms` | integer | keyword-search part only |
| out | `total_matches` | integer | matches before `limit` |

SearchResult: `doc_id` (string, relative path), `score` (number, in (0, 1], top result 1.0),
`snippet` (string, at most 206 code points, may be `""`).

**GET /api/health** and **GET /api/stats** (no request body, always 200):

| Endpoint | Field | Type | Notes |
|---|---|---|---|
| health | `status` | string | always `"ok"` |
| health | `ollama` | boolean | true if Ollama lists `OLLAMA_MODEL` |
| both | `index_ready` | boolean | at least one build completed |
| both | `indexing` | boolean | a build is running now |
| stats | `documents` | integer | documents in the served index, 0 before the first build |
| stats | `index_build_time_ms` | integer | `time_ms` of the last build |
| stats | `threads_used` | integer | threads of the last build |
| stats | `unique_terms` | integer | distinct terms |

**Errors**

Every non-2xx `/api/*` response (an unavailable Ollama is never an error, the search falls back): `{"status":"error","code":"invalid_threads","error":"..."}`
with `status` (string, always `"error"`), `code` (string, stable, listed below) and
`error` (string, human-readable, do not parse it).

| HTTP | `code` |
|---|---|
| 400 | `invalid_json`, `invalid_body` (not an object), `invalid_threads`, `invalid_query`, `empty_query`, `invalid_mode`, `invalid_limit`, `invalid_expand` |
| 404, 405 | `not_found` (unknown `/api/` path), `method_not_allowed` |
| 409 | `index_in_progress` |
| 500 | `dataset_not_found`, `dataset_empty`, `internal_error` |
| 503 | `index_not_built` |

**WebSocket /ws/search**: `ws://localhost:8080/ws/search`. One JSON text frame per message. The connection stays open
for further queries; queries of one connection run in order. Client message:

| Field | Type | Notes |
|---|---|---|
| `query` | string, required | as in REST |
| `mode` | string, optional | `"keyword"` (default) |
| `limit` | integer, optional | 1..100, default 10 |
| `expand` | boolean, optional | default `true` |

Server messages (each has `stage`, a string naming the message):

| `stage` | Field | Type | Notes |
|---|---|---|---|
| `query_expansion` | `data.original` | string | query as received |
| | `data.expanded_terms` | array<string> | as in REST |
| | `data.expansion` | string | `"llm"`, `"fallback"`, `"disabled"` |
| | `data.time_ms` | integer | expansion time |
| `keyword_search` | `data.results` | array<SearchResult> | same as REST |
| | `data.time_ms` | integer | keyword-search time only |
| | `data.total_matches` | integer | matches before `limit` |
| `done` | (no `data`) | | exactly `{"stage":"done"}` |
| `error` | `data.code` | string | code from the table above (terminal, no `done`) |
| | `data.error` | string | human-readable text |

Order per query: `query_expansion` -> `keyword_search` -> `done`, or a single `error`
(or `query_expansion` -> `error` on an internal failure).

Example: `echo '{"query": "climate change"}' | timeout 60 websocat -n ws://localhost:8080/ws/search`

Full contract with every rule: [docs/API.md](docs/API.md)

## More

- [docs/GUIDE.md](docs/GUIDE.md): architecture, configuration, Web UI, development (CMake, tests, TSAN), benchmarks, troubleshooting
- [docs/API.md](docs/API.md): full REST and WebSocket contract
- [bench/README.md](bench/README.md): how to run the benchmarks
- [bench/charts](bench/charts) and [bench/results](bench/results): measurements
