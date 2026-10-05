// server_common.h
// Code shared by the basic server and the thread-pool server:
//   - ServerStats: counters used for performance monitoring
//   - handle_client(): the per-connection request/response loop
//   - a monitor thread that prints stats every second
//   - signal handling for a clean Ctrl+C shutdown

#ifndef SERVER_COMMON_H
#define SERVER_COMMON_H

#include "net_utils.h"

#include <signal.h>
#include <sys/time.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <functional>
#include <string>
#include <thread>

// Set to false by the signal handler when the user presses Ctrl+C.
inline std::atomic<bool> g_running(true);

inline void on_signal(int) { g_running = false; }

inline void setup_signals() {
    // No SA_RESTART on purpose: we want accept() to return with EINTR
    // so the main loop can notice g_running == false and exit.
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
    signal(SIGPIPE, SIG_IGN);  // writing to a closed socket should not kill us
}

// All counters are atomics so many threads can update them without a lock.
struct ServerStats {
    std::atomic<int> active_clients{0};
    std::atomic<int> peak_clients{0};
    std::atomic<long long> total_connections{0};
    std::atomic<long long> total_requests{0};
    std::atomic<long long> total_proc_us{0};  // time spent processing requests

    void client_connected() {
        int now = ++active_clients;
        ++total_connections;
        // update peak (compare-exchange loop because another thread may race us)
        int old_peak = peak_clients.load();
        while (now > old_peak && !peak_clients.compare_exchange_weak(old_peak, now)) {
        }
    }
    void client_disconnected() { --active_clients; }
};

// The "business logic" of the server. To make the server do some real CPU
// work (and not just echo), it converts the payload to upper case and
// appends a simple checksum computed over the payload a few times.
inline std::string process_request(const std::string& req) {
    std::string out = req;
    for (char& c : out) c = toupper((unsigned char)c);

    unsigned long checksum = 0;
    for (int round = 0; round < 50; round++) {
        for (unsigned char c : req) checksum = checksum * 31 + c + round;
    }
    return out + " #" + std::to_string(checksum) + "\n";
}

// Serves one client until it disconnects. Used by both servers; the only
// difference between them is WHICH thread calls this function.
inline void handle_client(int client_fd, ServerStats& stats) {
    stats.client_connected();

    // Idle timeout: if a client sends nothing for 5 seconds, recv() fails and
    // we drop it. Without this, one idle client could block a pool worker
    // forever (and the server could never shut down cleanly).
    timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    std::string buffer, line;
    while (g_running && recv_line(client_fd, buffer, line)) {
        long long start = now_us();
        std::string reply = process_request(line);
        stats.total_proc_us += now_us() - start;

        if (!send_all(client_fd, reply)) break;
        ++stats.total_requests;
    }

    close(client_fd);
    stats.client_disconnected();
}

// Runs in its own thread and prints one line of stats per second.
// 'extra' lets the pool server add its queue length to the line.
inline void monitor_loop(ServerStats& stats, std::function<std::string()> extra) {
    long long last_requests = 0;
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        long long total = stats.total_requests.load();
        long long per_sec = total - last_requests;
        last_requests = total;

        double avg_proc = total > 0 ? (double)stats.total_proc_us.load() / total : 0.0;
        printf("[monitor] active=%d peak=%d conns=%lld reqs=%lld throughput=%lld req/s avg_proc=%.2f us%s\n",
               stats.active_clients.load(), stats.peak_clients.load(),
               stats.total_connections.load(), total, per_sec, avg_proc,
               extra ? extra().c_str() : "");
        fflush(stdout);
    }
}

inline void print_final_stats(const char* name, ServerStats& stats) {
    long long total = stats.total_requests.load();
    printf("\n=== %s shutting down ===\n", name);
    printf("total connections : %lld\n", stats.total_connections.load());
    printf("total requests    : %lld\n", total);
    printf("peak clients      : %d\n", stats.peak_clients.load());
    printf("avg processing    : %.2f us\n",
           total > 0 ? (double)stats.total_proc_us.load() / total : 0.0);
}

#endif
