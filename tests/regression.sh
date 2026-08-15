#!/bin/bash
# Регрессионные тесты на баги, найденные wrk-нагрузкой (август 2026).
#
# Каждый тест: старт сервера -> провокация -> проверка (живучесть/CPU/скорость).
# Баги и их триггеры:
#  T1 SIGPIPE         — TLS-сервер умирал (141) на клиентах, закрывших сокет
#  T2 keep-alive TLS static — 1 запрос на коннект (~3 rps вместо 90k+)
#  T3 рекурсия        — TLS keep-alive переразбирал запрос -> stack overflow
#  T4 EOF-спин handshake — FIN без close_notify -> WANT_READ -> 100% CPU
#  T5 stale-errno спин — recv==0 -> ssl_read CONN_EOF -> вечный re-arm poll
#  T6 drain fd=-1 спин — ошибка записи -> drain крутился на закрытом conn
#  T7 TLS static big  — большие файлы по TLS отдавались 413 (буферный лимит)
#  T8 HTTP splice     — 10 МБ по plaintext целостно (splice file->pipe->socket)
#  T9 HTTP splice EAGAIN — медленный клиент (limit-rate) не рвет splice-поток
#  T10 HTTP splice pipeline — два запроса в одном пакете (big + small)
#  T11 HTTP splice abort — обрыв клиента на середине: пайп не утекает, сервер жив
#  T12 TLS large buffer — 10 МБ по TLS целостно (Large Buffer Pool)
#
# Запуск:  tests/regression.sh [путь_к_binary]   (по умолчанию build/nano_sparrow)
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/nano_sparrow}"
L=/tmp/opencode/load
WRK=${WRK:-/tmp/opencode/wrk/wrk}
TLS_URL=https://127.0.0.1:8443
PIDFILE=$L/regr_tls.pid
LOG=$L/regr_tls.log

[ -x "$BIN" ] || { echo "SKIP: binary $BIN не найден"; exit 2; }
[ -x "$WRK" ] || { echo "SKIP: wrk не найден ($WRK)"; exit 2; }

PASS=0; FAIL=0

start_tls() {
    # Освобождаем порт от любых посторонних слушателей (матрица, предыдущие прогоны)
    for p in $(ss -ltnp 2>/dev/null | awk '/:8443 /{match($0,/pid=[0-9]+/); if (RSTART) print substr($0, RSTART+4, RLENGTH-4)}'); do
        kill -9 "$p" 2>/dev/null
    done
    kill -9 "$(cat $PIDFILE 2>/dev/null)" 2>/dev/null
    sleep 0.3
    "$BIN" $L/tls.conf > "$LOG" 2>&1 &
    disown 2>/dev/null || true
    echo $! > $PIDFILE
    sleep 1
    if grep -q "bind failed" "$LOG"; then
        echo "  (bind failed — порт занят, инстанс не поднялся)"
        return 1
    fi
    return 0
}

stop_tls() {
    kill -9 "$(cat $PIDFILE 2>/dev/null)" 2>/dev/null
}

# Plaintext-инстанс (порт 18080): splice-путь SEND_SPLICE
L_BASE=http://127.0.0.1:18080
PIDFILEP=$L/regr_plain.pid
LOGP=$L/regr_plain.log

start_plain() {
    for p in $(ss -ltnp 2>/dev/null | awk '/:18080 /{match($0,/pid=[0-9]+/); if (RSTART) print substr($0, RSTART+4, RLENGTH-4)}'); do
        kill -9 "$p" 2>/dev/null
    done
    kill -9 "$(cat $PIDFILEP 2>/dev/null)" 2>/dev/null
    sleep 0.3
    "$BIN" $L/plain.conf > "$LOGP" 2>&1 &
    disown 2>/dev/null || true
    echo $! > $PIDFILEP
    sleep 1
    if grep -q "bind failed" "$LOGP"; then
        echo "  (plain bind failed — порт занят, инстанс не поднялся)"
        return 1
    fi
    return 0
}

stop_plain() {
    kill -9 "$(cat $PIDFILEP 2>/dev/null)" 2>/dev/null
}

# CPU% за ~1 сек по /proc (utime+stime)
cpu_now() {
    local pid=$1
    local c1 c2
    c1=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
    sleep 1
    c2=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
    echo $(( (c2 - c1) * 100 / 100 ))
}

alive() { kill -0 "$1" 2>/dev/null; }

curl_ok() {
    local code
    code=$(curl -sk -o /dev/null -w '%{http_code}' --max-time 4 "$1" 2>/dev/null)
    [ "$code" = "200" ]
}

