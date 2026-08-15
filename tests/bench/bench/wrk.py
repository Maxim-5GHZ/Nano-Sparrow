"""Запуск wrk и парсинг его текстового вывода."""
import re
import subprocess

_UNITS = {"us": 0.001, "ms": 1.0, "s": 1000.0}
_SIZE_UNITS = {"B": 1, "KB": 1024, "MB": 1024 ** 2, "GB": 1024 ** 3,
               "KiB": 1024, "MiB": 1024 ** 2, "GiB": 1024 ** 3}


def _num(line, pattern):
    m = re.search(pattern, line)
    return float(m.group(1)) if m else None


def run(url, threads=4, conns=64, duration=15, headers=None, script=None):
    """Возвращает dict метрик или None, если wrk упал."""
    cmd = ["wrk", f"-t{threads}", f"-c{conns}", f"-d{duration}s", "--latency"]
    if script:
        cmd += ["-s", script]
    for h in headers or []:
        cmd += ["-H", h]
    cmd.append(url)
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=duration + 120)
    if proc.returncode != 0:
        return None
    return parse(proc.stdout)


def parse(out):
    """Парсинг вывода wrk (с --latency). Все латентности в мс."""
    res = {"raw": out}
    m = re.search(r"Requests/sec:\s+([0-9.]+)", out)
    if not m:
        return None
    res["rps"] = float(m.group(1))

    m = re.search(r"Transfer/sec:\s+([0-9.]+)\s*([A-Za-z]+)", out)
    if m:
        res["transfer_mbps"] = float(m.group(1)) * _SIZE_UNITS[m.group(2)] / (1024 ** 2)

    # "  Latency    10.11ms    2.51ms  125.51ms   95.84%"
    m = re.search(r"Latency\s+([0-9.]+)(us|ms|s)\s+([0-9.]+)(us|ms|s)\s+([0-9.]+)(us|ms|s)",
                  out)
    if m:
        res["lat_avg_ms"] = float(m.group(1)) * _UNITS[m.group(2)]
        res["lat_stdev_ms"] = float(m.group(3)) * _UNITS[m.group(4)]
        res["lat_max_ms"] = float(m.group(5)) * _UNITS[m.group(6)]

    dist = {}
    for line in out.splitlines():
        mm = re.match(r"\s*(\d+)%\s+([0-9.]+)(us|ms|s)", line)
        if mm:
            dist[int(mm.group(1))] = float(mm.group(2)) * _UNITS[mm.group(3)]
    res["lat_dist_ms"] = dist
    res["lat_p50_ms"] = dist.get(50)
    res["lat_p99_ms"] = dist.get(99)

    m = re.search(r"(\d+)\s+requests in", out)
    if m:
        res["requests"] = int(m.group(1))
    return res
