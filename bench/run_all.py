#!/usr/bin/env python3
"""Run the whole benchmark suite against a running server.

python3 bench/run_all.py                 # full run against localhost:8080
python3 bench/run_all.py --quick         # smoke run (a few minutes)
python3 bench/run_all.py --host server   # inside docker compose
"""

import argparse
import json
import os
import platform
import random
import sys
import time

import common
import loadgen
import plot
import report

STOP = set(
    "an and are as at be but by for from has have he her his in is it its of on or "
    "she that the their they this to was were will with".split()
)


def log(msg):
    print(time.strftime("%H:%M:%S"), msg, flush=True)


def parse_list(text):
    return [int(x) for x in text.split(",") if x.strip()]


# ---------------------------------------------------------------- queries


def pick_load_words(vocab):
    """About 36 queries: 2 head words per topic + 4 two-word combinations."""
    topics = common.topic_words(vocab)
    words, per_topic = [], {}
    for t, ws in topics.items():
        ok = [w for w in ws if w.isalpha() and len(w) >= 3 and w not in STOP]
        per_topic[t] = ok
        words += ok[:2]
    names = list(topics)
    for i in range(4):
        a, b = names[i * 3 % len(names)], names[(i * 3 + 5) % len(names)]
        words.append(f"{per_topic[a][2]} {per_topic[b][3]}")
    return words


def quality_queries(vocab):
    """10 fixed queries: (query, expected_topic or None)."""
    topics = common.topic_words(vocab)
    owner = {}
    for t, ws in vocab.items():
        for w in ws:
            owner.setdefault(w, set()).add(t)
    uniq = {}
    for t, ws in topics.items():
        uniq[t] = [
            w
            for w in ws
            if owner[w] == {t} and w.isalpha() and len(w) >= 3 and w not in STOP
        ]
    names = [t for t in topics if len(uniq[t]) >= 6]
    n = len(names)
    pick = lambda i: names[(i * 3) % n]
    qs = []
    for i in range(4):  # single words, head of the topic vocabulary
        t = pick(i)
        qs.append((uniq[t][0], t))
    for i in range(4, 7):  # two words
        t = pick(i)
        qs.append((f"{uniq[t][1]} {uniq[t][2]}", t))
    for i in range(7, 9):  # three words
        t = pick(i)
        qs.append((f"{uniq[t][3]} {uniq[t][4]} {uniq[t][5]}", t))
    qs.append(("zxqvwk", None))  # cannot match
    return qs


# ---------------------------------------------------------------- steps


def wait_health(host, port, tries=30):
    last = None
    for _ in range(tries):
        try:
            st, js = common.request_json(host, port, "GET", "/api/health", timeout=5)
            if st == 200:
                return js
        except OSError as exc:
            last = exc
        time.sleep(2)
    sys.exit(f"server {host}:{port} not reachable: {last}")


def build_index(host, port, threads):
    for _ in range(600):  # wait while another build runs
        st, js = common.request_json(
            host, port, "POST", "/api/index", {"threads": threads}, timeout=3600
        )
        if st == 200:
            return js
        if st == 409:
            time.sleep(2)
            continue
        sys.exit(f"index build failed: HTTP {st} {js}")
    sys.exit("index build kept returning 409")


def step_index(a, out):
    cap = 2 * (os.cpu_count() or 1)
    plist = [p for p in a.threads if 1 <= p <= min(cap, 64)]
    log(
        f"index: threads {plist} (cap 2 x {os.cpu_count()} cpus = {cap}), repeats {a.repeats}"
    )
    warm = min(4, max(plist))
    log(f"index: warm-up build with {warm} threads (untimed, fills the page cache)")
    build_index(a.host, a.port, warm)
    rows = []
    for run in range(1, a.repeats + 1):
        shift = (run - 1) % len(plist)  # rotate order so drift does not favour one p
        for p in plist[shift:] + plist[:shift]:
            js = build_index(a.host, a.port, p)
            log(f"index: run {run} threads {p}: {js['time_ms']} ms")
            rows.append(
                {
                    "threads": p,
                    "run": run,
                    "time_ms": js["time_ms"],
                    "documents_indexed": js["documents_indexed"],
                }
            )
    common.write_csv(
        os.path.join(out, common.INDEX_CSV),
        ["threads", "run", "time_ms", "documents_indexed"],
        rows,
    )


