#!/usr/bin/env python3
"""ASCII-сводка результатов бенчмарка для stdout (печатается в конце run.sh).

Таблицы: rps и p99 по сценариям × серверы для каждого набора WORKERS,
память при max_connections (research-фаза) и список сгенерированных графиков.
"""
import json
import os
import sys

RESULTS = os.environ.get("RESULTS_DIR") or os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "results")
SERVER_ORDER = ["sparrow", "caddy", "nginx"]

SCENARIO_LABELS = {
    "status": "status JSON", "status_noka": "no keep-alive", "tls_status": "TLS /status",
    "gzip_mid": "gzip 1.3K", "static_small": "file 86B", "static_big": "file 10MB",
    "proxy": "proxy GET", "proxy_post": "proxy POST",
    "badnet_base": "badnet base", "badnet_latency": "badnet latency",
    "badnet_jitter": "badnet jitter", "badnet_loss": "badnet loss",
}


def _load():
    path = os.path.join(RESULTS, "results.json")
    with open(path) as f:
        return json.load(f)


def _fmt(value, width=11, dec=1):
    """120450 -> '120 450'; None -> прочерки; dec — знаков после точки для <1000."""
    if value is None:
        return "-" * width
    s = f"{value:,.{dec}f}" if value < 1000 else f"{value:,.0f}"
    return s.replace(",", " ").rjust(width)


def _table(data, workers, key="rps", suffix="", dec=1):
    runs = [r for r in data["runs"] if str(r["workers"]) == str(workers) and r["result"]]
    scenarios = []
    for r in runs:
        if r["scenario"] not in scenarios:
            scenarios.append(r["scenario"])
    header = "  " + "Сценарий".ljust(16) + "".join(s.rjust(11) for s in SERVER_ORDER) + suffix
    print("  " + "-" * (len(header) - 2))
    print(header)
    print("  " + "-" * (len(header) - 2))
    for sc in scenarios:
        cells = {}
        for r in runs:
            if r["scenario"] == sc:
                cells[r["server"]] = (r["result"] or {}).get(key)
        row = SCENARIO_LABELS.get(sc, sc).ljust(16)
        row += "".join(_fmt(cells.get(s), dec=dec) for s in SERVER_ORDER)
        print("  " + row)
    print("  " + "-" * (len(header) - 2))


def _mem_table(data, workers):
    section = (data.get("memory") or {}).get(str(workers))
    if not section:
        return
    labels = {"bench-sparrow": "sparrow http", "bench-sparrow-tls": "sparrow tls"}
    rows = [(labels.get(n, n.replace("bench-", "")), m) for n, m in section.items()]

    def f(value):
        return "-" * 9 if value is None else f"{value:,.0f}".replace(",", " ").rjust(8) + " MB"

    header = "  " + "Контейнер".ljust(16) + \
        "idle RSS".rjust(12) + "idle usg".rjust(12) + \
        "peak RSS".rjust(12) + "peak usg".rjust(12) + "peak cache".rjust(14)
    print("  " + "-" * (len(header) - 2))
    print(header)
    print("  " + "-" * (len(header) - 2))
    for label, m in rows:
        print(f"  {label.ljust(16)}"
              + f(m.get("idle_anon_mb")).rjust(12)
              + f(m.get("idle_usage_mb")).rjust(12)
              + f(m.get("peak_anon_mb")).rjust(12)
              + f(m.get("peak_usage_mb")).rjust(12)
              + f(m.get("peak_cache_mb")).rjust(14))
    print("  " + "-" * (len(header) - 2))
    print("  RSS = anon из cgroup (без page cache); usage = anon + cache + kernel.")
    print("  peak = максимум за все сценарии матрицы; idle = до нагрузки.")


def _mem_conns_table(data):
    """Research-фаза: idle anon RSS при разных max_connections."""
    by_conns = data.get("memory_by_conns") or {}
    if not by_conns:
        return
    conns = sorted(by_conns, key=int)
    labels = {"bench-sparrow": "sparrow http", "bench-sparrow-tls": "sparrow tls"}
    names = list(next(iter(by_conns.values())).keys())

    header = "  " + "Контейнер".ljust(16) + "".join(f"max={c}".rjust(12) for c in conns)
    print("  " + "-" * (len(header) - 2))
    print("  " + "idle anon RSS (МБ) при max_connections".ljust(len(header) - 2))
    print(header)
    print("  " + "-" * (len(header) - 2))
    for n in names:
        vals = [by_conns[c][n].get("idle_anon_mb") for c in conns]
        row = labels.get(n, n.replace("bench-", "")).ljust(16)
        row += "".join(_fmt(v, 12) for v in vals)
        print("  " + row)
    print("  " + "-" * (len(header) - 2))
    print("  Sparrow преаллоцирует пул соединений (буферы при старте) — память растёт")
    print("  линейно с max_connections; nginx/caddy аллоцируют по требованию.")


