#!/bin/bash
# benchmark.sh
# Runs the load client against the basic server and the pool server for
# different numbers of concurrent clients and writes the results to CSV.
#
# Usage: ./scripts/benchmark.sh            (run from the project folder)
#        BIN=/some/dir ./scripts/benchmark.sh   (if binaries are elsewhere)
#
# For every run we also sample the server process from /proc to record
# the peak number of threads and the peak memory (VmHWM).

BIN=${BIN:-./bin}
OUT=${OUT:-results}
RUNS=${RUNS:-3}                       # repeat each test, results are averaged
LOADS=${LOADS:-"10 50 100 250 500 1000"}
REQS_PER_CONN=${REQS_PER_CONN:-5}     # short connections -> many thread creations
TOTAL_REQS=${TOTAL_REQS:-200000}      # roughly the same total work for each load
POOL_THREADS=${POOL_THREADS:-8}
PORT=9500

mkdir -p "$OUT"
CSV="$OUT/benchmark_results.csv"
echo "server,clients,run,reqs_ok,errors,seconds,throughput,avg_us,p50_us,p95_us,p99_us,max_us,avg_connect_us,peak_threads,peak_rss_kb" > "$CSV"

# Watches the server's /proc entry and prints "<max threads> <VmHWM kB>" at the end.
sample_server() {
    local pid=$1 max_threads=0
    while kill -0 "$pid" 2>/dev/null && [ ! -f "$OUT/.stop_sampling" ]; do
        t=$(awk '/^Threads:/ {print $2}' /proc/$pid/status 2>/dev/null)
        [ -n "$t" ] && [ "$t" -gt "$max_threads" ] && max_threads=$t
        sleep 0.01
    done
    hwm=$(awk '/^VmHWM:/ {print $2}' /proc/$pid/status 2>/dev/null)
    echo "$max_threads $hwm"
}

run_server_tests() {
    local name=$1; shift
    for clients in $LOADS; do
        conns=$(( TOTAL_REQS / (clients * REQS_PER_CONN) ))
        [ "$conns" -lt 1 ] && conns=1
        for run in $(seq 1 "$RUNS"); do
            PORT=$((PORT + 1))
            "$@" "$PORT" > /dev/null 2>&1 &   # start the server
            spid=$!
            sleep 0.3
            rm -f "$OUT/.stop_sampling"
            sample_server "$spid" > "$OUT/.sample" &
            mpid=$!

            line=$("$BIN/client" -p "$PORT" -c "$clients" -n "$conns" -r "$REQS_PER_CONN" --csv)

            touch "$OUT/.stop_sampling"
            wait "$mpid"
            read peak_threads peak_rss < "$OUT/.sample"
            kill -INT "$spid"; wait "$spid" 2>/dev/null

            # client prints: clients,ok,errors,... -> drop its first column
            echo "$name,$clients,$run,${line#*,},$peak_threads,$peak_rss" >> "$CSV"
            echo "$name clients=$clients run=$run -> $line threads=$peak_threads"
        done
    done
}

# pool_server takes: port threads queue  -> wrap so port comes first
pool_cmd() { exec "$BIN/pool_server" "$1" "$POOL_THREADS" 4096; }
basic_cmd() { exec "$BIN/basic_server" "$1"; }

run_server_tests basic basic_cmd
run_server_tests pool  pool_cmd

rm -f "$OUT/.stop_sampling" "$OUT/.sample"
echo "results written to $CSV"
