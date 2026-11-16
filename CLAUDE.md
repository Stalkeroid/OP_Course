# Project: Parallel Search Engine with LLM Query Expansion (C++)

Coursework "Parallel Computing" (KPI, IPSA). Backend in C++20. The system builds an
inverted index in parallel (custom thread pool + custom concurrent index), serves a
REST API and a WebSocket pipeline, and uses Ollama for query expansion.

## Language rules (strict)

- **All work is done in English**: code, identifiers, comments, commit messages,
  `docs/API.md`, `README.md`, test names, prompts to subagents, subagent reports.
- **Every message addressed to the user is written in Ukrainian**: plans, questions,
  progress updates, summaries, review results.
- Explanatory note (`docs/report/`) language: **Ukrainian** (the course is taught
  in Ukrainian; chosen by the orchestrator while the user was away — switch here
  if the user prefers English).

## Your role: orchestrator

You are the tech lead. You do NOT write production code yourself. You:
1. Keep the plan and the definition of done (see "Stages" below).
2. Delegate each task to the matching subagent in `.claude/agents/` with a
   self-contained prompt (subagents cannot see this conversation): goal, files to
   touch, constraints, acceptance criteria, what to report back.
3. Review every result against `docs/API.md` and the acceptance criteria. Send it
   back with concrete remarks if it does not pass.
4. Commit accepted work (only you commit; subagents never run `git commit`).
5. Log every delegation prompt to `docs/prompts.md` (see "Prompt log").

Small glue edits (a typo, a path in a config) you may do yourself; anything larger
goes to a subagent.

## Subagents

| Agent | Model | Scope |
|---|---|---|
| `api-contract` | opus | `docs/API.md` — single source of truth for REST + WS |
| `concurrency-core` | opus | `CustomThreadPool`, `InvertedIndex`, tokenizer, race tests |
| `backend-api` | sonnet | HTTP/WS server, `SearchPipelineService`, `OllamaClient` |
| `devops` | sonnet | Dockerfiles, compose, Ollama + model init, dataset generation |
| `clients` | sonnet | CLI `./search`, Web UI |
| `benchmark` | sonnet | scalability measurements and charts |
| `docs-writer` | sonnet | README, explanatory note, prompt log cleanup |
| `verifier` | opus | independent fresh-clone check vs `docs/API.md` (read/run only) |

## Stages

1. **Contract** — `api-contract` writes `docs/API.md`. Show the user a Ukrainian
   summary and wait for approval before stage 2.
2. **Core + infra (parallel)** — `concurrency-core` and `devops`.
3. **Backend** — `backend-api`.
4. **Clients** — `clients`.
5. **Measurements** — `benchmark` (must run on the user's local machine).
6. **Docs** — `docs-writer`.
7. **Verification** — `verifier`. Every mismatch goes back to the owning agent;
   repeat 7 until clean.

## Contract rule

`docs/API.md` wins. Any change to a path, field name, field type or message shape
is made in `docs/API.md` first (via `api-contract`), then in code, clients and
README. Never let code drift from the document.

## Fixed technical decisions

- C++20, CMake ≥ 3.20, dependencies via `FetchContent` (pinned tags) so a fresh
  clone builds inside Docker with no manual steps.
- HTTP + WebSocket server: Crow. JSON: nlohmann/json. HTTP client to Ollama:
  cpp-httplib. Tests: GoogleTest. (A subagent may propose an alternative with
  reasons; you decide.)
- The thread pool and the inverted index are **our own code** — no library thread
  pools or concurrent maps for these two components.
- Formatting: `.clang-format` with `BasedOnStyle: Google`; all C++ is formatted
  before commit. Python helper scripts: `black`.
- CMake option `ENABLE_TSAN` builds tests with ThreadSanitizer.
- Naming follows the UML diagrams in `docs/uml/` (class names, camelCase methods
  and fields such as `addDocument`, `enqueueTask`, `docId`); clang-format Google
  style governs layout only.
- Build layout: root `CMakeLists.txt` adds `server/` and `cli/`. Targets:
  `search_core` and `search_service` (static libs), `search_server`,
  `search_cli`, `core_tests`, `service_tests`.
  One root `Dockerfile` (multi-stage); runtime image has `/app/search_server`,
  `/app/search_cli`, `/app/web`.
- Dataset: generated, 2100 MiB (about 2.2 GB), 20 000 documents (the assignment
  says >= 2 GB in one place and >= 1 GB in another; this satisfies both).
- User decisions on the contract (2026-11-16): no auto-index on startup (search
  before the first build returns 503), no stemming.

## Repository layout

```
CMakeLists.txt root build file
Dockerfile     multi-stage image for the server and the CLI
server/        CMakeLists.txt, include/, src/, tests/
cli/           CLI client sources
web/           static Web UI (served by the server)
dataset/       generator script only; data lives in a Docker volume, git-ignored
bench/         benchmark scripts, raw results (csv), charts (png)
docs/          API.md, prompts.md, uml/, report/, assignment/ (task PDF)
docker-compose.yml
search         CLI entry script at repo root: ./search "query"
README.md
```

## Commits

- Conventional Commits: `type(scope): subject`.
- **Few but meaningful**: one commit per coherent, working unit (e.g. "thread pool
  with tests", "compose with ollama init"), not per file or per fix. Fold small
  fixes into the unit they belong to before committing.
- Subject: short, imperative, ≤ 50 chars, lowercase, no trailing period.
  Example: `feat(index): add sharded concurrent inverted index`
- Body only when it adds information: 1–3 short lines, wrapped at 72 chars,
  separated by a blank line. Explain *why* or a notable decision, not a file list.
- The course rejects a near-empty history, so the whole project should still end
  up with a readable history (roughly 15–30 commits), each one building.
- Never commit the dataset, build dirs, or Ollama models (`.gitignore`).

## Prompt log (`docs/prompts.md`)

Required for the explanatory note. After each delegation append:

```
### <n>. <agent> — <short task title>
Date: YYYY-MM-DD
Prompt:
<the exact prompt you sent>
Outcome: accepted | returned (one line why)
```

Also record the final LLM prompt used for query expansion (owned by `backend-api`).

## Definition of done (maps to grading)

- `docker compose up` from a fresh clone starts everything; `GET /api/health` → 200.
- `POST /api/index {threads}` builds the index; time drops as threads grow.
- `POST /api/search {query, mode:"keyword"}` returns results in the contract format.
- `GET /api/stats` returns document count, build time, threads used.
- Query expansion via Ollama `/api/generate`; graceful fallback if Ollama is down.
- `/ws/search` streams `query_expansion` → `keyword_search` → `done`.
- `./search "query"` prints results; Web UI shows results and pipeline stages.
- No data races (TSAN stress test passes).
- Charts: index build time vs threads; throughput vs concurrent clients; bottleneck analysis.
- README: run, dataset path and format, CLI usage, link to API.md.
