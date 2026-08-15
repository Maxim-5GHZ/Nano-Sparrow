#!/usr/bin/env python3
"""Оркестратор сравнительного бенчмарка Nano-Sparrow vs Caddy vs Nginx.

Запускается ВНУТРИ контейнера bench-runner (docker.sock + NET_ADMIN):
  1. генерирует ассеты (сертификаты, тестовые файлы) и конфиги серверов
  2. пересоздаёт сервисы compose с нужным числом воркеров (WORKERS=1,3,...)
  3. гоняет wrk по матрице сценариев (включая tc netem для плохого соединения)
  4. собирает docker stats (CPU) + Docker API (память: anon RSS / page cache)
     во время каждого прогона, плюс отдельный idle-замер до нагрузки
  5. пишет results.json/results.csv (раздел "memory") и рендерит графики plot.py

Примеры:
  python /bench/bench/run_bench.py
  python /bench/bench/run_bench.py --workers "1 3 4" --duration 10
  python /bench/bench/run_bench.py --scenarios status,tls_status,proxy --no-sweep
  python /bench/bench/run_bench.py --only-sparrow --sparrow-conns "1000 5000"
"""
import argparse
import csv
import json
import os
import random
import subprocess
import sys
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import metrics  # noqa: E402
import netem  # noqa: E402
import summary  # noqa: E402
import wrk  # noqa: E402
from scenarios import BADNET, DEFAULT_DURATION, SCENARIOS, SWEEP_CONNS, SWEEP_DURATION, SWEEP_URL  # noqa: E402
from servers import CONTAINER_NAMES, SERVERS  # noqa: E402

BENCH_DIR = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(BENCH_DIR)  # tests/bench
COMPOSE = os.path.join(ROOT, "docker-compose.yml")
RESULTS = os.environ.get("RESULTS_DIR", "/results")
SERVER_SERVICES = ["backend", "sparrow", "sparrow-tls", "caddy", "nginx"]

WRK_THREADS = 4


def log(msg):
    print(f"[bench] {msg}", flush=True)


def sh(cmd, env=None, check=True):
    proc = subprocess.run(cmd, capture_output=True, text=True, env=env)
    if check and proc.returncode != 0:
        raise RuntimeError(f"cmd {cmd} failed: {proc.stderr[-2000:]}")
    return proc


# ---------------- Ассеты ----------------

def gen_assets():
    cert_dir = os.path.join(ROOT, "certs")
    www_dir = os.path.join(ROOT, "www")
    os.makedirs(cert_dir, exist_ok=True)
    os.makedirs(www_dir, exist_ok=True)

    cert, key = os.path.join(cert_dir, "cert.pem"), os.path.join(cert_dir, "key.pem")
    if not (os.path.exists(cert) and os.path.exists(key)):
        log("генерация самоподписанного сертификата")
        sh(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "3650",
            "-keyout", key, "-out", cert,
            "-subj", "/CN=bench.local",
            "-addext", "subjectAltName=DNS:bench.local,DNS:localhost,IP:127.0.0.1"])
    # openssl 3.x пишет приватный ключ с 0600 — nginx (uid 101) не сможет прочитать
    os.chmod(key, 0o644)
    os.chmod(cert, 0o644)

    fox = "the quick brown fox jumps over the lazy dog; "

    def write(name, data, mode="wb"):
        with open(os.path.join(www_dir, name), mode) as f:
            f.write(data)

    write("status.json", b'{"status":"ok","server":"bench"}')
    write("small.txt", b"hello" * 17 + b"\n")  # 86 Б
    text = (fox * 20000)[:1300]
    write("mid.txt", text.encode())  # 1.3 КБ
    text = (fox * 100000)[:1850000]
    write("big.txt", text.encode())  # 1.85 МБ, хорошо сжимается
    rng = random.Random(42)
    write("big.bin", rng.randbytes(10 * 1024 * 1024))  # 10 МБ, несжимаемый
    log("ассеты готовы: certs/ www/")


