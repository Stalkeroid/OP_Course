---
name: clients
description: Builds the two clients — a CLI invoked as ./search "query" and a minimal Web UI with a search form, results and live pipeline stages over WebSocket. Use for any client-side work.
tools: Read, Write, Edit, Bash, Glob, Grep
model: sonnet
---

All code, UI text and reports in English. Use only the shapes in `docs/API.md`.

## CLI

- Entry point: executable script `search` at the repo root, so
  `./search "query"` works right after `docker compose up` from a fresh clone,
  without the user compiling anything. Acceptable approaches: the script execs a
  CLI binary built inside the server image (`docker compose exec server …`), or a
  dedicated compose service. Pick the simplest robust one; coordinate with what
  exists in `docker-compose.yml` and report if a compose change is needed.
- CLI binary in C++ (`cli/`), using cpp-httplib + nlohmann/json.
- Options: `--host`, `--port` (default localhost:8080), `--limit`, `--json` (raw
  output), `--index N` (trigger `POST /api/index` with N threads), `--stats`,
  `--help`.
- Default output: readable table — rank, score (2 decimals), doc_id, snippet
  (truncated), and the total time. Clear message on errors (server down, index not
  built). Exit codes: 0 ok, 1 usage, 2 server/API error.

## Web UI (`web/`, served by the server at `/`)

- Plain HTML + CSS + vanilla JS, no build step, no CDN dependencies.
- Search form; on submit opens/uses `ws://<host>/ws/search`, sends `{ "query" }`.
- Pipeline visualisation: three steps `query_expansion → keyword_search → done`,
  each marked pending / active / complete as messages arrive, with the expanded
  terms shown as chips and the keyword search time.
- Results list: doc_id, score, snippet with query terms highlighted.
- Small admin panel: health status, stats, "Build index" with a threads input
  (REST). Errors shown inline, not with `alert()`.

No `git commit`.

## Report back

How `./search` is wired, CLI help output, a sample CLI run, and which UI states
you tested.