def ensure_index(a):
    st, js = common.request_json(a.host, a.port, "GET", "/api/stats")
    if not (js or {}).get("index_ready"):
        log("no index yet: building one for the search steps")
        build_index(a.host, a.port, min(4, 2 * (os.cpu_count() or 1)))


def search_bodies(words, expand, limit=10):
    return [
        json.dumps({"query": w, "mode": "keyword", "expand": expand, "limit": limit})
        for w in words
    ]


def step_load(a, out, words, expand, clients, duration, fname, label):
    rows = []
    bodies = search_bodies(words, expand)
    for c in clients:
        r = loadgen.run_level(
            a.host,
            a.port,
            "POST",
            "/api/search",
            bodies,
            c,
            duration,
            a.warmup,
            timeout=120 if expand else 60,
        )
        r["label"] = label
        log(
            f"{label}: {c:>3} clients -> {r['throughput_rps']} req/s, p50 {r['p50_ms']} ms, "
            f"p95 {r['p95_ms']} ms, errors {r['errors']}, client cpu {r['client_cpu_pct']}%"
        )
        rows.append(r)
    common.write_csv(os.path.join(out, fname), common.LOAD_FIELDS, rows)


def step_ceiling(a, out, clients, duration):
    rows = []
    for c in clients:
        r = loadgen.run_level(
            a.host, a.port, "GET", "/api/health", None, c, duration, a.warmup
        )
        r["label"] = "health"
        log(
            f"ceiling (GET /api/health): {c:>3} clients -> {r['throughput_rps']} req/s, client cpu {r['client_cpu_pct']}%"
        )
        rows.append(r)
    common.write_csv(os.path.join(out, common.CEILING_CSV), common.LOAD_FIELDS, rows)


def step_quality(a, out, vocab):
    rows, md = [], [
        "| # | query | expected | top-3 actual (doc_id, score) | result |",
        "|---:|---|---|---|---|",
    ]
    for i, (q, topic) in enumerate(quality_queries(vocab), 1):
        st, js = common.request_json(
            a.host,
            a.port,
            "POST",
            "/api/search",
            {"query": q, "mode": "keyword", "expand": False, "limit": 3},
        )
        res = (js or {}).get("results", []) if st == 200 else []
        top = [(r["doc_id"], r["score"]) for r in res]
        if st != 200:
            ok, exp = False, f"topic {topic}"
        elif topic is None:
            ok, exp = len(res) == 0, "no results"
        else:
            ok, exp = (
                bool(res) and res[0]["doc_id"].split("/")[0] == topic,
                f"{topic}/*",
            )
        actual = "; ".join(f"{d} ({s})" for d, s in top) or (
            "HTTP %d" % st if st != 200 else "(none)"
        )
        rows.append(
            {
                "n": i,
                "query": q,
                "expected": exp,
                "top3": actual,
                "result": "PASS" if ok else "FAIL",
            }
        )
        md.append(f"| {i} | `{q}` | {exp} | {actual} | {'PASS' if ok else 'FAIL'} |")
    passed = sum(r["result"] == "PASS" for r in rows)
    md += [
        "",
        f"Passed {passed} of {len(rows)}. Expected topic = folder of the vocab file that holds the word(s).",
    ]
    common.write_csv(
        os.path.join(out, common.QUALITY_CSV),
        ["n", "query", "expected", "top3", "result"],
        rows,
    )
    with open(os.path.join(out, common.QUALITY_MD), "w", encoding="utf-8") as fh:
        fh.write("\n".join(md) + "\n")
    log(f"quality: {passed}/{len(rows)} PASS")