def materialize_configs(workers, max_conns):
    """__WORKERS__/__MAX_CONNECTIONS__ -> из *.tmpl в рабочие конфиги."""
    templates = ("configs/sparrow/server.conf", "configs/sparrow/server-tls.conf",
                 "configs/nginx/nginx.conf")
    for path in templates:
        with open(os.path.join(ROOT, path + ".tmpl")) as f:
            text = f.read()
        text = text.replace("__WORKERS__", str(workers))
        text = text.replace("__MAX_CONNECTIONS__", str(max_conns))
        with open(os.path.join(ROOT, path), "w") as f:
            f.write(text)


# ---------------- Compose / готовность ----------------

def host_project_dir():
    """Хост-путь к каталогу бенчмарка, где лежит docker-compose.yml.

    Внутренний `docker compose` резолвит относительные bind-mount'ы относительно
    --project-directory. Это путь в ФС ХОСТА (демон монтирует его сам), а не
    контейнерный /bench — его берём из /proc/self/mountinfo.
    """
    with open("/proc/self/mountinfo") as f:
        for line in f:
            parts = line.split()
            if len(parts) > 5 and parts[4] == "/bench":
                return parts[3].replace("\\040", " ")
    raise RuntimeError("не нашёл /bench в /proc/self/mountinfo")


def compose_up(workers, services=SERVER_SERVICES):
    log(f"пересоздание сервисов с WORKERS={workers}")
    env = dict(os.environ, WORKERS=str(workers))
    sh(["docker", "compose", "--project-directory", host_project_dir(),
        "-f", COMPOSE, "up", "-d", "--no-build", "--force-recreate"]
       + services, env=env)


def curl_ok(url, timeout=3):
    proc = subprocess.run(["curl", "-sk", "-o", "/dev/null", "-w", "%{http_code}",
                           "--max-time", str(timeout), url],
                          capture_output=True, text=True)
    return proc.stdout.strip() == "200"


def wait_ready(timeout=120, servers=SERVERS):
    deadline = time.time() + timeout
    pending = [(s["name"], u) for s in servers
               for u in (s["http"], s["https"])]
    while pending and time.time() < deadline:
        for name, url in list(pending):
            if curl_ok(url + "/status", timeout=2):
                log(f"{name}: {url.split('://')[0]} поднялся")
                pending.remove((name, url))
            else:
                time.sleep(0.5)
    if pending:
        raise SystemExit(f"FATAL: не дождались {pending}")
    log("все серверы (http+https) отвечают")


def ready_with_retry(workers, servers=SERVERS, services=SERVER_SERVICES):
    """wait_ready с самолечением: при таймауте сервисы пересоздаются ещё раз."""
    try:
        wait_ready(servers=servers)
    except SystemExit:
        log("не дождались серверов — пересоздаю сервисы и жду ещё раз")
        compose_up(workers, services=services)
        wait_ready(servers=servers)


def sanity_checks(servers=SERVERS):
    """Проверка правильности ответов. Проблема -> поднять флаг abort (3 ретрая)."""
    problems = []

    def check(label, fn):
        for _ in range(3):
            if fn():
                return
            time.sleep(1)
        problems.append(label)

    for s in servers:
        check(f"{s['name']}: /static/big.bin не 10 МБ",
              lambda s=s: subprocess.run(
                  ["curl", "-sk", "-o", "/dev/null", "-w", "%{size_download}",
                   "--max-time", "60", s["http"] + "/static/big.bin"],
                  capture_output=True, text=True).stdout == str(10 * 1024 * 1024))
        check(f"{s['name']}: /static/mid.txt не сжат gzip",
              lambda s=s: "gzip" in subprocess.run(
                  ["curl", "-sk", "-o", "/dev/null", "-w", "%{header_json}",
                   "-H", "Accept-Encoding: gzip", s["http"] + "/static/mid.txt"],
                  capture_output=True, text=True).stdout)

    if problems:
        for p in problems:
            log(f"FAIL: {p}")
        raise SystemExit("FATAL: sanity-проверки не пройдены — стенд сломан, "
                         "прогон отменён. Смотрите docker compose logs.")
    log("sanity-проверки пройдены (статика 10МБ цела, gzip работает)")


# ---------------- Прогоны ----------------

