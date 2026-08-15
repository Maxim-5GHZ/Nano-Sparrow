FROM python:3.12-slim

# wrk — нагрузчик, curl — проверки, openssl — сертификаты, iproute2 — tc netem,
# git — ревизия репозитория в метаданных результатов
RUN apt-get update && apt-get install -y --no-install-recommends \
        wrk curl openssl iproute2 git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Docker CLI + compose-плагин: runner пересоздаёт сервисы под WORKERS через docker.sock
ARG DOCKER_ARCH=x86_64
ADD https://download.docker.com/linux/static/stable/${DOCKER_ARCH}/docker-27.5.1.tgz /tmp/docker.tgz
RUN tar -xzf /tmp/docker.tgz -C /tmp \
    && mv /tmp/docker/docker /usr/local/bin/docker \
    && rm -rf /tmp/docker /tmp/docker.tgz \
    && mkdir -p /usr/local/lib/docker/cli-plugins
ADD https://github.com/docker/compose/releases/download/v2.29.7/docker-compose-linux-${DOCKER_ARCH} \
    /usr/local/lib/docker/cli-plugins/docker-compose
RUN chmod +x /usr/local/lib/docker/cli-plugins/docker-compose

RUN pip install --no-cache-dir matplotlib pyyaml

WORKDIR /bench
CMD ["sleep", "infinity"]
