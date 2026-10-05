// client.cpp
// Load-generating client used for benchmarking.
//
// It starts C client threads. Each thread opens N connections one after
// another, and on each connection sends R requests (send, wait for reply).
// For every request it records the response time (round trip). At the end
// it prints throughput and latency percentiles.
//
// Usage:
//   ./client [-h host] [-p port] [-c clients] [-n conns_per_client]
//            [-r requests_per_conn] [-s payload_size] [--csv]

#include "net_utils.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct Config {
    std::string host = "127.0.0.1";
    int port = 8080;
    int clients = 10;
    int conns_per_client = 10;
    int reqs_per_conn = 10;
    int payload_size = 64;
    bool csv = false;
};

// Results collected by all client threads.
struct Results {
    std::mutex mtx;
    std::vector<long long> latencies_us;  // one entry per successful request
    std::atomic<long long> errors{0};
    std::atomic<long long> connect_us{0};  // total time spent in connect()
    std::atomic<long long> connections{0};
};

void client_thread(int id, const Config& cfg, Results& results) {
    std::vector<long long> my_latencies;  // local vector, merged once at the end
    my_latencies.reserve(cfg.conns_per_client * cfg.reqs_per_conn);

    std::string payload(cfg.payload_size, 'a' + id % 26);
    std::string request = payload + "\n";

    for (int c = 0; c < cfg.conns_per_client; c++) {
        long long t0 = now_us();
        int fd = connect_to_server(cfg.host, cfg.port);
        if (fd < 0) {
            results.errors += cfg.reqs_per_conn;
            continue;
        }
        results.connect_us += now_us() - t0;
        ++results.connections;

        std::string buffer, reply;
        for (int r = 0; r < cfg.reqs_per_conn; r++) {
            long long start = now_us();
            if (!send_all(fd, request) || !recv_line(fd, buffer, reply)) {
                results.errors += cfg.reqs_per_conn - r;
                break;
            }
            my_latencies.push_back(now_us() - start);
        }
        close(fd);
    }

    std::lock_guard<std::mutex> lock(results.mtx);
    results.latencies_us.insert(results.latencies_us.end(), my_latencies.begin(), my_latencies.end());
}

// p in [0,100]. Expects a sorted vector.
double percentile(const std::vector<long long>& sorted, double p) {
    if (sorted.empty()) return 0;
    size_t idx = (size_t)(p / 100.0 * (sorted.size() - 1));
    return (double)sorted[idx];
}

void usage(const char* prog) {
    printf("usage: %s [-h host] [-p port] [-c clients] [-n conns_per_client] "
           "[-r requests_per_conn] [-s payload_size] [--csv]\n", prog);
}

int main(int argc, char* argv[]) {
    Config cfg;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--csv") { cfg.csv = true; continue; }
        if (i + 1 >= argc) { usage(argv[0]); return 1; }
        if (arg == "-h") cfg.host = argv[++i];
        else if (arg == "-p") cfg.port = atoi(argv[++i]);
        else if (arg == "-c") cfg.clients = atoi(argv[++i]);
        else if (arg == "-n") cfg.conns_per_client = atoi(argv[++i]);
        else if (arg == "-r") cfg.reqs_per_conn = atoi(argv[++i]);
        else if (arg == "-s") cfg.payload_size = atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }

    Results results;
    std::vector<std::thread> threads;

    long long start = now_us();
    for (int i = 0; i < cfg.clients; i++) {
        threads.emplace_back(client_thread, i, std::cref(cfg), std::ref(results));
    }
    for (std::thread& t : threads) t.join();
    double elapsed_sec = (now_us() - start) / 1e6;

    std::vector<long long>& lat = results.latencies_us;
    std::sort(lat.begin(), lat.end());

    long long ok = lat.size();
    double sum = 0;
    for (long long v : lat) sum += v;
    double avg = ok > 0 ? sum / ok : 0;
    double throughput = ok / elapsed_sec;
    long long conns = results.connections.load();
    double avg_connect = conns > 0 ? (double)results.connect_us.load() / conns : 0;

    if (cfg.csv) {
        // clients,reqs_ok,errors,seconds,throughput,avg_us,p50_us,p95_us,p99_us,max_us,avg_connect_us
        printf("%d,%lld,%lld,%.3f,%.0f,%.1f,%.0f,%.0f,%.0f,%lld,%.1f\n",
               cfg.clients, ok, results.errors.load(), elapsed_sec, throughput, avg,
               percentile(lat, 50), percentile(lat, 95), percentile(lat, 99),
               ok > 0 ? lat.back() : 0LL, avg_connect);
    } else {
        printf("clients            : %d\n", cfg.clients);
        printf("connections        : %lld\n", conns);
        printf("successful requests: %lld\n", ok);
        printf("failed requests    : %lld\n", results.errors.load());
        printf("total time         : %.3f s\n", elapsed_sec);
        printf("throughput         : %.0f req/s\n", throughput);
        printf("avg response time  : %.1f us\n", avg);
        printf("p50 / p95 / p99    : %.0f / %.0f / %.0f us\n",
               percentile(lat, 50), percentile(lat, 95), percentile(lat, 99));
        printf("max response time  : %lld us\n", ok > 0 ? lat.back() : 0LL);
        printf("avg connect time   : %.1f us\n", avg_connect);
    }
    return 0;
}
