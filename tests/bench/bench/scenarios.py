"""Матрица сценариев бенчмарка.

Каждый сценарий гоняется для всех серверов. netem (плохое соединение)
накладывается на eth0 runner'а — это влияет на ВЕСЬ его трафик к серверам.
"""
# Основные сценарии. tls=True -> https-URL; headers — доп. заголовки wrk.
SCENARIOS = [
    dict(name="status",        url="/status",            conns=64,
         desc="маленький JSON, keep-alive"),
    dict(name="status_noka",   url="/status",            conns=64,
         header="Connection: close", desc="тот же запрос, но без keep-alive"),
    dict(name="tls_status",    url="/status",            conns=64, tls=True,
         desc="TLS-рукопожатие + keep-alive"),
    dict(name="gzip_mid",      url="/static/mid.txt",    conns=64,
         header="Accept-Encoding: gzip",
         desc="gzip на лету, 1.3 КБ текста"),
    dict(name="static_small",  url="/static/small.txt",  conns=64,
         desc="86 Б файл, буферный путь"),
    dict(name="static_big",    url="/static/big.bin",    conns=16,
         desc="10 МБ random raw (sparrow: zero-copy SEND_ZC)"),
    dict(name="proxy",         url="/api/ping",          conns=64,
         desc="прокси GET на бэкенд"),
    dict(name="proxy_post",    url="/api/echo",          conns=64,
         script="post.lua",
         desc="прокси POST с телом (стриминг аплоада)"),
]

# Плохое соединение: tc netem. "base" — тот же запрос без netem (базовая линия).
BADNET = [
    dict(name="badnet_base",   tc=None,                    conns=32,
         desc="/status, c=32 — базовая линия для badnet"),
    dict(name="badnet_latency", tc=["delay", "50ms", "10ms", "distribution", "normal"],
         conns=32, desc="/status, задержка 50±10 мс"),
    dict(name="badnet_jitter", tc=["delay", "20ms", "15ms", "distribution", "normal"],
         conns=32, desc="/status, джиттер 20±15 мс"),
    dict(name="badnet_loss",   tc=["loss", "2%"],          conns=32,
         desc="/status, потеря 2% пакетов"),
]

# Свип по конкарренси для графика rps(c) (plain /status).
SWEEP_CONNS = [1, 16, 64, 256, 1024]
SWEEP_URL = "/status"

DEFAULT_DURATION = 15
SWEEP_DURATION = 10