check() { # name ok_message
    if [ "$2" = "1" ]; then
        echo "PASS: $1"
        PASS=$((PASS+1))
    else
        echo "FAIL: $1"
        FAIL=$((FAIL+1))
    fi
}

echo "=== Regression-тесты ($BIN) ==="

# T1: SIGPIPE — TLS-нагрузка не должна убить процесс (раньше: exit 141)
start_tls
$WRK -t4 -c64 -d5s $TLS_URL/status >/dev/null 2>&1
$WRK -t4 -c64 -d10s $TLS_URL/status >/dev/null 2>&1
PID=$(cat $PIDFILE)
check "T1 SIGPIPE: процесс жив после TLS-нагрузки" "$(alive $PID && echo 1 || echo 0)"
check "T1 SIGPIPE: сервер отвечает" "$(curl_ok $TLS_URL/status && echo 1 || echo 0)"
stop_tls

# T2: keep-alive TLS static — раньше 3 rps (1 запрос/коннект), теперь 90k+
start_tls
$WRK -t4 -c64 -d5s $TLS_URL/static/small.txt >/dev/null 2>&1
RPS=$($WRK -t4 -c64 -d10s $TLS_URL/static/small.txt 2>&1 | grep -oE 'Requests/sec:[ ]*[0-9.]+' | grep -oE '[0-9.]+')
check "T2 keep-alive TLS static: rps=$RPS (порог 10000)" "$(echo "$RPS > 10000" | bc 2>/dev/null || awk -v r="$RPS" 'BEGIN{exit !(r>10000)}')"
stop_tls

# T3: рекурсия — TLS keep-alive c64 + pipelined-хвосты не должен крашить
start_tls
$WRK -t4 -c64 -d10s $TLS_URL/status >/dev/null 2>&1
PID=$(cat $PIDFILE)
check "T3 рекурсия: процесс жив после c64 keep-alive" "$(alive $PID && echo 1 || echo 0)"
check "T3 рекурсия: сервер отвечает" "$(curl_ok $TLS_URL/status && echo 1 || echo 0)"
stop_tls

# T4: EOF-спин рукопожатия — обрыв на середине handshake, CPU должен быть ~0
start_tls
for i in $(seq 1 60); do
    # Частичный ClientHello + закрытие (FIN без close_notify)
    (head -c 64 /dev/urandom | timeout 0.3 nc -q0 127.0.0.1 8443) >/dev/null 2>&1
done
sleep 0.5
PID=$(cat $PIDFILE)
CPU=$(cpu_now $PID)
check "T4 EOF-handshake: CPU=$CPU% (порог 20)" "$([ "$CPU" -lt 20 ] && echo 1 || echo 0)"
check "T4 EOF-handshake: сервер отвечает" "$(curl_ok $TLS_URL/status && echo 1 || echo 0)"
stop_tls

# T5: stale-errno спин — завершение wrk (массовые FIN) не должно зациклить poll
start_tls
$WRK -t4 -c64 -d15s $TLS_URL/status >/dev/null 2>&1 &
WRKPID=$!
sleep 12
kill -9 $WRKPID 2>/dev/null   # жёсткий обрыв 64 TLS-соединений (RST/FIN)
sleep 1
PID=$(cat $PIDFILE)
CPU=$(cpu_now $PID)
check "T5 stale-errno: CPU=$CPU% после обрыва 64 conns (порог 20)" "$([ "$CPU" -lt 20 ] && echo 1 || echo 0)"
check "T5 stale-errno: сервер отвечает" "$(curl_ok $TLS_URL/status && echo 1 || echo 0)"
stop_tls

# T6: drain fd=-1 — обрыв клиентом в момент записи ответа, CPU ~0
start_tls
for i in $(seq 1 30); do
    # curl, убиваемый через 100 мс — RST/EOF в момент доставки ответа
    (curl -sk --max-time 0.1 -o /dev/null $TLS_URL/status) >/dev/null 2>&1
done
sleep 0.5
PID=$(cat $PIDFILE)
CPU=$(cpu_now $PID)
check "T6 drain-fd-1: CPU=$CPU% после 30 оборванных ответов (порог 20)" "$([ "$CPU" -lt 20 ] && echo 1 || echo 0)"
check "T6 drain-fd-1: сервер отвечает" "$(curl_ok $TLS_URL/status && echo 1 || echo 0)"
stop_tls

