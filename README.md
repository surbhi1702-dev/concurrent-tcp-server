# Concurrent TCP Client-Server System (C++ / Linux)

A multi-client TCP server written in C++ using POSIX sockets, in two versions:

1. **basic_server**: creates a new thread for every client (thread-per-client).
2. **pool_server**: uses a fixed-size thread pool with a job queue.

Both servers have built-in performance monitoring (active clients, throughput,
processing time). A multithreaded load-generating client measures response
time and throughput, and a benchmark script compares the two servers under
different loads.

## Project structure

```
tcp-server-project/
├── Makefile
├── include/
│   ├── net_utils.h        socket helpers: send_all, recv_line, listen/connect, timer
│   ├── server_common.h    ServerStats, handle_client(), monitor thread, signals
│   └── thread_pool.h      ThreadPool (mutex + 2 condition variables + bounded queue)
├── src/
│   ├── basic_server.cpp   thread-per-client server
│   ├── pool_server.cpp    thread-pool server
│   └── client.cpp         load generator + latency/throughput measurement
├── scripts/
│   └── benchmark.sh       runs both servers across loads, writes CSV
└── results/
    ├── benchmark_results.csv
    └── long_lived_connections.csv
```

## Build and run

Requirements: Linux, g++ with C++17, make.

```bash
make                      # builds bin/basic_server, bin/pool_server, bin/client

# terminal 1: start a server
./bin/basic_server 8080
# or
./bin/pool_server 8080 8 1024      # port, worker threads, max queue length

# terminal 2: generate load
./bin/client -p 8080 -c 100 -n 20 -r 5
#   -c  number of concurrent client threads
#   -n  connections each client opens (one after another)
#   -r  requests sent on each connection
#   -s  payload size in bytes (default 64)
#   --csv  print one CSV line instead of the human readable report

# run the full benchmark (both servers, 10..1000 clients, 3 runs each)
make benchmark
```

Press Ctrl+C to stop a server; it prints final statistics before exiting.

Example server monitor output (printed every second):

```
[monitor] active=8 peak=8 conns=1000 reqs=10000 throughput=10000 req/s avg_proc=4.59 us queue=42
```

## Protocol

A simple text protocol. Every message ends with `\n`.

- Client sends: `<payload>\n`
- Server replies: `<PAYLOAD IN UPPER CASE> #<checksum>\n`

The checksum is computed over the payload 50 times so that the server does a
small amount of real CPU work per request (about 4-5 µs) instead of a pure echo.

Because TCP is a byte stream, one `recv()` can return half a message or two
messages together. `recv_line()` keeps a buffer per connection and only returns
complete lines. `send_all()` loops because `send()` can send fewer bytes than asked.

## Design

### Basic server (thread per client)

```
main thread: accept() ──> std::thread(handle_client, fd).detach()
```

Easy to write, and every client gets its own thread immediately. The problems:
creating and destroying a thread for each connection has a cost (a `clone()`
system call, stack allocation of 8 MB virtual memory, scheduler work), and the number of
threads is unbounded. With many short connections this overhead dominates.

### Thread-pool server

```
main thread: accept() ──> pool.submit(job) ──> [ bounded job queue ] ──> N worker threads
                                                                         └─ handle_client(fd)
```

