"""Charts from the CSV files (matplotlib, Agg backend). Usable standalone:

python3 plot.py [--results DIR] [--charts DIR]
"""

import argparse
import os
import statistics

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402

import common  # noqa: E402


def index_stats(rows):
    """threads -> list of time_ms (sorted by threads)."""
    by = {}
    for r in rows:
        by.setdefault(int(r["threads"]), []).append(float(r["time_ms"]))
    return dict(sorted(by.items()))


def _save(fig, path):
    fig.tight_layout()
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print("chart:", path)


def plot_index(by, charts):
    ps = list(by)
    means = [statistics.mean(by[p]) / 1000 for p in ps]
    lo = [m - min(by[p]) / 1000 for m, p in zip(means, ps)]
    hi = [max(by[p]) / 1000 - m for m, p in zip(means, ps)]
    fig, ax = plt.subplots(figsize=(7, 4.5))
    ax.errorbar(
        ps, means, yerr=[lo, hi], fmt="o-", capsize=4, label="mean (bars: min-max)"
    )
    ax.set_xlabel("Worker threads")
    ax.set_ylabel("Index build time (s)")
    ax.set_title("Index build time vs number of threads")
    ax.set_xticks(ps)
    ax.set_ylim(bottom=0)
    ax.grid(True, alpha=0.3)
    ax.legend()
    _save(fig, os.path.join(charts, "index_time_vs_threads.png"))


def plot_speedup(by, charts):
    ps = list(by)
    if 1 not in by:
        print("no 1-thread data: speedup chart skipped")
        return
    t1 = statistics.mean(by[1])
    sp = [t1 / statistics.mean(by[p]) for p in ps]
    ef = [s / p for s, p in zip(sp, ps)]
    fig, ax = plt.subplots(figsize=(7, 4.5))
    ax.plot(ps, ps, "k--", label="ideal S(p) = p")
    ax.plot(ps, sp, "o-", color="tab:blue", label="measured speedup S(p)")
    ax.set_xlabel("Worker threads p")
    ax.set_ylabel("Speedup S(p) = T(1) / T(p)")
    ax.set_xticks(ps)
    ax.grid(True, alpha=0.3)
    ax2 = ax.twinx()
    ax2.plot(
        ps, ef, "s:", color="tab:red", label="efficiency E(p) = S(p) / p (right axis)"
    )
    ax2.set_ylabel("Efficiency E(p)")
    ax2.set_ylim(0, 1.1)
    h1, l1 = ax.get_legend_handles_labels()
    h2, l2 = ax2.get_legend_handles_labels()
    ax.legend(h1 + h2, l1 + l2, loc="upper center", bbox_to_anchor=(0.5, -0.2), ncol=1)
    ax.set_title("Speedup and efficiency of index building")
    _save(fig, os.path.join(charts, "speedup_efficiency.png"))


def _load(rows):
    rows = sorted(rows, key=lambda r: int(r["clients"]))
    return [
        {
            k: (float(v) if v not in ("", None) and k != "label" else v)
            for k, v in r.items()
        }
        for r in rows
    ]


def plot_throughput(kw, llm, charts):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    if kw:
        ax.plot(
            [r["clients"] for r in kw],
            [r["throughput_rps"] for r in kw],
            "o-",
            label="keyword only (expand=false)",
        )
    if llm:
        tag = llm[0].get("label", "")
        ax.plot(
            [r["clients"] for r in llm],
            [r["throughput_rps"] for r in llm],
            "s--",
            color="tab:red",
            label="with query expansion" + (f" [{tag}]" if tag else ""),
        )
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    xs = sorted({int(r["clients"]) for r in kw + llm})
    ax.set_xticks(xs)
    ax.set_xticklabels([str(x) for x in xs])
    ax.set_xlabel("Concurrent clients")
    ax.set_ylabel("Throughput (requests/s, log scale)")
    ax.set_title("Search throughput vs concurrent clients")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    _save(fig, os.path.join(charts, "throughput_vs_clients.png"))


def plot_latency(kw, llm, charts):
    fig, ax = plt.subplots(figsize=(7, 4.5))
    for rows, ls, name, c in (
        (kw, "-", "keyword only", "tab:blue"),
        (llm, "--", "with expansion", "tab:red"),
    ):
        if not rows:
            continue
        xs = [r["clients"] for r in rows]
        ax.plot(xs, [r["p50_ms"] for r in rows], "o" + ls, color=c, label=f"{name} p50")
        ax.plot(
            xs,
            [r["p95_ms"] for r in rows],
            "^" + ls,
            color=c,
            alpha=0.55,
            label=f"{name} p95",
        )
    ax.set_xscale("log", base=2)
    ax.set_yscale("log")
    xs = sorted({int(r["clients"]) for r in kw + llm})
    ax.set_xticks(xs)
    ax.set_xticklabels([str(x) for x in xs])
    ax.set_xlabel("Concurrent clients")
    ax.set_ylabel("Request latency (ms, log scale)")
    ax.set_title("Search latency (p50 / p95) vs concurrent clients")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend()
    _save(fig, os.path.join(charts, "latency_vs_clients.png"))


def make_charts(results, charts):
    os.makedirs(charts, exist_ok=True)
    idx = common.read_csv(os.path.join(results, common.INDEX_CSV))
    if idx:
        by = index_stats(idx)
        plot_index(by, charts)
        plot_speedup(by, charts)
    kw = _load(common.read_csv(os.path.join(results, common.KEYWORD_CSV)))
    llm = _load(common.read_csv(os.path.join(results, common.LLM_CSV)))
    if kw or llm:
        plot_throughput(kw, llm, charts)
        plot_latency(kw, llm, charts)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=common.DEFAULT_RESULTS)
    ap.add_argument("--charts", default=common.DEFAULT_CHARTS)
    a = ap.parse_args()
    make_charts(a.results, a.charts)
