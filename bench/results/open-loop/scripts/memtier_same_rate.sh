#!/bin/bash
# memtier and kosh-loadgen at a rate memtier can reach, same stall, same workload.
OUT=$HOME/mr-dev/lg-proof-quiet
B=~/mr-dev/build
MT=$HOME/mr-dev/memtier/install/bin/memtier_benchmark
PORT=7391
RATE=10000
mkdir -p "$OUT"; cd "$OUT" || exit 1
WORK=$(mktemp -d ~/mr-dev/lgfair.XXXXXX)
ping_ok() { timeout 2 redis-cli -p $PORT PING 2>/dev/null | grep -q PONG; }
start_redis() {
  redis-server --port $PORT --save '' --appendonly no --enable-debug-command yes --daemonize no --dir "$WORK" > "$WORK/redis.log" 2>&1 &
  SRV=$!
  for i in $(seq 1 50); do ping_ok && break; sleep 0.1; done
  ping_ok || { echo "redis did not start"; exit 1; }
}
stop_srv() { kill "$SRV" 2>/dev/null; wait "$SRV" 2>/dev/null; }

echo "=== memtier, $RATE req/s target"
start_redis
"$MT" -s 127.0.0.1 -p $PORT -t 1 -c 1 --ratio=1:0 --key-pattern=P:P --key-maximum=100000 -n allkeys -d 64 --hide-histogram > /dev/null 2>&1
( sleep 17; redis-cli -p $PORT DEBUG SLEEP 1 > /dev/null ) &
"$MT" -s 127.0.0.1 -p $PORT -t 1 -c 8 --rate-limiting=$((RATE / 8)) --ratio=1:9 --key-pattern=R:R --key-maximum=100000 -d 64 \
  --test-time=32 --print-percentiles 50,90,99,99.9,99.99 --json-out-file=fair-memtier.json --hide-histogram 2>&1 |
  tr '\r' '\n' | grep -E '^(Type|Sets|Gets|Totals)' | tee fair-memtier.txt
stop_srv

echo "=== memtier JSON: maximum latency and request count"
python3 - fair-memtier.json <<'EOF'
import json, sys
def walk(o, path=""):
    if isinstance(o, dict):
        for k, v in o.items():
            p = f"{path}/{k}"
            if isinstance(v, (dict, list)):
                walk(v, p)
            elif any(w in k.lower() for w in ("max", "count", "ops/sec")) and "Totals" in p and "Time-Serie" not in p:
                print(p, "=", v)
walk(json.load(open(sys.argv[1])))
EOF

echo "=== kosh-loadgen, $RATE req/s, same stall"
start_redis
( sleep 17; redis-cli -p $PORT DEBUG SLEEP 1 > /dev/null ) &
"$B/kosh-loadgen" --port $PORT --rate $RATE --duration 30 --json fair-loadgen.json | tee fair-loadgen.txt
stop_srv
rm -rf "$WORK"
echo DONE
