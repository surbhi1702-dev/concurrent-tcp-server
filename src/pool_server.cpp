// pool_server.cpp
// Version 2: thread-pool server.
// A fixed number of worker threads is created once. The main thread only
// accepts connections and pushes them into the pool's job queue.
//
// Usage: ./pool_server [port] [num_threads] [max_queue]

#include "server_common.h"
#include "thread_pool.h"

#include <cstdlib>

int main(int argc, char* argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;
    int num_threads = argc > 2 ? atoi(argv[2]) : 2 * (int)std::thread::hardware_concurrency();
    int max_queue = argc > 3 ? atoi(argv[3]) : 1024;
    if (num_threads <= 0) num_threads = 8;

    setup_signals();
    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) return 1;

    printf("pool_server listening on port %d with %d worker threads (queue limit %d)\n",
           port, num_threads, max_queue);
    fflush(stdout);

    ServerStats stats;
    ThreadPool pool(num_threads, max_queue);

    std::thread monitor(monitor_loop, std::ref(stats), [&pool]() {
        return " queue=" + std::to_string(pool.queue_size());
    });

    while (g_running) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        // Hand the connection to a worker. A worker serves this client until
        // it disconnects, then picks the next connection from the queue.
        if (!pool.submit([client_fd, &stats]() { handle_client(client_fd, stats); })) {
            close(client_fd);
        }
    }

    close(listen_fd);
    monitor.join();
    pool.shutdown();
    print_final_stats("pool_server", stats);
    return 0;
}
