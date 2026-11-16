#!/usr/bin/env python3
"""Deterministic English-like dataset generator (standard library only).

Usage: python3 dataset/generate.py --out DIR --size-mb N --docs M --seed S
Layout: DIR/<topic>/doc_NNNNNN.txt, plus DIR/.complete (written last).
"""

import argparse
import json
import multiprocessing as mp
import os
import random
import shutil
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import vocab  # noqa: E402

MARKER = ".complete"
TOPIC_SHARE = 0.60  # words drawn from the document's own topic
GENERAL_SHARE = 0.34  # shared everyday words
CROSS_SHARE = 0.06  # words from any topic (cross-topic overlap)
OVERHEAD = 1.12  # bytes per word beyond word length + space (punctuation, newlines)

_state = {}


def init_worker(seed):
    """Build per-topic mixed pools once per process."""
    rng = random.Random(seed)
    topics = vocab.topic_names()
    bases = {t: vocab.read_words(t) for t in topics}
    general = vocab.build_general_pool()
    cross = [w for t in topics for w in bases[t]]
    pools = {}
    for t in topics:
        own = vocab.build_topic_pool(bases[t], rng)
        words, weights = [], []
        for part, share in (
            (own, TOPIC_SHARE),
            (general, GENERAL_SHARE),
            (cross, CROSS_SHARE),
        ):
            cum = vocab.zipf_cum_weights(len(part))
            total = cum[-1]
            prev = 0.0
            for w, c in zip(part, cum):
                words.append(w)
                weights.append((c - prev) / total * share)
                prev = c
        cum_w, acc = [], 0.0
        for x in weights:
            acc += x
            cum_w.append(acc)
        avg = (
            sum(len(w) * x for w, x in zip(words, weights)) / acc + 1.0 + OVERHEAD - 1.0
        )
        pools[t] = (words, cum_w, avg)
    _state.update(topics=topics, pools=pools, seed=seed)


def make_text(topic, rng, target_bytes):
    words, cum_w, avg = _state["pools"][topic]
    n = max(8, int(target_bytes / avg))
    w = rng.choices(words, cum_weights=cum_w, k=n)
    sentences, i = [], 0
    while i < n:
        j = min(n, i + rng.randint(6, 18))
        s = " ".join(w[i:j])
        sentences.append(s[0].upper() + s[1:] + ".")
        i = j
    paras, k = [], 0
    while k < len(sentences):
        m = k + rng.randint(4, 9)
        paras.append(" ".join(sentences[k:m]))
        k = m
    title = " ".join(x.capitalize() for x in w[:4])
    return title + "\n\n" + "\n\n".join(paras) + "\n"


def write_chunk(args):
    """Generate documents with indexes in [lo, hi); return (docs, bytes)."""
    out, lo, hi, mean_bytes, seed = args
    topics = _state["topics"]
    total = 0
    for i in range(lo, hi):
        rng = random.Random(seed * 1_000_003 + i)
        topic = topics[i % len(topics)]
        text = make_text(topic, rng, mean_bytes * (0.5 + rng.random()))
        data = text.encode("utf-8")
        with open(os.path.join(out, topic, "doc_%06d.txt" % i), "wb") as fh:
            fh.write(data)
        total += len(data)
    return hi - lo, total


def marker_matches(out, size_mb, docs, seed):
    try:
        with open(os.path.join(out, MARKER), encoding="utf-8") as fh:
            m = json.load(fh)
    except (OSError, ValueError):
        return False
    return (m.get("size_mb"), m.get("docs"), m.get("seed")) == (size_mb, docs, seed)


def clear_dir(out):
    os.makedirs(out, exist_ok=True)
    for name in os.listdir(out):
        p = os.path.join(out, name)
        if os.path.isdir(p) and not os.path.islink(p):
            shutil.rmtree(p)
        else:
            os.remove(p)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", required=True)
    ap.add_argument("--size-mb", type=int, default=2100)
    ap.add_argument("--docs", type=int, default=20000)
    ap.add_argument("--seed", type=int, default=42)
    ap.add_argument("--workers", type=int, default=os.cpu_count() or 1)
    ap.add_argument(
        "--if-needed",
        action="store_true",
        help="exit immediately when the marker matches size/docs/seed",
    )
    a = ap.parse_args()
    if a.size_mb < 1 or a.docs < 1:
        ap.error("--size-mb and --docs must be positive")
    if a.if_needed and marker_matches(a.out, a.size_mb, a.docs, a.seed):
        print("dataset up to date, nothing to do")
        return 0
    clear_dir(a.out)
    topics = vocab.topic_names()
    for t in topics:
        os.makedirs(os.path.join(a.out, t), exist_ok=True)
    mean_bytes = a.size_mb * 1024 * 1024 / a.docs
    step = 50
    jobs = [
        (a.out, lo, min(a.docs, lo + step), mean_bytes, a.seed)
        for lo in range(0, a.docs, step)
    ]
    t0 = time.time()
    done_docs = done_bytes = 0
    with mp.Pool(a.workers, initializer=init_worker, initargs=(a.seed,)) as pool:
        for d, b in pool.imap_unordered(write_chunk, jobs):
            done_docs += d
            done_bytes += b
            print(
                "\r%d/%d docs, %.0f MB" % (done_docs, a.docs, done_bytes / 1e6),
                end="",
                file=sys.stderr,
                flush=True,
            )
    secs = time.time() - t0
    print(file=sys.stderr)
    with open(os.path.join(a.out, MARKER), "w", encoding="utf-8") as fh:
        json.dump(
            {"size_mb": a.size_mb, "docs": a.docs, "seed": a.seed, "bytes": done_bytes},
            fh,
        )
    print(
        "done: %d docs, %.1f MB in %.1f s (%.1f MB/s)"
        % (done_docs, done_bytes / 1e6, secs, done_bytes / 1e6 / max(secs, 1e-9))
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
