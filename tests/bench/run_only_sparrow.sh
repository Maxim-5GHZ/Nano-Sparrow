#!/usr/bin/env bash
# =========================================================
# Анализ производительности ТОЛЬКО Nano-Sparrow при разных
# max_connections (лимит задаётся в КОНФИГЕ sparrow):
# полная матрица сценариев, nginx/caddy останавливаются.
# Результаты — в tests/bench/results-sparrow/ (сравнительный
# ./run.sh и его results/ не трогаются).
#
#   ./run_only_sparrow.sh                  max_connections 500 и 1500
#   ./run_only_sparrow.sh --full           полные длительности (15 с на сценарий)
#   ./run_only_sparrow.sh --conns "500 1500 5000"  свои лимиты
#   ./run_only_sparrow.sh --build          предварительно собрать образы и уйти
#   ./run_only_sparrow.sh --down           снять контейнеры (не прогоняя)
#   ./run_only_sparrow.sh -- <arg>...      остальные аргументы уходят в run_bench.py
#
# Exit code: 0 = всё прошло успешно, 1 = ошибка.
# =========================================================
set -euo pipefail

cd "$(dirname "$0")"

COMPOSE="docker compose -f docker-compose.yml"
DEMO_DURATION=${DEMO_DURATION:-6}
DEMO_SWEEP_DURATION=${DEMO_SWEEP_DURATION:-5}
CONNS=${SPARROW_CONNS:-"500 1500"}

usage() {
    sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
}

[ $# -gt 0 ] && case "$1" in
    -h|--help) usage ;;
esac

# --- Проверка окружения ------------------------------------------------
if ! command -v docker >/dev/null 2>&1; then
    echo "ОШИБКА: docker не установлен." >&2
    exit 1
fi
if ! docker info >/dev/null 2>&1; then
    echo "ОШИБКА: docker демон не запущен (docker info не отвечает)." >&2
    exit 1
fi
if ! docker compose version >/dev/null 2>&1; then
    echo "ОШИБКА: docker compose v2 не установлен." >&2
    exit 1
fi

FREE_MB=$(awk '/MemAvailable/ {print int($2/1024)}' /proc/meminfo 2>/dev/null || true)
if [ -n "$FREE_MB" ] && [ "$FREE_MB" -lt 1536 ]; then
    echo "ВНИМАНИЕ: свободно всего ~${FREE_MB} МБ RAM — стенд может стартовать медленно (своппинг)." >&2
fi

MODE=demo
DO_BUILD=false
DO_DOWN=false
BENCH_ARGS=()

while [ $# -gt 0 ]; do
    case "$1" in
        --full)   MODE=full ;;
        --build)  DO_BUILD=true ;;
        --down)   DO_DOWN=true ;;
        --conns)  CONNS="$2"; shift ;;
        --)       shift; BENCH_ARGS+=("$@"); break ;;
        -h|--help) usage ;;
        *)        BENCH_ARGS+=("$1") ;;
    esac
    shift
done

if [ "$DO_BUILD" = true ]; then
    echo "==> Сборка образов (первый запуск или обновление)"
    $COMPOSE build sparrow runner backend
    $COMPOSE pull backend
    echo "==> Образы готовы. Теперь можно запускать: ./run_only_sparrow.sh"
    exit 0
fi

if [ "$DO_DOWN" = true ]; then
    $COMPOSE down
    echo "==> Стенд снят."
    exit 0
fi

if [ "$MODE" = full ]; then
    BENCH_ARGS=${BENCH_ARGS[@]+"${BENCH_ARGS[@]}"}
else
    BENCH_ARGS=(--duration "$DEMO_DURATION" --sweep-duration "$DEMO_SWEEP_DURATION"
                ${BENCH_ARGS[@]+"${BENCH_ARGS[@]}"})
fi

# --- Сборка образов при необходимости (повторные запуски мгновенные) ---
needs_image() { docker image inspect "$1" >/dev/null 2>&1; }
if ! needs_image zero-alloc-server:bench || ! needs_image bench-runner:local; then
    echo "==> Образы не найдены — собираю (нужна сеть при первом запуске)"
    $COMPOSE build sparrow runner
fi

# Папка результатов фазы (отдельная от results/). Создаём заранее от
# имени пользователя, чтобы файлы не оказались в root-папке.
mkdir -p results-sparrow

echo "==> Поднимаю runner"
$COMPOSE up -d runner
trap '$COMPOSE down >/dev/null 2>&1 || true' EXIT

# up -d возвращается раньше, чем контейнер реально поднимется — ждём готовности
READY=
for i in $(seq 1 60); do
    if $COMPOSE exec -T runner true 2>/dev/null; then
        READY=1
        break
    fi
    sleep 1
done
if [ "$READY" != 1 ]; then
    echo "ОШИБКА: runner не поднялся за 60 с (см. docker compose logs runner)" >&2
    exit 1
fi

echo "==> Запускаю анализ только sparrow: max_connections = $CONNS"
set +e
# Вывод exec гонится через pipe: у snap-docker 29.x `docker compose exec`
# ломается при прямом редиректе stdout в файл — демон рвёт соединение
# ("exec attach failed: broken pipe") и exec молча возвращает 1.
set -o pipefail
$COMPOSE exec -T -e RESULTS_DIR=/results-sparrow runner \
    python /bench/bench/run_bench.py --only-sparrow \
    --sparrow-conns "$CONNS" "${BENCH_ARGS[@]}" 2>&1 | cat
RC=${PIPESTATUS[0]}
set +o pipefail
set -e
echo "==> bench завершился с кодом $RC"

echo "==> Снимаю стенд"
$COMPOSE down

if [ $RC -eq 0 ]; then
    echo "==> УСПЕХ. Результаты и графики: tests/bench/results-sparrow/"
else
    echo "==> ПРОГРАММА ЗАВЕРШИЛАСЬ С ОШИБКОЙ (код $RC). См. логи выше." >&2
fi
exit $RC