# T7: TLS static big — 10 МБ по TLS отдаются целиком (раньше: 413 + обрыв на 8 КБ)
start_tls
SZ=$(curl -sk --max-time 30 -o /dev/null -w '%{size_download}' $TLS_URL/static/big.bin 2>/dev/null)
check "T7 TLS static big: получено $SZ из 10485760" "$([ "$SZ" = "10485760" ] && echo 1 || echo 0)"
check "T7 TLS static big: сервер отвечает" "$(curl_ok $TLS_URL/status && echo 1 || echo 0)"
stop_tls

# T8: HTTP splice — 10 МБ целостно (plaintext, splice file->pipe->socket)
start_plain
BIG_MD5=$(md5sum $L/www/big.bin | awk '{print $1}')
GOT=$(curl -s --max-time 30 $L_BASE/static/big.bin 2>/dev/null | md5sum | awk '{print $1}')
check "T8 HTTP splice: md5 цел ($GOT)" "$([ "$GOT" = "$BIG_MD5" ] && echo 1 || echo 0)"
check "T8 HTTP splice: сервер отвечает" "$(curl_ok $L_BASE/status && echo 1 || echo 0)"
stop_plain

# T9: HTTP splice EAGAIN — медленный клиент (сокет полон -> -EAGAIN -> POLL_OUT)
start_plain
GOT=$(curl -s --max-time 60 --limit-rate 512K $L_BASE/static/big.bin 2>/dev/null | md5sum | awk '{print $1}')
check "T9 HTTP splice EAGAIN: md5 цел при 512 КБ/с ($GOT)" "$([ "$GOT" = "$BIG_MD5" ] && echo 1 || echo 0)"
check "T9 HTTP splice EAGAIN: сервер отвечает" "$(curl_ok $L_BASE/status && echo 1 || echo 0)"
stop_plain

# T10: HTTP splice pipeline — big.bin + small.txt в одном TCP-пакете
start_plain
python3 - "$L_BASE" "$BIG_MD5" "$L/www/small.txt" <<'EOF'
import socket, sys, hashlib
base, big_md5, small_path = sys.argv[1], sys.argv[2], sys.argv[3]
host, port = base[7:base.rfind(":")], int(base[base.rfind(":")+1:])
s = socket.create_connection((host, port), timeout=15)
s.sendall(b"GET /static/big.bin HTTP/1.1\r\nHost: x\r\n\r\nGET /static/small.txt HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n")
data = b""
while True:
    c = s.recv(65536)
    if not c: break
    data += c
body1 = data.split(b"\r\n\r\n", 1)[1]
rest = body1[10485760:].split(b"\r\n\r\n", 1)
ok = hashlib.md5(body1[:10485760]).hexdigest() == big_md5
ok = ok and rest[1] == open(small_path, "rb").read()
sys.exit(0 if ok else 1)
EOF
check "T10 HTTP splice pipeline: big+small в одном пакете" "$([ "$?" = "0" ] && echo 1 || echo 0)"
stop_plain

# T11: HTTP splice abort — обрыв на середине передачи, пайп пересоздан, сервер жив
start_plain
(curl -s --max-time 0.3 -o /dev/null $L_BASE/static/big.bin) >/dev/null 2>&1
sleep 0.3
GOT=$(curl -s --max-time 30 $L_BASE/static/big.bin 2>/dev/null | md5sum | awk '{print $1}')
check "T11 HTTP splice abort: md5 цел после обрыва ($GOT)" "$([ "$GOT" = "$BIG_MD5" ] && echo 1 || echo 0)"
PID=$(cat $PIDFILEP)
check "T11 HTTP splice abort: процесс жив" "$(alive $PID && echo 1 || echo 0)"
stop_plain

# T12: TLS large buffer — 10 МБ по TLS целостно (Large Buffer Pool)
start_tls
GOT=$(curl -sk --max-time 60 $TLS_URL/static/big.bin 2>/dev/null | md5sum | awk '{print $1}')
check "T12 TLS large buffer: md5 цел ($GOT)" "$([ "$GOT" = "$BIG_MD5" ] && echo 1 || echo 0)"
GOT=$(curl -sk --max-time 90 --limit-rate 512K $TLS_URL/static/big.bin 2>/dev/null | md5sum | awk '{print $1}')
check "T12 TLS large buffer: md5 цел при 512 КБ/с" "$([ "$GOT" = "$BIG_MD5" ] && echo 1 || echo 0)"
stop_tls

echo "=== Итог: PASS=$PASS FAIL=$FAIL ==="
[ "$FAIL" -eq 0 ]
