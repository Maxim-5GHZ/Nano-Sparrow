#!/usr/bin/env python3
"""Графики по results.json (matplotlib, headless Agg).

Генерирует в /results:
  rps_<w>.png        — rps по сценариям (группированные бары, лог-шкала)
  latency_<w>.png    — p99 латентность по сценариям
  cpu_<w>.png        — средний CPU% серверов во время прогонов
  memory_<w>.png     — память контейнеров: idle/peak RSS (anon) и page cache
  badnet.png         — влияние плохого соединения (base/latency/jitter/loss)
  sweep.png          — rps в зависимости от числа соединений (1..1024)
  memory_conns.png   — research: idle-память и rps /status при max_connections
"""
import json
import os

import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt  # noqa: E402
import numpy as np  # noqa: E402

RESULTS = os.environ.get("RESULTS_DIR", "/results")
SERVER_COLORS = {"sparrow": "#d62728", "caddy": "#2ca02c", "nginx": "#ff7f0e"}
SERVER_ORDER = ["sparrow", "caddy", "nginx"]

SCENARIO_LABELS = {
    "status": "status JSON", "status_noka": "без keep-alive", "tls_status": "TLS /status",
    "gzip_mid": "gzip 1.3КБ", "static_small": "файл 86Б", "static_big": "файл 10МБ",
    "proxy": "прокси GET", "proxy_post": "прокси POST",
    "badnet_base": "base c=32", "badnet_latency": "latency 50мс",
    "badnet_jitter": "jitter 20мс", "badnet_loss": "loss 2%",
}
WORKER_LINESTYLE = {"1": "-", "3": "--"}


def _load():
    path = os.path.join(RESULTS, "results.json")
    with open(path) as f:
        return json.load(f)


def _rows(data, scenario_names=None, workers=None):
    out = []
    for run in data["runs"]:
        if scenario_names is not None and run["scenario"] not in scenario_names:
            continue
        if workers is not None and str(run["workers"]) != str(workers):
            continue
        out.append(run)
    return out


def _bar_groups(rows, key, value):
    """{scenario: {server: value}} — для группированных баров."""
    groups = {}
    for run in rows:
        r = run["result"] or {}
        v = value(r)
        if v is None:
            continue
        groups.setdefault(run["scenario"], {})[run["server"]] = v
    return groups


def _save(fig, name):
    path = os.path.join(RESULTS, name)
    fig.savefig(path, dpi=130, bbox_inches="tight")
    plt.close(fig)
    print(f"[plot] {path}")


def human_format(num, is_cpu=False):
    """124345 -> '124k', 1500000 -> '1.5M' — подписи не слипаются."""
    if num is None:
        return ""
    if is_cpu:
        return f"{num:.0f}%"
    if num >= 1_000_000:
        return f"{num / 1_000_000:.1f}M"
    if num >= 10_000:
        return f"{num / 1000:.0f}k"
    if num >= 1_000:
        return f"{num / 1000:.1f}k"
    return f"{num:.0f}"


def _bars(fig, ax, groups, workers, ylabel, log=False, cpu=False):
    scenarios = list(groups.keys())
    if not scenarios:
        ax.set_ylabel(ylabel)
        ax.set_title(f"wrk: {ylabel}, WORKERS={workers} (нет данных)")
        return
    x = np.arange(len(scenarios))
    width = 0.8 / len(SERVER_ORDER)
    max_val = 0
    for i, srv in enumerate(SERVER_ORDER):
        vals = [groups[sc].get(srv) for sc in scenarios]
        ax.bar(x + (i - len(SERVER_ORDER) / 2 + 0.5) * width, vals, width,
               label=srv, color=SERVER_COLORS[srv], log=log)
        for xi, v in zip(x + (i - len(SERVER_ORDER) / 2 + 0.5) * width, vals):
            if v is None:
                continue
            max_val = max(max_val, v)
            if v > 1 or log:
                ax.annotate(human_format(v, cpu), (xi, v), ha="center", va="bottom",
                            fontsize=7.5, rotation=45, xytext=(0, 2),
                            textcoords="offset points")
    ax.set_xticks(x)
    ax.set_xticklabels([SCENARIO_LABELS.get(s, s) for s in scenarios], rotation=25, ha="right")
    ax.set_ylabel(ylabel)
    if log:
        ax.set_yscale("log")
        ax.set_ylim(bottom=max(1, min(v for g in groups.values() for v in g.values() if v) * 0.5))
        if max_val > 0:
            ax.set_ylim(top=max_val * 4.0)
    elif max_val > 0:
        ax.set_ylim(top=max_val * 1.15)
    ax.legend(ncol=4, fontsize=9, loc="lower center", bbox_to_anchor=(0.5, 1.02))
    ax.grid(axis="y", alpha=0.3, linestyle="--")
    ax.set_title(f"wrk: {ylabel}, WORKERS={workers}")


