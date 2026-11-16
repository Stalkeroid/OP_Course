"""Writes REPORT.md from the CSV files only (every number is computed).

python3 report.py [--results DIR]
"""

import argparse
import json
import os
import statistics

import common


def _std(v):
    return statistics.stdev(v) if len(v) > 1 else 0.0


def index_section(rows):
    out = ["## Index build time vs threads", ""]
    if not rows:
        return out + ["_No index data (step skipped)._", ""]
    by = {}
    for r in rows:
        by.setdefault(int(r["threads"]), []).append(float(r["time_ms"]))
    by = dict(sorted(by.items()))
    docs = sorted({r["documents_indexed"] for r in rows})
    t1 = statistics.mean(by[1]) if 1 in by else None
    out += [
        f"Documents indexed: {', '.join(docs)}. Times are the server-reported `time_ms`.",
        "",
        "| threads p | runs | mean T(p), ms | std, ms | min, ms | max, ms | S(p)=T1/Tp | E(p)=S/p | Karp-Flatt serial fraction e |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    best = None
    for p, v in by.items():
        m = statistics.mean(v)
        if t1:
            s = t1 / m
            e = s / p
            kf = f"{(1 / s - 1 / p) / (1 - 1 / p):.3f}" if p > 1 else "n/a"
            sc, ec = f"{s:.2f}", f"{e:.2f}"
        else:
            s = None
            sc = ec = kf = "n/a"
        if best is None or m < best[1]:
            best = (p, m, s)
        out.append(
            f"| {p} | {len(v)} | {m:.0f} | {_std(v):.0f} | {min(v):.0f} | {max(v):.0f} | {sc} | {ec} | {kf} |"
        )
    out.append("")
    if best:
        txt = f"Best thread count: **{best[0]}** (mean {best[1]:.0f} ms"
        if best[2]:
            txt += f", speedup {best[2]:.2f}x"
        out.append(txt + ").")
    if t1 and len(by) > 1:
        pmax = max(by)
        s = t1 / statistics.mean(by[pmax])
        kf = (1 / s - 1 / pmax) / (1 - 1 / pmax)
        out.append(
            f"Amdahl serial fraction at p={pmax} (Karp-Flatt): {kf:.3f}; "
            f"implied speedup limit 1/e = {1 / kf:.1f}x."
            if kf > 0
            else f"Karp-Flatt at p={pmax} is non-positive ({kf:.3f}): speedup is at or above ideal."
        )
    out.append("")
    return out


def _load_table(rows, with_exp):
    head = "| clients | requests | errors | req/s | p50, ms | p95, ms | p99, ms | mean server time_ms |"
    sep = "|---:|---:|---:|---:|---:|---:|---:|---:|"
    if with_exp:
        head += " llm | fallback |"
        sep += "---:|---:|"
    out = [head, sep]
    for r in rows:
        line = (
            f"| {r['clients']} | {r['requests']} | {r['errors']} | {r['throughput_rps']} | "
            f"{r['p50_ms']} | {r['p95_ms']} | {r['p99_ms']} | {r['mean_server_time_ms'] or 'n/a'} |"
        )
        if with_exp:
            line += f" {r['expansion_llm']} | {r['expansion_fallback']} |"
        out.append(line)
    return out + [""]


def load_section(kw, llm):
    out = ["## Throughput vs concurrent clients", ""]
    if kw:
        kw = sorted(kw, key=lambda r: int(r["clients"]))
        out += ["### Keyword only (`expand=false`)", ""] + _load_table(kw, False)
        peak = max(kw, key=lambda r: float(r["throughput_rps"]))
        pk = float(peak["throughput_rps"])
        sat = next(r for r in kw if float(r["throughput_rps"]) >= 0.95 * pk)
        out.append(
            f"Peak throughput: **{pk:.1f} req/s** at {peak['clients']} clients; "
            f"95% of the peak is first reached at **{sat['clients']}** clients (saturation point)."
        )
        out.append("")
    else:
        out += ["_No keyword-only data (step skipped)._", ""]
    if llm:
        llm = sorted(llm, key=lambda r: int(r["clients"]))
        out += ["### With query expansion (`expand=true`)", ""]
        tot_llm = sum(int(r["expansion_llm"]) for r in llm)
        tot_fb = sum(int(r["expansion_fallback"]) for r in llm)
        out.append(
            f"Responses with expansion `llm`: {tot_llm}, `fallback`: {tot_fb}. "
            + (
                "**Ollama was NOT serving the model: this run measures the fallback path (timeouts / connection errors), not real LLM expansion.**"
                if tot_llm == 0
                else (
                    "Mixed run: some expansions fell back, treat with care."
                    if tot_fb
                    else "All expansions came from the LLM."
                )
            )
        )
        out.append("")
        out += _load_table(llm, True)
        pk = max(float(r["throughput_rps"]) for r in llm)
        out.append(f"Peak throughput with expansion: **{pk:.2f} req/s**.")
        out.append("")
        if kw:
            kwd = {int(r["clients"]): float(r["throughput_rps"]) for r in kw}
            out += [
                "| clients | keyword-only req/s | with-LLM req/s | ratio keyword / LLM |",
                "|---:|---:|---:|---:|",
            ]
            for r in llm:
                c = int(r["clients"])
                if c in kwd and float(r["throughput_rps"]) > 0:
                    out.append(
                        f"| {c} | {kwd[c]:.1f} | {float(r['throughput_rps']):.2f} | {kwd[c] / float(r['throughput_rps']):.1f}x |"
                    )
            out.append("")
    else:
        out += ["_No query-expansion data (step skipped)._", ""]
    return out


def ceiling_section(rows):
    if not rows:
        return []
    out = [
        "## Load generator ceiling check",
        "",
        "Same load generator against `GET /api/health` (trivial handler). If this is much "
        "faster than search, the client is not the bottleneck.",
        "",
        "| clients | req/s | p50, ms | p95, ms | client CPU, % of one core |",
        "|---:|---:|---:|---:|---:|",
    ]
    for r in rows:
        out.append(
            f"| {r['clients']} | {r['throughput_rps']} | {r['p50_ms']} | {r['p95_ms']} | {r['client_cpu_pct']} |"
        )
    return out + [""]


def quality_section(path):
    if not os.path.exists(path):
        return []
    with open(path, encoding="utf-8") as fh:
        return ["## Search quality (10 fixed queries)", "", fh.read().rstrip(), ""]


def build_report(results):
    j = lambda n: os.path.join(results, n)
    out = [
        "# Benchmark report",
        "",
        "_Generated by `bench/report.py` from the CSV files in this directory._",
        "",
    ]
    if os.path.exists(j(common.ENV_JSON)):
        with open(j(common.ENV_JSON), encoding="utf-8") as fh:
            env = json.load(fh)
        out += ["## Environment", "", "```json", json.dumps(env, indent=2), "```", ""]
        out += [
            "_Add CPU model, RAM, OS and Docker limits by hand (see bench/README.md)._",
            "",
        ]
    out += index_section(common.read_csv(j(common.INDEX_CSV)))
    out += load_section(
        common.read_csv(j(common.KEYWORD_CSV)), common.read_csv(j(common.LLM_CSV))
    )
    out += ceiling_section(common.read_csv(j(common.CEILING_CSV)))
    out += quality_section(j(common.QUALITY_MD))
    path = j(common.REPORT_MD)
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(out) + "\n")
    print("report:", path)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", default=common.DEFAULT_RESULTS)
    build_report(ap.parse_args().results)
