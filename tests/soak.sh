#!/bin/bash
# Soak-тест: 30 минут непрерывной нагрузки (plain 8080 + TLS 8443).
# Каждые 10 с: RSS/CPU/FD/живучесть; каждые ~100 с: 20-сек замер RPS (wrk).
# Критерии: RPS(30м) >= 90% RPS(старт), рост RSS <= 10 МБ, CPU без пиков,
#           curl всегда 200, FD не растет.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/nano_sparrow}"
L=/tmp/opencode/load
WRK=${WRK:-/tmp/opencode/wrk/wrk}
OUT=$L/soak_report.txt
PIDS=$L/soak_pids.txt
LOGPLAIN=$L/soak_plain.log
LOGTLS=$L/soak_tls.log

[ -x "$BIN" ] || { echo "SKIP: binary $BIN не найден"; exit 2; }
[ -x "$WRK" ] || { echo "SKIP: wrk не найден ($WRK)"; exit 2; }

for p in $(ss -ltnp 2>/dev/null | grep -E ':(8080|8443) ' | grep -oE 'pid=[0-9]+' | cut -d= -f2 | sort -u); do
    kill -9 "$p" 2>/dev/null
done
sleep 0.3

"$BIN" $L/plain.conf > "$LOGPLAIN" 2>&1 &
PLAIN=$!
"$BIN" $L/tls.conf > "$LOGTLS" 2>&1 &
TLS=$!
echo "$PLAIN $TLS" > $PIDS
sleep 1
grep -q "bind failed" "$LOGPLAIN" && echo "FATAL: plain bind failed" && exit 2
grep -q "bind failed" "$LOGTLS" && echo "FATAL: tls bind failed" && exit 2

cpu_now() { # pid -> cpu% за ~1 сек
    local pid=$1 c1 c2
    c1=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
    sleep 1
    c2=$(awk '{print $14+$15}' /proc/$pid/stat 2>/dev/null || echo 0)
    echo $(( (c2 - c1) * 100 / 100 ))
}

rss_kb() { awk '/VmRSS/{print $2}' /proc/$1/status 2>/dev/null || echo 0; }
fd_cnt() { ls /proc/$1/fd 2>/dev/null | wc -l; }
alive()  { kill -0 "$1" 2>/dev/null; }

echo "=== SOAK 30min $(date +%H:%M:%S) plain=$PLAIN tls=$TLS ===" > "$OUT"
echo "time  rps_plain  rps_tls  rss_plainKB  rss_tlsKB  cpu_plain%  cpu_tls%  fd_p  fd_t  curl" >> "$OUT"

START=$(date +%s)
DEADLINE=$((START + 1800))
POINT=0
while [ "$(date +%s)" -lt "$DEADLINE" ]; do
    T=$(date +%s)
    EL=$((T - START))

    RPS_P=-; RPS_T=-
    if [ $((EL % 100)) -lt 10 ]; then
        POINT=$((POINT + 1))
        RPS_P=$($WRK -t4 -c256 -d20s http://127.0.0.1:8080/status 2>&1 | grep -oE 'Requests/sec:[ ]*[0-9.]+' | grep -oE '[0-9.]+')
        RPS_T=$($WRK -t4 -c64 -d20s https://127.0.0.1:8443/status 2>&1 | grep -oE 'Requests/sec:[ ]*[0-9.]+' | grep -oE '[0-9.]+')
    fi

    RP=$(rss_kb $PLAIN); RT=$(rss_kb $TLS)
    CP=$(cpu_now $PLAIN); CT=$(cpu_now $TLS)
    FP=$(fd_cnt $PLAIN); FT=$(fd_cnt $TLS)
    C=$([ "$(curl -s -o /dev/null -w '%{http_code}' --max-time 4 http://127.0.0.1:8080/status 2>/dev/null)" = "200" ] && [ "$(curl -sk -o /dev/null -w '%{http_code}' --max-time 4 https://127.0.0.1:8443/status 2>/dev/null)" = "200" ] && echo OK || echo FAIL)
    AP=$([ "$(alive $PLAIN && echo 1 || echo 0)" = "1" ] && echo 1 || echo 0)
    AT=$([ "$(alive $TLS && echo 1 || echo 0)" = "1" ] && echo 1 || echo 0)
    [ "$C" = "FAIL" ] || [ "$AP" = "0" ] || [ "$AT" = "0" ] && echo "!! $EL s: alive=$AP/$AT curl=$C" >> "$OUT"

    printf "%4dm  %-9s %-9s %-11s %-10s %-9s %-8s %-4s %-4s %s\n" \
        "$((EL / 60))" "$RPS_P" "$RPS_T" "$RP" "$RT" "$CP" "$CT" "$FP" "$FT" "$C" >> "$OUT"
done

echo "=== FINAL $(date +%H:%M:%S) ===" >> "$OUT"
kill -9 $PLAIN $TLS 2>/dev/null
cat "$OUT"
