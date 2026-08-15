# =========================================================
# ЭТАП 1: Окружение сборки (Компиляция C++ бинарника)
# =========================================================
FROM ubuntu:24.04 AS builder

ENV DEBIAN_FRONTEND=noninteractive

# pkg-config обязателен: CMakeLists использует find_package(PkgConfig REQUIRED)
# для libdeflate. git нужен FetchContent'у: mbedtls и liburing клонируются
# по пиннутым тегам на этапе cmake (нужна сеть при docker build).
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    pkg-config \
    git \
    libdeflate-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build

COPY CMakeLists.txt .
COPY include/ include/
COPY src/ src/

RUN mkdir build_out && cd build_out && \
    cmake -DCMAKE_BUILD_TYPE=Release .. && \
    make -j$(nproc)

# =========================================================
# ЭТАП 2: Рантайм окружение (Минимальный итоговый образ)
# =========================================================
FROM ubuntu:24.04 AS runner

ENV DEBIAN_FRONTEND=noninteractive

# Единственная динамическая зависимость бинарника — libdeflate.so.0
# (mbedtls и liburing слинкованы статически, см. ldd)
RUN apt-get update && apt-get install -y \
    libdeflate0 \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=builder /build/build_out/nano_sparrow /app/nano_sparrow
COPY server.conf /app/server.conf

EXPOSE 8080

CMD ["./nano_sparrow", "server.conf"]