def plot_per_worker(data):
    for workers in data["meta"]["workers"]:
        rows = _rows(data, workers=workers)
        for scenario in ["badnet_base", "badnet_latency", "badnet_jitter", "badnet_loss"]:
            rows = [r for r in rows if r["scenario"] != scenario]

        fig, ax = plt.subplots(figsize=(12, 5.5))
        groups = _bar_groups(rows, "scenario", lambda r: r.get("rps"))
        _bars(fig, ax, groups, workers, "Requests/sec", log=True)
        _save(fig, f"rps_{workers}.png")

        fig, ax = plt.subplots(figsize=(12, 5.5))
        groups = _bar_groups(rows, "scenario", lambda r: r.get("lat_p99_ms"))
        _bars(fig, ax, groups, workers, "p99 latency, ms")
        _save(fig, f"latency_{workers}.png")

        fig, ax = plt.subplots(figsize=(12, 5.5))
        groups = {}
        for run in rows:
            if run.get("cpu") and run["cpu"].get("cpu_avg") is not None:
                groups.setdefault(run["scenario"], {})[run["server"]] = run["cpu"]["cpu_avg"]
        _bars(fig, ax, groups, workers, "CPU% (средний за прогон)", cpu=True)
        _save(fig, f"cpu_{workers}.png")


def plot_badnet(data):
    names = [f"badnet_{s}" for s in ("base", "latency", "jitter", "loss")]
    fig, axes = plt.subplots(len(data["meta"]["workers"]), 1,
                             figsize=(10, 3.5 * len(data["meta"]["workers"])), squeeze=False)
    for ax, workers in zip(axes[:, 0], data["meta"]["workers"]):
        rows = _rows(data, scenario_names=names, workers=workers)
        groups = _bar_groups(rows, "scenario", lambda r: r.get("rps"))
        _bars(fig, ax, groups, workers, "Requests/sec", log=True)
    _save(fig, "badnet.png")


def plot_sweep(data):
    if not data["sweep"]:
        return
    fig, ax = plt.subplots(figsize=(10, 5.5))
    conns_set = sorted({run["conns"] for run in data["sweep"]})
    for run in data["sweep"]:
        if not run["result"]:
            continue
        srv, w = run["server"], str(run["workers"])
        label = f"{srv} (WORKERS={w})"
        ax.plot(run["conns"], run["result"]["rps"], "o",
                color=SERVER_COLORS[srv], linestyle=WORKER_LINESTYLE.get(w, ":"),
                label=label, markersize=4)
    ax.set_xscale("log")
    if conns_set:
        ax.set_xticks(conns_set)
        ax.set_xticklabels([str(c) for c in conns_set])
    ax.set_xlabel("соединений (wrk -c)")
    ax.set_ylabel("Requests/sec")
    ax.grid(alpha=0.3, linestyle="--")
    ax.legend(fontsize=9, ncol=2, loc="upper left")
    ax.set_title("rps от конкарренси (plain /status)")
    _save(fig, "sweep.png")


