# Benchmarks

Scalability measurements of the search engine: index build time vs threads,
speedup / efficiency, search throughput vs concurrent clients (keyword-only and
with LLM query expansion), and a 10-query result-quality table. The qualitative
bottleneck discussion is in [ANALYSIS.md](ANALYSIS.md).

## Prerequisites

The stack must be running with the full dataset (2.1 GB, 20 000 documents) and
Ollama, then wait until `GET /api/health` answers and Ollama has pulled the model
(`"ollama": true` in `/api/health`; otherwise the expansion run is labelled
`OLLAMA-NOT-READY(fallback)` and measures the fallback path only).

    docker compose up -d --build

Close other heavy programs and keep the laptop plugged in (no power saving): the
measurements are sensitive to CPU frequency and background load.

## Run

Option A, no Python on the host (Docker profile `bench`; installs matplotlib in a
`python:3.12-slim` container, talks to `http://server:PORT`):

    docker compose --profile bench run --rm bench              # full run
    docker compose --profile bench run --rm bench --quick      # smoke test

Option B, plain Python 3.9+ on the host:

    python3 -m venv .venv && . .venv/bin/activate
    pip install -r bench/requirements.txt
    python3 bench/run_all.py --host localhost --port 8080
    python3 bench/run_all.py --quick                           # smoke test

Note: with Option A the thread list is capped by the CPU count the container
sees (`os.cpu_count()`, the host count unless you set a CPU limit).

## Options (`bench/run_all.py`)

| Option | Default | Meaning |
|---|---|---|
| `--host`, `--port` | `localhost`, `8080` | server address |
| `--quick` | off | smoke run: threads 1,2,4; clients 1,4,16 (3 s); LLM clients 1,2; 1 repeat |
| `--out DIR` | `bench/results` | CSV / REPORT.md / environment.json |
| `--charts DIR` | `bench/charts` (or `<out>/charts` if `--out` is custom) | PNG charts |
| `--threads LIST` | `1,2,3,4,6,8,12,16` capped at 2 x CPUs | thread counts for the index build |
| `--repeats N` | 3 | timed builds per thread count (after one untimed warm-up) |
| `--clients LIST` | `1,2,4,8,16,32,64` | concurrent clients, keyword-only |
| `--llm-clients LIST` | `1,2,4,8` | concurrent clients, with expansion |
| `--duration S` | 15 | seconds per keyword client level (after 2 s warm-up) |
| `--skip-index`, `--skip-load`, `--skip-llm` | off | skip a step (existing CSVs stay) |

`bench/plot.py` and `bench/report.py` can be run alone to redraw charts / the
report from existing CSVs (`--results DIR`, `--charts DIR`).

## Expected duration (full run, 2.1 GB dataset)

Estimates only, not measured here: one index build takes tens of seconds to a few
minutes depending on disk and threads, and the run does 1 + 3 x (number of thread
points, 8 on an 8-thread CPU) builds, so plan **20-60 minutes** for the index step.
The keyword load takes 7 levels x 17 s plus a ceiling check, about 2.5 minutes. The LLM
step is 4 levels x 32 s, about 2.5 minutes. Use `--threads` / `--repeats` to shorten.

## Outputs

`bench/results/`: `environment.json`, `index_build.csv`, `throughput_keyword.csv`,
`throughput_llm.csv`, `loadgen_ceiling.csv`, `search_quality.csv` / `.md`,
`REPORT.md` (all tables, computed from the CSVs).
`bench/charts/`: `index_time_vs_threads.png`, `speedup_efficiency.png`,
`throughput_vs_clients.png`, `latency_vs_clients.png`.

Results are meant to be committed after a local run.

## Record the hardware by hand

`environment.json` has only what a script can see. Add to the explanatory note
(or at the top of `REPORT.md`) the items below:

- CPU model, physical cores / logical threads (Linux `lscpu`; Windows
  `wmic cpu get name,numberofcores,numberoflogicalprocessors`; macOS
  `sysctl -a | grep machdep.cpu`), power mode (plugged in / performance)
- RAM size, disk type (NVMe / SATA SSD / HDD)
- OS and version; Docker Desktop (WSL2 / Hyper-V / macOS VM) or native Docker
- Docker resource limits: Docker Desktop -> Settings -> Resources (CPUs, memory),
  or `docker info | grep -E "CPUs|Total Memory"`, and any `cpus:` / `mem_limit:`
- Dataset size: `docker compose exec server du -sh /data/dataset`; document
  count is `documents_indexed` in `index_build.csv`
- Ollama model and whether it runs on CPU or GPU (GPU is not passed into the
  container by this compose file)

## Method notes

- Index time is the server-reported `time_ms` of `POST /api/index` (API.md 7.3),
  not client wall time. The order of thread counts is rotated between repeats so
  that drift (thermal throttling, cache state) does not favour one count.
- Load generator: closed loop, one keep-alive connection per client, clients
  spread over several processes. `loadgen_ceiling.csv` runs the same generator
  against the trivial `GET /api/health`; the `client_cpu_pct` column of every
  load CSV shows how busy the generator was.
- Search queries are single words / word pairs from `dataset/vocab/`, so they
  always exist in the dataset.