- N worker threads are created once at startup (default 2 × CPU cores).
- `submit()` pushes a job into a `std::queue` protected by a `std::mutex`.
- Workers wait on a `condition_variable` (`not_empty_`) until a job arrives.
- The queue is **bounded**: when it is full, `submit()` waits on a second
  condition variable (`not_full_`). This gives back-pressure, so under extreme
  load the accept loop slows down instead of memory growing without limit
  (unaccepted connections wait in the kernel's listen backlog).
- The job runs **outside** the lock so workers don't block each other.
- `shutdown()` sets a flag, wakes all workers with `notify_all()`, lets them
  finish queued jobs and joins them.

### Performance monitoring

`ServerStats` uses `std::atomic` counters so that all threads can update them
without a mutex: active clients, peak clients, total connections, total
requests and total processing time. A separate monitor thread prints one line
every second with the current throughput (difference of the request counter
between two samples), and the pool server adds the current queue length.

The client measures, for every request, the round-trip time from just before
`send()` until the full reply line is received, using `std::chrono::steady_clock`.
Each client thread stores its latencies in its own vector (no locking in the hot
path) and merges them once at the end. From these it prints throughput, average,
p50/p95/p99 and max response time.

### Other details

- `SO_REUSEADDR` so the server can restart immediately.
- `TCP_NODELAY` on both sides, because Nagle's algorithm delays small messages.
- `MSG_NOSIGNAL` / `SIGPIPE` ignored, so a client disconnecting during `send()`
  does not kill the server.
- `SO_RCVTIMEO` of 5 s: an idle client is disconnected, so it cannot hold a
  pool worker forever.
- `SIGINT` handler without `SA_RESTART`, so `accept()` returns `EINTR` and the
  main loop can exit cleanly.

## Benchmark results

**Setup:** 4 vCPU Intel Xeon @ 2.10 GHz, Linux 6.18, g++ 13.3 with `-O2`.
Client and server ran on the same machine over loopback (127.0.0.1).
Pool server used 8 worker threads. Each load sends about 200,000 requests in
total using short connections (5 requests per connection, then reconnect),
which is the workload where thread creation overhead matters most.
Every value is the average of 3 runs. Zero failed requests in all runs.

| Concurrent clients | Basic (req/s) | Pool (req/s) | Speedup | Basic avg (ms) | Pool avg (ms) | Basic p99 (ms) | Pool p99 (ms) |
|---:|---:|---:|---:|---:|---:|---:|---:|
| 10   | 67,665 | 106,374 | 1.57x | 0.13  | 0.08  | 1.2   | 0.5  |
| 50   | 43,225 | 71,814  | 1.66x | 1.14  | 0.68  | 7.5   | 4.1  |
| 100  | 42,887 | 68,329  | 1.59x | 2.31  | 1.45  | 14.3  | 8.6  |
| 250  | 41,944 | 68,357  | 1.63x | 5.90  | 3.61  | 34.4  | 21.1 |
| 500  | 42,169 | 68,878  | 1.63x | 11.70 | 7.11  | 65.7  | 42.1 |
| 1000 | 40,068 | 66,988  | 1.67x | 24.37 | 14.36 | 140.3 | 85.6 |

Raw data (every run, plus p50/p95/max, connect time, peak threads and peak
memory of the server): `results/benchmark_results.csv`.

**Observations**

- The pool server gave **about 1.6x higher throughput** at every load and
  **about 40% lower average and p99 response time**.
- The basic server created a new thread for each of the 40,000 connections in a
  run, and up to ~120 of them were alive at the same time (sampled from
  `/proc/<pid>/status`). The pool server always had exactly 10 threads
  (main + monitor + 8 workers).
- Throughput of both servers flattens after ~50 clients because the 4 CPU cores
  are saturated (the client threads run on the same machine). Beyond that,
  extra clients only add queueing delay, so response time grows roughly
  linearly with the number of clients.
- On a real network (separate client machine) the absolute numbers would be
  different; the relative comparison is the interesting part.

### Limitation found: long-lived connections

I also tested 100 clients that each keep **one** connection open for 2,000
requests (`results/long_lived_connections.csv`):

| Server | Throughput (req/s) | Avg (µs) | p99 (µs) | Max (µs) |
|---|---:|---:|---:|---:|
| basic | 310,747 | 298 | 848 | 18,560 |
| pool (8 threads) | 337,085 | 147 | 65 | 509,873 |

Throughput is similar, but the **max** response time of the pool server is
about 0.5 s. A worker serves one connection until it closes, so with 100 clients and
8 workers, 92 clients wait in the queue until another client finishes. This is
the main trade-off of the "one worker per connection" pool design. Ways to fix
it (not implemented here) are: hand only one *request* (not the whole
connection) to the pool, or use non-blocking sockets with `epoll` so a few
threads can serve thousands of connections.

## Possible improvements

- `epoll` based event loop (Reactor pattern) for many idle/long connections.
- Per-request scheduling instead of per-connection scheduling in the pool.
- Run client and server on different machines to remove CPU contention.
- Dynamic pool size based on queue length.
