# Стартовий промт для Fable

Вставте в нову сесію Claude Code в корені репозиторію (модель: Fable).

---

Read CLAUDE.md and the agents in .claude/agents/. Assignment summary: parallel
search engine in C++20 with a custom thread pool, a custom concurrent inverted
index, REST API (/api/health, /api/index, /api/search, /api/stats), WebSocket
/ws/search streaming query_expansion → keyword_search → done, query expansion via
Ollama /api/generate, CLI ./search "query", Web UI, docker compose up from a fresh
clone with Ollama as a separate service, dataset ≥ 1 GB and ≥ 10 000 English
documents, scalability charts. UML diagrams (use case, class, sequence) are in
docs/uml/ — keep class and method names consistent with them.

Start with stage 1: delegate docs/API.md to api-contract, then show me a short
summary in Ukrainian and wait for my approval. Remember: all work in English,
all messages to me in Ukrainian, only you commit, log every delegation prompt to
docs/prompts.md.
