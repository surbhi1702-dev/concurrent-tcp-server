// basic_server.cpp
// Version 1: "thread per client" server.
// For every accepted connection a new std::thread is created and detached.
// Simple and correct, but creating a thread costs time and memory (stack),
// and under heavy load we can end up with thousands of threads.
//
// Usage: ./basic_server [port]

#include "server_common.h"

#include <cstdlib>
#include <system_error>

int main(int argc, char* argv[]) {
    int port = argc > 1 ? atoi(argv[1]) : 8080;

    setup_signals();
    int listen_fd = create_listen_socket(port);
    if (listen_fd < 0) return 1;

    printf("basic_server (thread per client) listening on port %d\n", port);
    fflush(stdout);

    ServerStats stats;
    std::thread monitor(monitor_loop, std::ref(stats), nullptr);

    while (g_running) {
        int client_fd = accept(listen_fd, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR) continue;  // interrupted by Ctrl+C, loop re-checks g_running
            perror("accept");
            continue;
        }

        int opt = 1;
        setsockopt(client_fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

        try {
            // New thread for this client. detach() so we don't have to join it.
            std::thread(handle_client, client_fd, std::ref(stats)).detach();
        } catch (const std::system_error& e) {
            // Happens when the OS refuses to create more threads.
            fprintf(stderr, "could not create thread: %s\n", e.what());
            close(client_fd);
        }
    }

    close(listen_fd);
    monitor.join();
    // Give detached threads a moment to notice g_running == false.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    print_final_stats("basic_server", stats);
    return 0;
}