def bench_one(srv, base_url, scenario, duration, threads=WRK_THREADS,
              containers=CONTAINER_NAMES):
    """Один wrk-прогон с параллельным сбором docker stats серверов."""
    url = base_url + scenario["url"]
    sampler_result = {}

    def sampler():
        sampler_result["cpu"] = metrics.sample_containers(
            containers, duration + 6)

    thread = threading.Thread(target=sampler, daemon=True)
    thread.start()
    try:
        result = wrk.run(url, threads=threads, conns=scenario["conns"],
                         duration=duration,
                         headers=[scenario["header"]] if scenario.get("header") else None,
                         script=os.path.join(BENCH_DIR, scenario["script"])
                         if scenario.get("script") else None)
    finally:
        thread.join()
    return result, sampler_result.get("cpu", {})


def measure_idle_memory(seconds=6, containers=CONTAINER_NAMES):
    """Замер памяти всех контейнеров БЕЗ нагрузки (сразу после старта)."""
    log("замер памяти на холостом ходу (без нагрузки)")
    return metrics.sample_containers(containers, seconds)


def memory_section(workers, idle, runs, containers=CONTAINER_NAMES):
    """Сводит {container: {idle_*_mb, peak_*_mb}} для results.json.

    peak = максимум idle и всех прогонов матрицы (usage/anon/cache).
    Каждый run несёт `mem` — sampler-словарь своего контейнера.
    """
    section = {}
    for name in containers:
        idle_m = idle.get(name, {})
        peaks = {k: idle_m.get(k) for k in ("usage_peak_mb", "anon_peak_mb", "cache_peak_mb")}
        for run in runs:
            if run.get("container") != name:
                continue
            m = run.get("mem") or {}
            for k in peaks:
                if m.get(k) is not None and (peaks[k] is None or m[k] > peaks[k]):
                    peaks[k] = m[k]
        section[name] = {
            "idle_usage_mb": idle_m.get("usage_mb"),
            "idle_anon_mb": idle_m.get("anon_mb"),
            "idle_cache_mb": idle_m.get("cache_mb"),
            "peak_usage_mb": peaks["usage_peak_mb"],
            "peak_anon_mb": peaks["anon_peak_mb"],
            "peak_cache_mb": peaks["cache_peak_mb"],
        }
    return section


def run_matrix(workers, duration, scenario_names, do_badnet, do_sweep, sweep_duration,
               max_conns, servers=SERVERS, containers=CONTAINER_NAMES):
    runs, sweep = [], []
    failures = 0

    def run_scenario(scenario, conns_override=None, extra_headers=None, netem_desc="none"):
        nonlocal failures
        order = servers
        for i, srv in enumerate(order):
            base = srv["https"] if scenario.get("tls") else srv["http"]
            sc = dict(scenario, conns=conns_override or scenario["conns"])
            result, cpu = bench_one(srv, base, sc, duration, containers=containers)
            container = srv["container_tls" if scenario.get("tls") else "container"]
            entry = {
                "workers": workers, "max_conns": max_conns, "scenario": scenario["name"],
                "server": srv["name"], "url": base + scenario["url"],
                "conns": sc["conns"],
                "netem": netem_desc,
                "container": container,
                "cpu": (cpu.get(container) if cpu else None),
                "mem": (cpu.get(container) if cpu else None),
                "result": result,
            }
            runs.append(entry)
            if result is None:
                failures += 1
                log(f"  !! {srv['name']} {scenario['name']}: wrk не отработал")
            else:
                cpu_avg = entry["cpu"]["cpu_avg"] if entry["cpu"] else None
                mem_anon = (entry["mem"] or {}).get("anon_mb")
                log(f"  {srv['name']:8s} {scenario['name']:12s} "
                    f"rps={result['rps']:>9.0f}  "
                    f"p99={result.get('lat_p99_ms', 0):>8.2f}мс  "
                    f"cpu={cpu_avg if cpu_avg is not None else 0:>5.1f}%  "
                    f"rss={mem_anon if mem_anon is not None else 0:>6.1f}МБ")

    for idx, sc in enumerate(SCENARIOS):
        if scenario_names != "all" and sc["name"] not in scenario_names:
            continue
        log(f"[workers={workers}] сценарий: {sc['name']} ({sc['desc']})")
        run_scenario(sc)

    if do_badnet:
        for sc in BADNET:
            log(f"[workers={workers}] badnet: {sc['name']} ({sc['desc']})")
            if sc["tc"]:
                netem.apply(sc["tc"])
            else:
                netem.clear()
            try:
                run_scenario(dict(name=sc["name"], url="/status", conns=sc["conns"]),
                             netem_desc=", ".join(sc["tc"]) if sc["tc"] else "none")
            finally:
                netem.clear()

    if do_sweep:
        for conns in SWEEP_CONNS:
            log(f"[workers={workers}] sweep: /status c={conns}")
            for srv in servers:
                sc = dict(name="sweep", url=SWEEP_URL, conns=conns)
                result, cpu = bench_one(srv, srv["http"], sc, sweep_duration,
                                        threads=min(WRK_THREADS, conns),
                                        containers=containers)
                entry = {
                    "workers": workers, "max_conns": max_conns,
                    "server": srv["name"], "conns": conns,
                    "pool_limit": conns > max_conns,
                    "cpu": (cpu.get(srv["container"]) if cpu else None),
                    "result": result,
                }
                sweep.append(entry)
                if result is None:
                    failures += 1
    return runs, sweep, failures


