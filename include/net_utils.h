// net_utils.h
// Small helper functions for socket programming that both the servers
// and the client use. Kept header-only so the Makefile stays simple.

#ifndef NET_UTILS_H
#define NET_UTILS_H

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>

// Every message in our protocol ends with '\n'.
// Client sends:  "<payload>\n"
// Server replies: "<processed payload>\n"
const char MSG_DELIM = '\n';
const int MAX_MSG_LEN = 4096;

// send() may write fewer bytes than asked, so loop until everything is sent.
inline bool send_all(int fd, const std::string& data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        sent += n;
    }
    return true;
}

// Reads one '\n' terminated line from the socket.
// 'buffer' keeps leftover bytes between calls (TCP is a byte stream, so one
// recv() can contain part of a message or more than one message).
// Returns false when the peer closed the connection or an error happened.
inline bool recv_line(int fd, std::string& buffer, std::string& line) {
    while (true) {
        size_t pos = buffer.find(MSG_DELIM);
        if (pos != std::string::npos) {
            line = buffer.substr(0, pos);
            buffer.erase(0, pos + 1);
            return true;
        }
        if (buffer.size() > (size_t)MAX_MSG_LEN) {
            return false;  // message too long, treat as bad client
        }
        char tmp[1024];
        ssize_t n = recv(fd, tmp, sizeof(tmp), 0);
        if (n == 0) return false;  // peer closed
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        buffer.append(tmp, n);
    }
}

// Creates a listening socket on the given port. Returns -1 on failure.
inline int create_listen_socket(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) {
        perror("socket");
        return -1;
    }

    // Allow quick restart of the server without "Address already in use".
    int opt = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(fd);
        return -1;
    }
    if (listen(fd, SOMAXCONN) < 0) {
        perror("listen");
        close(fd);
        return -1;
    }
    return fd;
}

// Connects to host:port. Returns the socket fd or -1 on failure.
inline int connect_to_server(const std::string& host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;

    // Disable Nagle's algorithm: our messages are small and we want
    // each request to go out immediately (otherwise latency looks bad).
    int opt = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof(opt));

    sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, host.c_str(), &addr.sin_addr) <= 0) {
        close(fd);
        return -1;
    }
    if (connect(fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

// Current time in microseconds (monotonic clock, safe for measuring intervals).
inline long long now_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

#endif