def plot_memory(data):
    labels = {"bench-sparrow": "sparrow http", "bench-sparrow-tls": "sparrow tls"}
    for workers in data["meta"]["workers"]:
        section = (data.get("memory") or {}).get(str(workers))
        if not section:
            continue
        names = list(section.keys())
        keys = [("idle_anon_mb", "idle RSS (anon)"),
                ("peak_anon_mb", "peak RSS (anon)"),
                ("peak_cache_mb", "peak page cache")]
        fig, ax = plt.subplots(figsize=(11, 5.5))
        x = np.arange(len(names))
        width = 0.8 / len(keys)
        max_val = 0
        for i, (key, label) in enumerate(keys):
            vals = [section[n].get(key) for n in names]
            ax.bar(x + (i - len(keys) / 2 + 0.5) * width, vals, width, label=label)
            for xi, v in zip(x + (i - len(keys) / 2 + 0.5) * width, vals):
                if v is not None:
                    max_val = max(max_val, v)
                    ax.annotate(f"{v:,.0f}", (xi, v), ha="center", va="bottom",
                                fontsize=7.5, rotation=45, xytext=(0, 2),
                                textcoords="offset points")
        ax.set_xticks(x)
        ax.set_xticklabels([labels.get(n, n.replace("bench-", "")) for n in names],
                           rotation=15, ha="right")
        ax.set_ylabel("МБ")
        if max_val > 0:
            ax.set_ylim(top=max_val * 1.15)
        ax.legend(ncol=3, fontsize=9, loc="lower center", bbox_to_anchor=(0.5, 1.02))
        ax.grid(axis="y", alpha=0.3, linestyle="--")
        ax.set_title(f"Память контейнеров, WORKERS={workers} "
                     "(RSS = anon без page cache)")
        _save(fig, f"memory_{workers}.png")


def plot_memory_conns(data):
    """Research-фаза: idle-память и rps /status при разных max_connections."""
    by_conns = data.get("memory_by_conns") or {}
    if not by_conns:
        return
    conns_sorted = sorted(by_conns, key=int)
    labels = {"bench-sparrow": "sparrow http", "bench-sparrow-tls": "sparrow tls"}
    names = list(next(iter(by_conns.values())).keys())
    x = np.arange(len(conns_sorted))

    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 8), sharex=True)
    max_mem = 0
    for n in names:
        vals = [by_conns[c][n].get("idle_anon_mb") for c in conns_sorted]
        srv_key = labels.get(n, n.replace("bench-", "")).split()[0]
        color = SERVER_COLORS.get(srv_key, None)
        ax1.plot(x, vals, "o-", label=labels.get(n, n.replace("bench-", "")),
                 color=color, markersize=5)
        for xi, v in zip(x, vals):
            if v is not None:
                max_mem = max(max_mem, v)
                ax1.annotate(f"{v:.0f}", (xi, v), ha="center", va="bottom",
                             fontsize=7.5, rotation=45, xytext=(0, 2),
                             textcoords="offset points")
    ax1.set_ylabel("idle anon RSS, МБ")
    ax1.set_title("Sparrow преаллоцирует пул соединений — память растёт "
                  "с max_connections; остальные аллоцируют по требованию")
    if max_mem > 0:
        ax1.set_ylim(top=max_mem * 1.25)
    ax1.legend(ncol=len(names), fontsize=9, loc="lower center", bbox_to_anchor=(0.5, 1.02))
    ax1.grid(alpha=0.3, linestyle="--")

    groups = {}
    for run in data.get("config_runs", []):
        if run["scenario"] != "status":
            continue
        groups.setdefault(str(run["max_conns"]), {})[run["server"]] = \
            (run["result"] or {}).get("rps")
    for srv in SERVER_ORDER:
        vals = [groups.get(c, {}).get(srv) for c in conns_sorted]
        ax2.plot(x, vals, "o-", label=srv, color=SERVER_COLORS[srv], markersize=5)
    ax2.set_xticks(x)
    ax2.set_xticklabels([f"{int(c):,}".replace(",", " ") for c in conns_sorted])
    ax2.set_xlabel("max_connections")
    ax2.set_ylabel("rps (/status)")
    ax2.legend(ncol=4, fontsize=9, loc="lower center", bbox_to_anchor=(0.5, 1.02))
    ax2.grid(alpha=0.3, linestyle="--")
    _save(fig, "memory_conns.png")