def run_research(conns_list, duration):
    """Исследовательская фаза: для каждого max_connections — idle-память и /status.

    workers=1; данные пишутся в data["config_runs"] и data["memory_by_conns"],
    из них plot.py рисует memory_conns.png (память и rps от max_connections).
    """
    config_runs, mem_by_conns, failures = [], {}, 0
    for conns in conns_list:
        log(f"[research] max_connections={conns} (workers=1)")
        materialize_configs(1, conns)
        compose_up(1)
        ready_with_retry(1)
        idle_mem = measure_idle_memory()
        sc = next(s for s in SCENARIOS if s["name"] == "status")
        runs = []
        for srv in SERVERS:
            result, cpu = bench_one(srv, srv["http"], sc, duration)
            container = srv["container"]
            entry = {
                "workers": 1, "max_conns": conns, "scenario": sc["name"],
                "server": srv["name"], "url": srv["http"] + sc["url"],
                "conns": sc["conns"], "netem": "none",
                "container": container,
                "cpu": (cpu.get(container) if cpu else None),
                "mem": (cpu.get(container) if cpu else None),
                "result": result,
            }
            runs.append(entry)
            if result is None:
                failures += 1
                log(f"  !! {srv['name']} status: wrk не отработал (max_conns={conns})")
            else:
                log(f"  {srv['name']:8s} max_conns={conns:<6d} "
                    f"rps={result['rps']:>9.0f}")
        config_runs += runs
        mem_by_conns[str(conns)] = memory_section(1, idle_mem, runs)
    return config_runs, mem_by_conns, failures


SPARROW_SERVICES = ["backend", "sparrow", "sparrow-tls"]


def sparrow_servers():
    return [s for s in SERVERS if s["name"] == "sparrow"]


def sparrow_containers():
    ss = sparrow_servers()
    return sorted({s["container"] for s in ss} | {s["container_tls"] for s in ss})


def run_sparrow_only(conns_list, duration, do_badnet, do_sweep, sweep_duration):
    """Фаза «чистый sparrow»: полная матрица ТОЛЬКО для sparrow.

    Для каждого max_connections из conns_list значение пишется в КОНФИГ sparrow
    (max_connections, server.conf/server-tls.conf), nginx/caddy останавливаются.
    Sweep-точки с c > max_connections помечаются pool_limit — sparrow по конфигу
    отклоняет лишние соединения (wrk считает их ошибками, это потолок пула).
    Данные -> data["sparrow_only"] = {str(conns): {"runs", "sweep", "memory"}}.
    """
    ss, containers = sparrow_servers(), sparrow_containers()
    out, failures = {}, 0
    for conns in conns_list:
        log(f"[sparrow-only] max_connections={conns} (workers=1, только sparrow)")
        materialize_configs(1, conns)
        sh(["docker", "compose", "--project-directory", host_project_dir(),
            "-f", COMPOSE, "stop", "caddy", "nginx"], check=False)
        compose_up(1, services=SPARROW_SERVICES)
        ready_with_retry(1, servers=ss, services=SPARROW_SERVICES)
        sanity_checks(servers=ss)
        idle_mem = measure_idle_memory(containers=containers)
        runs, sweep, f = run_matrix(1, duration, "all", do_badnet, do_sweep,
                                    sweep_duration, conns, servers=ss,
                                    containers=containers)
        failures += f
        out[str(conns)] = {
            "runs": runs, "sweep": sweep,
            "memory": memory_section(1, idle_mem, runs, containers),
        }
    return out, failures


