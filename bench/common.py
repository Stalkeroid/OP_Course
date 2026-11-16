"""Shared helpers for the benchmark scripts (standard library only)."""

import csv
import http.client
import json
import os
import time

BENCH_DIR = os.path.dirname(os.path.abspath(__file__))
REPO_DIR = os.path.dirname(BENCH_DIR)
VOCAB_DIR = os.path.join(REPO_DIR, "dataset", "vocab")
DEFAULT_RESULTS = os.path.join(BENCH_DIR, "results")
DEFAULT_CHARTS = os.path.join(BENCH_DIR, "charts")

# CSV file names inside the results directory (shared by all scripts).
INDEX_CSV = "index_build.csv"
KEYWORD_CSV = "throughput_keyword.csv"
LLM_CSV = "throughput_llm.csv"
QUALITY_CSV = "search_quality.csv"
QUALITY_MD = "search_quality.md"
CEILING_CSV = "loadgen_ceiling.csv"
ENV_JSON = "environment.json"
REPORT_MD = "REPORT.md"

LOAD_FIELDS = [
    "clients",
    "requests",
    "errors",
    "duration_s",
    "throughput_rps",
    "p50_ms",
    "p95_ms",
    "p99_ms",
    "mean_server_time_ms",
    "expansion_llm",
    "expansion_fallback",
    "expansion_disabled",
    "client_cpu_pct",
    "label",
]


def request_json(host, port, method, path, body=None, timeout=60):
    """One-shot JSON request. Returns (status, parsed_json_or_None)."""
    conn = http.client.HTTPConnection(host, port, timeout=timeout)
    try:
        payload = None if body is None else json.dumps(body)
        headers = {"Content-Type": "application/json"} if payload else {}
        conn.request(method, path, body=payload, headers=headers)
        resp = conn.getresponse()
        raw = resp.read()
        try:
            return resp.status, json.loads(raw)
        except ValueError:
            return resp.status, None
    finally:
        conn.close()


def read_vocab():
    """topic -> ordered word list (most frequent first, as in the generator)."""
    vocab = {}
    for name in sorted(os.listdir(VOCAB_DIR)):
        if not name.endswith(".txt"):
            continue
        words, seen = [], set()
        with open(os.path.join(VOCAB_DIR, name), encoding="utf-8") as fh:
            for line in fh:
                w = line.strip().lower()
                if w and w not in seen:
                    seen.add(w)
                    words.append(w)
        vocab[name[:-4]] = words
    return vocab


def topic_words(vocab):
    """Topic vocab without the 'general' (stop-word-like) list."""
    return {t: w for t, w in vocab.items() if t != "general"}


def write_csv(path, fields, rows):
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "w", newline="", encoding="utf-8") as fh:
        wr = csv.DictWriter(fh, fieldnames=fields, extrasaction="ignore")
        wr.writeheader()
        wr.writerows(rows)


def read_csv(path):
    if not os.path.exists(path):
        return []
    with open(path, newline="", encoding="utf-8") as fh:
        return list(csv.DictReader(fh))


def now_iso():
    return time.strftime("%Y-%m-%dT%H:%M:%S%z")