def plot_sparrow_only(data):
    """Фаза «чистый sparrow»: rps/p99 по сценариям и sweep при max_connections
    (лимит — в конфиге sparrow; точки c > max_connections помечаются)."""
    so = data.get("sparrow_only") or {}
    if not so:
        return
    conns = sorted(so, key=int)
    conn_colors = {c: col for c, col in zip(conns, ["#1f77b4", "#d62728", "#2ca02c", "#ff7f0e"])}

    scenarios = []
    for c in conns:
        for run in so[c]["runs"]:
            if run["scenario"] not in scenarios:
                scenarios.append(run["scenario"])

    def values(sc, key):
        out = []
        for c in conns:
            v = None
            for run in so[c]["runs"]:
                if run["scenario"] == sc:
                    v = (run["result"] or {}).get(key)
            out.append(v)
        return out

    fig, (ax1, ax2, ax3) = plt.subplots(3, 1, figsize=(11, 13))
    x = np.arange(len(scenarios))
    width = 0.8 / len(conns)
    for i, c in enumerate(conns):
        vals = [values(sc, "rps")[i] for sc in scenarios]
        ax1.bar(x + (i - len(conns) / 2 + 0.5) * width, vals, width,
                label=f"max_connections={c}", color=conn_colors[c], log=True)
        for xi, v in zip(x + (i - len(conns) / 2 + 0.5) * width, vals):
            if v and v > 1:
                ax1.annotate(human_format(v), (xi, v), ha="center", va="bottom",
                             fontsize=7, rotation=45, xytext=(0, 2),
                             textcoords="offset points")
    ax1.set_xticks(x)
    ax1.set_xticklabels([SCENARIO_LABELS.get(s, s) for s in scenarios],
                        rotation=25, ha="right")
    ax1.set_yscale("log")
    ax1.set_ylabel("Requests/sec")
    ax1.set_title("Чистый Sparrow (WORKERS=1): rps по сценариям")
    ax1.legend(ncol=len(conns), fontsize=9, loc="lower center", bbox_to_anchor=(0.5, 1.02))
    ax1.grid(axis="y", alpha=0.3, linestyle="--")

    for i, c in enumerate(conns):
        vals = [values(sc, "lat_p99_ms")[i] for sc in scenarios]
        ax2.bar(x + (i - len(conns) / 2 + 0.5) * width, vals, width,
                label=f"max_connections={c}", color=conn_colors[c])
        for xi, v in zip(x + (i - len(conns) / 2 + 0.5) * width, vals):
            if v is not None:
                ax2.annotate(f"{v:.1f}", (xi, v), ha="center", va="bottom",
                             fontsize=7, rotation=45, xytext=(0, 2),
                             textcoords="offset points")
    ax2.set_xticks(x)
    ax2.set_xticklabels([SCENARIO_LABELS.get(s, s) for s in scenarios],
                        rotation=25, ha="right")
    ax2.set_ylabel("p99 latency, ms")
    ax2.set_title("Чистый Sparrow (WORKERS=1): p99 latency по сценариям")
    ax2.legend(ncol=len(conns), fontsize=9, loc="lower center", bbox_to_anchor=(0.5, 1.02))
    ax2.grid(axis="y", alpha=0.3, linestyle="--")

    sweep_conns = sorted({r["conns"] for c in conns for r in so[c]["sweep"]})
    for c in conns:
        runs = sorted(so[c]["sweep"], key=lambda r: r["conns"])
        xs, ys, limits = [], [], []
        for r in runs:
            if not r["result"]:
                continue
            xs.append(r["conns"])
            ys.append(r["result"]["rps"])
            limits.append(bool(r.get("pool_limit")))
        ax3.plot(xs, ys, "o-", label=f"max_connections={c}",
                 color=conn_colors[c], markersize=5)
        for xi, yi, lim in zip(xs, ys, limits):
            if lim:
                ax3.annotate("потолок пула", (xi, yi), ha="center", va="bottom",
                             fontsize=7, xytext=(0, 4), textcoords="offset points")
    ax3.set_xscale("log")
    if sweep_conns:
        ax3.set_xticks(sweep_conns)
        ax3.set_xticklabels([str(c) for c in sweep_conns])
    ax3.set_xlabel("соединений (wrk -c)")
    ax3.set_ylabel("Requests/sec")
    ax3.set_title("Чистый Sparrow: rps от конкарренси "
                  "(лимит соединений — в конфиге sparrow)")
    ax3.grid(alpha=0.3, linestyle="--")
    ax3.legend(fontsize=9, loc="upper left")

    _save(fig, "sparrow_only.png")


def main():
    data = _load()
    if data.get("runs"):
        plot_per_worker(data)
        plot_badnet(data)
        plot_sweep(data)
        plot_memory(data)
    plot_memory_conns(data)
    plot_sparrow_only(data)
    print("[plot] готово")


if __name__ == "__main__":
    main()
