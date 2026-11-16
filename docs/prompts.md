# Prompt log

Every delegation prompt sent by the orchestrator (Claude Fable) to a subagent,
verbatim, plus the LLM prompt used for query expansion. Required for the
explanatory note.

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