# ---------------- Сохранение ----------------

def save_json(data):
    os.makedirs(RESULTS, exist_ok=True)
    path = os.path.join(RESULTS, "results.json")
    with open(path, "w") as f:
        json.dump(data, f, indent=2)
    log(f"results.json сохранён ({path})")


def _csv_row(run, phase, sweep=False):
    r, cpu = run["result"], run["cpu"]
    m = run.get("mem") or {}
    return {
        "workers": run["workers"], "max_conns": run.get("max_conns", ""),
        "scenario": "sweep" if sweep else run["scenario"],
        "server": run["server"], "url": SWEEP_URL if sweep else run["url"],
        "conns": run["conns"] if (sweep or phase == "research") else "",
        "netem": "none" if sweep else run["netem"],
        "phase": phase,
        "pool_limit": run.get("pool_limit", ""),
        "rps": (r or {}).get("rps", ""), "transfer_mbps": (r or {}).get("transfer_mbps", ""),
        "lat_avg_ms": (r or {}).get("lat_avg_ms", ""), "lat_p50_ms": (r or {}).get("lat_p50_ms", ""),
        "lat_p99_ms": (r or {}).get("lat_p99_ms", ""), "lat_max_ms": (r or {}).get("lat_max_ms", ""),
        "cpu_avg": (cpu or {}).get("cpu_avg", ""), "cpu_max": (cpu or {}).get("cpu_max", ""),
        "rss_mb": (cpu or {}).get("rss_mb", ""),
        "mem_usage_mb": (m or {}).get("usage_mb", ""),
        "mem_anon_mb": (m or {}).get("anon_mb", ""),
        "mem_cache_mb": (m or {}).get("cache_mb", ""),
    }


