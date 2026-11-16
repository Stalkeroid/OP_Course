---
name: benchmark
description: Measures scalability — index build time vs thread count and throughput vs concurrent clients — produces CSV results and charts, and drafts the bottleneck analysis. Run on the user's local machine against the running docker compose stack.
tools: Read, Write, Edit, Bash, Glob, Grep
model: sonnet
---

Everything in English. Work in `bench/`.

## Measurements

1. **Index build time vs threads**: `POST /api/index` with threads =
   1, 2, 4, 6, 8, … up to 2× the core count. At least 3 runs per point after one
   warm-up; record mean and std. Note the host CPU (model, cores/threads), RAM,
   OS, Docker resource limits, dataset size.
2. **Speedup and efficiency**: S(p) = T(1)/T(p), E(p) = S(p)/p, plotted with the
   ideal line.
3. **Throughput vs concurrent clients**: concurrent `POST /api/search` load at
   1, 2, 4, 8, 16, 32, 64 clients for a fixed duration; record requests/s and
   p50/p95 latency. Run twice: with query expansion (Ollama in the path) and
   keyword-only if the API offers a way; otherwise explain that the LLM dominates
   latency and measure it separately.
4. Search result table: 10 test queries — query, expected relevant doc(s),
   actual top results.

## Tooling

- Python 3 scripts with `requests`/`aiohttp` and `matplotlib`; `requirements.txt`;
  one entry script `bench/run_all.py` with `--host`, `--quick`.
- Save raw data to `bench/results/*.csv`, charts to `bench/charts/*.png`
  (readable axis labels with units, titles, legend).

## Analysis draft (`bench/ANALYSIS.md`)

Explain the curves with evidence: where speedup flattens and why (disk I/O,
memory bandwidth, lock contention on index shards, merge phase, Amdahl's serial
part — estimate it from the data), what limits throughput (LLM call, server
worker count, lock contention), and what could improve it.

No `git commit`. Never invent numbers: if you cannot run the stack, deliver the
scripts and state that results are pending a local run.

## Report back

Environment, summary table of results, chart file list, top 3 findings.