def _sparrow_table(data):
    """Фаза «чистый sparrow»: полная матрица только sparrow при max_connections."""
    so = data.get("sparrow_only") or {}
    if not so:
        return
    conns = sorted(so, key=int)
    labels = {"bench-sparrow": "sparrow http", "bench-sparrow-tls": "sparrow tls"}
    scenarios = []
    for c in conns:
        for run in so[c]["runs"]:
            if run["scenario"] not in scenarios:
                scenarios.append(run["scenario"])

    print()
    print(f"  [ ЧИСТЫЙ SPARROW ] полная матрица, WORKERS=1 "
          f"(nginx/caddy остановлены)")
    for key, title, dec in (("rps", "RPS (запросов в секунду)", 0),
                            ("lat_p99_ms", "p99 Latency (мс)", 1)):
        header = "  " + "Сценарий".ljust(16) + "".join(f"max={c}".rjust(12) for c in conns)
        print("  " + "-" * (len(header) - 2))
        print(f"  {title}".ljust(len(header)))
        print(header)
        print("  " + "-" * (len(header) - 2))
        for sc in scenarios:
            cells = {}
            for c in conns:
                for run in so[c]["runs"]:
                    if run["scenario"] == sc:
                        cells[c] = (run["result"] or {}).get(key)
            row = SCENARIO_LABELS.get(sc, sc).ljust(16)
            row += "".join(_fmt(cells.get(c), 12, dec) for c in conns)
            print("  " + row)
        print("  " + "-" * (len(header) - 2))

    header = "  " + "idle anon RSS (МБ)".ljust(16) + \
        "".join(f"max={c}".rjust(12) for c in conns)
    print("  " + "-" * (len(header) - 2))
    print(header)
    print("  " + "-" * (len(header) - 2))
    names = list(next(iter(so.values()))["memory"].keys())
    for n in names:
        vals = [so[c]["memory"][n].get("idle_anon_mb") for c in conns]
        row = labels.get(n, n.replace("bench-", "")).ljust(16)
        row += "".join(_fmt(v, 12) for v in vals)
        print("  " + row)
    print("  " + "-" * (len(header) - 2))
    print("  Лимит соединений задаётся в КОНФИГЕ sparrow (max_connections); при")
    print("  c > max_connections sparrow отклоняет лишние соединения, wrk считает")
    print("  их ошибками — точки sweep c=1024 при max=1000 показывают потолок пула.")


def main():
    data = _load()
    meta = data["meta"]
    print()
    print("=" * 66)
    print("  РЕЗУЛЬТАТЫ БЕНЧМАРКА: Nano-Sparrow vs Caddy vs Nginx")
    print(f"  Дата: {meta.get('date', '')} | git: {meta.get('git_rev', '?')} | ядро: {meta.get('host_kernel', '?')}")
    print("=" * 66)

    if data.get("runs"):
        for workers in data["meta"]["workers"]:
            print()
            print(f"  [ WORKERS = {workers} ] RPS (запросов в секунду)")
            _table(data, workers, "rps", dec=0)

            print()
            print(f"  [ WORKERS = {workers} ] p99 Latency (мс)")
            _table(data, workers, "lat_p99_ms", dec=1)

            print()
            print(f"  [ WORKERS = {workers} ] Память (МБ)")
            _mem_table(data, workers)
    else:
        print()
        print("  (сравнительная матрица не гонялась — только sparrow-only)")

    _mem_conns_table(data)
    _sparrow_table(data)

    pngs = sorted(f for f in os.listdir(RESULTS) if f.endswith(".png"))
    if pngs:
        print()
        print("  Графики (откройте картинки из папки tests/bench/results/):")
        for p in pngs:
            print(f"    - {p}")
    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
