"""Closed-loop HTTP load generator (multiprocessing + threads, stdlib http.client).

Every client is a thread with its own keep-alive connection that sends the next
request as soon as the previous response has arrived. Clients are spread over
several worker processes so that the Python GIL of one process is not the limit.
"""

import http.client
import json
import multiprocessing as mp
import os
import resource
import threading
import time


def _client(host, port, method, path, bodies, offset, t_measure, t_end, out, tmo):
    """Runs until t_end; appends (finish_ts, latency_s, ok, server_ms, expansion)."""
    conn = None
    i = offset
    while time.time() < t_end:
        body = bodies[i % len(bodies)] if bodies else None
        i += 1
        try:
            if conn is None:
                conn = http.client.HTTPConnection(host, port, timeout=tmo)
            t0 = time.perf_counter()
            conn.request(
                method,
                path,
                body=body,
                headers={"Content-Type": "application/json"} if body else {},
            )
            resp = conn.getresponse()
            raw = resp.read()
            lat = time.perf_counter() - t0
            ok = resp.status == 200
            server_ms, expansion = None, ""
            if ok and body:
                try:
                    js = json.loads(raw)
                    server_ms = js.get("time_ms")
                    expansion = js.get("expansion", "")
                except ValueError:
                    ok = False
            if resp.getheader("Connection", "").lower() == "close":
                conn.close()
                conn = None
        except Exception:
            lat, ok, server_ms, expansion = 0.0, False, None, ""
            if conn is not None:
                conn.close()
            conn = None
            time.sleep(0.01)
        fin = time.time()
        if fin >= t_measure:
            out.append((fin, lat, ok, server_ms, expansion))
    if conn is not None:
        conn.close()


def _worker(args):
    host, port, method, path, bodies, n, first_id, t_measure, t_end, tmo = args
    t_start = time.time()
    results, threads = [], []
    cpu0 = resource.getrusage(resource.RUSAGE_SELF)
    for k in range(n):
        out = []
        results.append(out)
        th = threading.Thread(
            target=_client,
            args=(
                host,
                port,
                method,
                path,
                bodies,
                (first_id + k) * 7,
                t_measure,
                t_end,
                out,
                tmo,
            ),
        )
        th.start()
        threads.append(th)
    for th in threads:
        th.join()
    cpu1 = resource.getrusage(resource.RUSAGE_SELF)
    cpu_s = (cpu1.ru_utime + cpu1.ru_stime) - (cpu0.ru_utime + cpu0.ru_stime)
    return [r for out in results for r in out], cpu_s


def percentile(sorted_vals, q):
    if not sorted_vals:
        return 0.0
    k = (len(sorted_vals) - 1) * q
    lo = int(k)
    hi = min(lo + 1, len(sorted_vals) - 1)
    return sorted_vals[lo] + (sorted_vals[hi] - sorted_vals[lo]) * (k - lo)


def run_level(
    host, port, method, path, bodies, clients, duration, warmup, procs=None, timeout=60
):
    """Run `clients` closed-loop clients for `duration` s after `warmup` s."""
    cpus = os.cpu_count() or 1
    procs = max(1, min(clients, procs or max(2, cpus)))
    split = [clients // procs + (1 if i < clients % procs else 0) for i in range(procs)]
    t_begin = time.time() + 1.0  # let processes start and connect
    t_measure = t_begin + warmup
    t_end = t_measure + duration
    jobs, first = [], 0
    for n in split:
        jobs.append(
            (host, port, method, path, bodies, n, first, t_measure, t_end, timeout)
        )
        first += n
    ctx = mp.get_context("fork" if "fork" in mp.get_all_start_methods() else "spawn")
    with ctx.Pool(procs) as pool:
        res = pool.map(_worker, jobs, chunksize=1)
    rows = [r for part, _ in res for r in part if r[0] <= t_end]
    cpu_s = sum(c for _, c in res)
    good = sorted(r[1] * 1000.0 for r in rows if r[2])
    errors = sum(1 for r in rows if not r[2])
    srv = [r[3] for r in rows if r[2] and r[3] is not None]
    exp = [r[4] for r in rows if r[2]]
    wall = warmup + duration + 1.0
    return {
        "clients": clients,
        "requests": len(good),
        "errors": errors,
        "duration_s": duration,
        "throughput_rps": round(len(good) / duration, 2),
        "p50_ms": round(percentile(good, 0.50), 2),
        "p95_ms": round(percentile(good, 0.95), 2),
        "p99_ms": round(percentile(good, 0.99), 2),
        "mean_server_time_ms": round(sum(srv) / len(srv), 2) if srv else "",
        "expansion_llm": exp.count("llm"),
        "expansion_fallback": exp.count("fallback"),
        "expansion_disabled": exp.count("disabled"),
        # client CPU seconds over the whole run, as % of one core
        "client_cpu_pct": round(100.0 * cpu_s / wall, 1),
    }
