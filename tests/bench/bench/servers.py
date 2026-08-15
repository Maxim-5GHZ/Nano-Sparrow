"""Описанные в compose серверы: имена, внутренние URL, контейнеры для docker stats."""
SERVERS = [
    {"name": "sparrow", "http": "http://sparrow:8080", "https": "https://sparrow-tls:8443",
     "container": "bench-sparrow", "container_tls": "bench-sparrow-tls"},
    {"name": "caddy", "http": "http://caddy:8080", "https": "https://caddy:8443",
     "container": "bench-caddy", "container_tls": "bench-caddy"},
    {"name": "nginx", "http": "http://nginx:8080", "https": "https://nginx:8443",
     "container": "bench-nginx", "container_tls": "bench-nginx"},
]

CONTAINER_NAMES = sorted({s["container"] for s in SERVERS}
                         | {s["container_tls"] for s in SERVERS})