def save_csv(data):
    rows = [_csv_row(run, "matrix") for run in data["runs"]]
    rows += [_csv_row(run, "matrix", sweep=True) for run in data["sweep"]]
    rows += [_csv_row(run, "research") for run in data.get("config_runs", [])]
    for section in data.get("sparrow_only", {}).values():
        rows += [_csv_row(run, "sparrow") for run in section["runs"]]
        rows += [_csv_row(run, "sparrow", sweep=True) for run in section["sweep"]]
    path = os.path.join(RESULTS, "results.csv")
    fieldnames = ["workers", "max_conns", "scenario", "server", "url", "conns", "netem",
                  "phase", "pool_limit",
                  "rps", "transfer_mbps", "lat_avg_ms", "lat_p50_ms", "lat_p99_ms",
                  "lat_max_ms", "cpu_avg", "cpu_max", "rss_mb",
                  "mem_usage_mb", "mem_anon_mb", "mem_cache_mb"]
    with open(path, "w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)
    log(f"results.csv сохранён ({len(rows)} строк)")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--workers", default=os.environ.get("WORKERS", "1 3"),
                    help="список значений WORKERS через пробел (по умолчанию '1 3')")
    ap.add_argument("--duration", type=int, default=DEFAULT_DURATION,
                    help=f"длительность сценария wrk, сек (по умолчанию {DEFAULT_DURATION})")
    ap.add_argument("--sweep-duration", type=int, default=SWEEP_DURATION,
                    help=f"длительность точки sweep, сек (по умолчанию {SWEEP_DURATION})")
    ap.add_argument("--scenarios", default="all",
                    help="подмножество сценариев через запятую, 'all' или ''/'none' "
                         "(только sweep/research)")
    ap.add_argument("--no-badnet", action="store_true", help="пропустить tc netem-сценарии")
    ap.add_argument("--no-sweep", action="store_true", help="пропустить sweep по конкарренси")
    ap.add_argument("--no-plot", action="store_true", help="не рисовать графики")
    ap.add_argument("--tag", default="", help="метка прогона (в метаданные)")
    ap.add_argument("--no-up", action="store_true",
                    help="не пересоздавать сервисы compose (серверы уже подняты)")
    ap.add_argument("--max-conns", type=int, default=10000,
                    help="лимит соединений в конфигах (max_connections sparrow/nginx)")
    ap.add_argument("--conns-research",
                    default=os.environ.get("CONNS_RESEARCH", "5000 10000 15000 20000"),
                    help="доп. прогоны с разными max_connections: idle-память + /status")
    ap.add_argument("--no-research", action="store_true",
                    help="пропустить research-фазу (max_connections sweep)")
    ap.add_argument("--sparrow-conns",
                    default=os.environ.get("SPARROW_CONNS", ""),
                    help="фаза «чистый sparrow»: значения max_connections в КОНФИГЕ "
                         "sparrow через пробел (например '1000 5000'); пусто = не запускать")
    ap.add_argument("--no-sparrow", action="store_true",
                    help="пропустить фазу sparrow-only (даже если задан --sparrow-conns)")
    ap.add_argument("--only-sparrow", action="store_true",
                    help="не гонять сравнительную матрицу и research — только "
                         "фазу sparrow-only (использует run_only_sparrow.sh)")
    args = ap.parse_args()

    workers_list = args.workers.split()
    scenario_names = ("none" if args.scenarios in ("", "none")
                      else (args.scenarios or "all"))

    gen_assets()

    data = {
        "meta": {
            "date": time.strftime("%Y-%m-%d %H:%M:%S"),
            "tag": args.tag,
            "workers": workers_list,
            "git_rev": sh(["git", "-C", BENCH_DIR, "rev-parse", "--short", "HEAD"],
                          check=False).stdout.strip(),
            "host_kernel": sh(["uname", "-r"], check=False).stdout.strip(),
        },
        "runs": [],
        "sweep": [],
        "config_runs": [],
        "memory": {},
        "memory_by_conns": {},
        "sparrow_only": {},
    }

    total_failures = 0
    try:
        if not args.only_sparrow:
            for workers in workers_list:
                if not args.no_up:
                    materialize_configs(workers, args.max_conns)
                    compose_up(workers)
                ready_with_retry(workers)
                sanity_checks()
                idle_mem = measure_idle_memory()
                runs, sweep, failures = run_matrix(
                    workers, args.duration, scenario_names,
                    do_badnet=not args.no_badnet, do_sweep=not args.no_sweep,
                    sweep_duration=args.sweep_duration, max_conns=args.max_conns)
                total_failures += failures
                data["memory"][str(workers)] = memory_section(workers, idle_mem, runs)
                data["runs"] += runs
                data["sweep"] += sweep
                save_json(data)
                if failures:
                    log(f"WARN: {failures} прогонов wrk не удалось")
    finally:
        netem.clear()

    if not args.only_sparrow:
        if not args.no_research and args.conns_research.strip():
            conns_list = [int(c) for c in args.conns_research.split()]
            cr, mbc, rfail = run_research(conns_list, args.duration)
            data["config_runs"] += cr
            data["memory_by_conns"].update(mbc)
            total_failures += rfail
            save_json(data)

    if not args.no_sparrow and args.sparrow_conns.strip():
        conns_list = [int(c) for c in args.sparrow_conns.split()]
        so, sfail = run_sparrow_only(conns_list, args.duration,
                                     do_badnet=not args.no_badnet,
                                     do_sweep=not args.no_sweep,
                                     sweep_duration=args.sweep_duration)
        data["sparrow_only"].update(so)
        total_failures += sfail
        save_json(data)

    save_json(data)
    save_csv(data)

    if not args.no_plot:
        log("рендер графиков")
        subprocess.run([sys.executable, os.path.join(BENCH_DIR, "plot.py")], check=True)

    summary.main()
    log("готово. Результаты в /results/ (results.json, results.csv, *.png)")

    if total_failures:
        log(f"FAIL: {total_failures} прогонов wrk завершились ошибкой")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
