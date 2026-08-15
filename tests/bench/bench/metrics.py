"""Сбор метрик контейнеров (docker stats + Docker API) параллельно прогону wrk.

docker stats даёт MemUsage (usage = anon + page cache + kernel), а для честного
сравнения «пожирания памяти» нужен отдельно true RSS (anon) и page cache — их
отдаёт Docker API (GET /containers/<name>/stats, cgroup v2: memory_stats.stats
поля anon / file; v1 — rss / cache). API ходим по unix-сокету docker.sock,
смонтированному в runner (docker-compose.yml).
"""
import http.client
import json
import os
import socket
import subprocess
import time

_SOCKET = "/var/run/docker.sock"


def _parse_bytes(text):
    parts = text.split()
    if not parts:
        return None
    for unit in ("GiB", "MiB", "KiB", "GB", "MB", "KB", "B"):
        if parts[0].endswith(unit):
            value = float(parts[0][: -len(unit)])
            if unit == "GiB":
                return value * 1024
            if unit == "MiB":
                return value
            if unit == "KiB":
                return value / 1024
            if unit == "GB":
                return value * 1000
            if unit == "MB":
                return value
            if unit == "KB":
                return value / 1000
    return None


def _api_get(path):
    """GET docker API через unix-сокет. Возвращает dict или None."""
    if not os.path.exists(_SOCKET):
        return None
    try:
        conn = http.client.HTTPConnection("localhost", timeout=10)
        conn.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        conn.sock.connect(_SOCKET)
        conn.request("GET", path)
        resp = conn.getresponse()
        if resp.status != 200:
            return None
        data = json.load(resp)
        conn.close()
        return data
    except (OSError, ValueError):
        return None


def _api_mem(name):
    """memory {usage, anon, cache} в байтах из Docker API, или None."""
    data = _api_get(f"/containers/{name}/stats?stream=false")
    if not data:
        return None
    ms = data.get("memory_stats") or {}
    usage = ms.get("usage")
    if usage is None:
        return None
    st = ms.get("stats") or {}
    if "anon" in st:  # cgroup v2
        return {"usage": usage, "anon": st["anon"], "cache": st.get("file", 0)}
    if "rss" in st:  # cgroup v1
        return {"usage": usage, "anon": st.get("rss", 0), "cache": st.get("cache", 0)}
    return None


def sample_containers(names, seconds, interval=1.5):
    """Опрашивает docker stats и Docker API для контейнеров names в течение seconds.

    Возвращает {name: {"cpu_avg", "cpu_max", "rss_mb" (usage из docker stats),
                        "usage_mb", "anon_mb", "cache_mb" (avg из API),
                        "usage_peak_mb", "anon_peak_mb", "cache_peak_mb"}}.
    CPUPerc — процент от ВСЕХ ядер хоста (как в docker stats).
    """
    cpu_samples = {n: [] for n in names}
    rss_samples = {n: [] for n in names}
    usage, anon, cache = {n: [] for n in names}, {n: [] for n in names}, {n: [] for n in names}
    deadline = time.time() + seconds
    while time.time() < deadline:
        proc = subprocess.run(
            ["docker", "stats", "--no-stream", "--format", "{{json .}}"] + names,
            capture_output=True, text=True)
        for line in proc.stdout.splitlines():
            try:
                data = json.loads(line)
            except json.JSONDecodeError:
                continue
            name = data.get("Name", "").lstrip("/")
            if name not in names:
                continue
            cpu = data.get("CPUPerc", "0%").rstrip("%")
            try:
                cpu_samples[name].append(float(cpu))
            except ValueError:
                pass
            mem = data.get("MemUsage", "0B / 0B").split(" / ")[0]
            mb = _parse_bytes(mem)
            if mb is not None:
                rss_samples[name].append(mb)
        for name in names:
            m = _api_mem(name)
            if m is None:
                continue
            usage[name].append(m["usage"] / 1024 / 1024)
            anon[name].append(m["anon"] / 1024 / 1024)
            cache[name].append(m["cache"] / 1024 / 1024)
        time.sleep(interval)

    def avg(samples):
        return (sum(samples) / len(samples)) if samples else None

    result = {}
    for name in names:
        cpus = cpu_samples[name]
        if not usage[name]:
            print(f"[metrics] WARN: нет данных Docker API для {name} "
                  "(docker.sock недоступен?) — только docker stats", flush=True)
        result[name] = {
            "cpu_avg": avg(cpus),
            "cpu_max": max(cpus) if cpus else None,
            "rss_mb": avg(rss_samples[name]),
            "usage_mb": avg(usage[name]),
            "anon_mb": avg(anon[name]),
            "cache_mb": avg(cache[name]),
            "usage_peak_mb": max(usage[name]) if usage[name] else None,
            "anon_peak_mb": max(anon[name]) if anon[name] else None,
            "cache_peak_mb": max(cache[name]) if cache[name] else None,
        }
    return result
