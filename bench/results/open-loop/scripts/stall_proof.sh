#!/bin/bash
# First proof: stall real Redis for a known time and see who reports it.
# redis-benchmark (closed loop) against kosh-loadgen (open loop); also Kosh,
# frozen with SIGSTOP because it has no DEBUG SLEEP.
# Port 6379 belongs to the user's Windows Redis: never used here.
OUT=${1:-$HOME/mr-dev/lg-proof}
B=~/mr-dev/build
PORT=7391
RATE=20000
MT=$HOME/mr-dev/memtier/install/bin/memtier_benchmark
DUR=30
STALL=1
mkdir -p "$OUT"; cd "$OUT" || exit 1
WORK=$(mktemp -d ~/mr-dev/lgproof.XXXXXX)
uptime | tee uptime.txt

ping_ok() { timeout 2 redis-cli -p $PORT PING 2>/dev/null | grep -q PONG; }
start_redis() {
  redis-server --port $PORT --save '' --appendonly no --enable-debug-command yes --daemonize no --dir "$WORK" > "$WORK/redis.log" 2>&1 &
  SRV=$!
  for i in $(seq 1 50); do ping_ok && break; sleep 0.1; done
  ping_ok || { echo "redis did not start"; exit 1; }
}
start_kosh() {
  (cd "$WORK" && exec "$B/kosh-server" --port $PORT --appendonly no > "$WORK/kosh.log" 2>&1) &
  SRV=$!
  for i in $(seq 1 50); do ping_ok && break; sleep 0.1; done
  ping_ok || { echo "kosh did not start"; exit 1; }
}
stop_srv() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }

echo "=== 1. Redis, no stall: is the generator's own floor low?"
start_redis
"$B/kosh-loadgen" --port $PORT --rate $RATE --duration 20 --json nostall.json | tee nostall.txt
stop_srv

echo "=== 2. Redis, DEBUG SLEEP $STALL s halfway: kosh-loadgen"
start_redis
( sleep $((2 + DUR / 2)); redis-cli -p $PORT DEBUG SLEEP $STALL > /dev/null ) &
"$B/kosh-loadgen" --port $PORT --rate $RATE --duration $DUR --json stall-loadgen.json | tee stall-loadgen.txt
wait %2 2>/dev/null
stop_srv

echo "=== 3. Redis, DEBUG SLEEP $STALL s: redis-benchmark (closed loop)"
start_redis
( sleep 4; redis-cli -p $PORT DEBUG SLEEP $STALL > /dev/null ) &
redis-benchmark -p $PORT -t get -n 1000000 -c 8 -r 100000 -d 64 --threads 1 --precision 3 2>&1 | tr '\r' '\n' | grep -vE '^(GET|SET): rps' | tee stall-redis-benchmark.txt
stop_srv

echo "=== 3b. Redis, DEBUG SLEEP $STALL s: memtier (a rate cap on a closed loop)"
start_redis
"$MT" -s 127.0.0.1 -p $PORT -t 1 -c 1 --ratio=1:0 --key-pattern=P:P --key-maximum=100000 -n allkeys -d 64 --hide-histogram > /dev/null 2>&1
( sleep 17; redis-cli -p $PORT DEBUG SLEEP $STALL > /dev/null ) &
"$MT" -s 127.0.0.1 -p $PORT -t 1 -c 8 --rate-limiting=$((RATE / 8)) --ratio=1:9 --key-pattern=R:R --key-maximum=100000 -d 64 \
  --test-time=32 --print-percentiles 50,90,99,99.9,99.99 --json-out-file=stall-memtier.json --hide-histogram 2>&1 |
  tr '\r' '\n' | grep -E '^(Type|Sets|Gets|Totals)' | tee stall-memtier.txt
stop_srv

echo "=== 4. Kosh, frozen $STALL s (SIGSTOP): kosh-loadgen"
start_kosh
( sleep $((2 + DUR / 2)); kill -STOP "$SRV"; sleep $STALL; kill -CONT "$SRV" ) &
"$B/kosh-loadgen" --port $PORT --rate $RATE --duration $DUR --json stall-kosh.json | tee stall-kosh.txt
stop_srv

echo "=== 5. 1 thread vs 4 threads at the same rate: does the client's own queueing show?"
start_redis
"$B/kosh-loadgen" --port $PORT --rate $RATE --duration 15 --threads 1 --connections 8 --json t1.json | grep -E '^  (p50|p99|p99.9) |lateness' | head -8
"$B/kosh-loadgen" --port $PORT --rate $RATE --duration 15 --threads 4 --connections 8 --json t4.json | grep -E '^  (p50|p99|p99.9) |lateness' | head -8
stop_srv

echo "=== 6. overload: rate far above capacity, send-timed against schedule-timed"
start_redis
"$B/kosh-loadgen" --port $PORT --rate 400000 --duration 8 --drain 30 --threads 2 --connections 8 --json overload.json | tee overload.txt
stop_srv
rm -rf "$WORK"
echo DONE