def step_env(a, out, health):
    _, stats = common.request_json(a.host, a.port, "GET", "/api/stats")
    env = {
        "timestamp": common.now_iso(),
        "server": f"{a.host}:{a.port}",
        "health": health,
        "stats_at_start": stats,
        "client_cpu_count": os.cpu_count(),
        "client_platform": platform.platform(),
        "python": platform.python_version(),
        "parameters": {k: v for k, v in vars(a).items()},
        "to_fill_by_hand": "CPU model, physical cores/threads, RAM, OS, Docker CPU/memory limits (see bench/README.md)",
    }
    with open(os.path.join(out, common.ENV_JSON), "w", encoding="utf-8") as fh:
        json.dump(env, fh, indent=2)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawTextHelpFormatter
    )
    ap.add_argument("--host", default="localhost")
    ap.add_argument("--port", type=int, default=8080)
    ap.add_argument("--quick", action="store_true", help="short smoke run")
    ap.add_argument("--out", default=common.DEFAULT_RESULTS, help="results directory")
    ap.add_argument(
        "--charts",
        default=None,
        help="charts directory (default: bench/charts, or <out>/charts when --out is custom)",
    )
    ap.add_argument("--threads", type=parse_list, default=None, help="e.g. 1,2,4,8")
    ap.add_argument(
        "--clients", type=parse_list, default=None, help="e.g. 1,2,4,8,16,32,64"
    )
    ap.add_argument("--llm-clients", type=parse_list, default=None)
    ap.add_argument("--repeats", type=int, default=None)
    ap.add_argument(
        "--duration", type=float, default=None, help="seconds per client level"
    )
    ap.add_argument("--skip-index", action="store_true")
    ap.add_argument("--skip-load", action="store_true")
    ap.add_argument("--skip-llm", action="store_true")
    a = ap.parse_args()

    a.threads = a.threads or ([1, 2, 4] if a.quick else [1, 2, 3, 4, 6, 8, 12, 16])
    a.clients = a.clients or ([1, 4, 16] if a.quick else [1, 2, 4, 8, 16, 32, 64])
    a.llm_clients = a.llm_clients or ([1, 2] if a.quick else [1, 2, 4, 8])
    a.repeats = a.repeats or (1 if a.quick else 3)
    a.duration = a.duration or (3 if a.quick else 15)
    a.warmup = 1.0 if a.quick else 2.0
    llm_duration = 5 if a.quick else 30
    out = os.path.abspath(a.out)
    default_out = os.path.abspath(common.DEFAULT_RESULTS)
    charts = a.charts or (
        common.DEFAULT_CHARTS if out == default_out else os.path.join(out, "charts")
    )
    os.makedirs(out, exist_ok=True)

    health = wait_health(a.host, a.port)
    log(f"server health: {health}")
    step_env(a, out, health)
    vocab = common.read_vocab()
    words = pick_load_words(vocab)
    log(f"{len(words)} load-test queries from dataset/vocab")

    if not a.skip_index:
        step_index(a, out)
    if not (a.skip_load and a.skip_llm):
        ensure_index(a)
    if not a.skip_load:
        step_load(
            a, out, words, False, a.clients, a.duration, common.KEYWORD_CSV, "keyword"
        )
        step_ceiling(a, out, [max(a.clients)], a.duration)
    if not a.skip_llm:
        ready = wait_health(a.host, a.port).get("ollama")
        label = "ollama-ready" if ready else "OLLAMA-NOT-READY(fallback)"
        if not ready:
            log("WARNING: Ollama not ready; the run is labelled accordingly")
        step_load(
            a, out, words, True, a.llm_clients, llm_duration, common.LLM_CSV, label
        )
    step_quality(a, out, vocab)
    plot.make_charts(out, charts)
    # keep stats of the last build in the environment file for the record
    report.build_report(out)
    log("done")


if __name__ == "__main__":
    main()
