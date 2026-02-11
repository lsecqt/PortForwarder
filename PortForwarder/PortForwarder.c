/*
 * Windows TCP Port Forwarder with IP Filtering
 * Redirects TCP traffic from a local port to a remote host:port
 * Optionally filters by source IP address
 *
 * Usage: PortForwarder.exe <local_port> <remote_host> <remote_port> [allowed_ip] [-v]
 * Example: PortForwarder.exe 8080 192.168.1.100 80
 * Example: PortForwarder.exe 8080 192.168.1.100 80 192.168.1.50
 * Example: PortForwarder.exe 8080 192.168.1.100 80 192.168.1.50 -v
 */

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <mstcpip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#pragma comment(lib, "ws2_32.lib")

#define BUFFER_SIZE 8192
#define MAX_CONNECTIONS 100

typedef struct {
    SOCKET client_socket;
    SOCKET remote_socket;
    HANDLE thread_handle;
    int active;
    unsigned long long bytes_client_to_remote;
    unsigned long long bytes_remote_to_client;
} connection_t;

// Global variables for cleanup
connection_t connections[MAX_CONNECTIONS];
SOCKET listen_socket = INVALID_SOCKET;
volatile int running = 1;
CRITICAL_SECTION conn_lock;
char* allowed_ip = NULL;  // NULL means allow all IPs
int verbose_mode = 0;     // Verbose mode for IP filtering (default: off)

// Forward declarations
void cleanup();
BOOL WINAPI console_handler(DWORD signal);

// Error handling function
void print_error(const char* msg) {
    fprintf(stderr, "[ERROR] %s: %d\n", msg, WSAGetLastError());
}

// Check if IP is allowed
int is_ip_allowed(const char* client_ip) {
    if (allowed_ip == NULL) {
        return 1;  // No filter, allow all
    }
    return strcmp(client_ip, allowed_ip) == 0;
}

// Forward data bidirectionally between two sockets
DWORD WINAPI forward_thread(LPVOID param) {
    connection_t* conn = (connection_t*)param;
    SOCKET client = conn->client_socket;
    SOCKET remote = conn->remote_socket;

    fd_set readfds;
    char buffer[BUFFER_SIZE];
    int max_fd;
    struct timeval timeout;

    // Set sockets to blocking mode for reliable data transfer
    u_long mode = 0;
    ioctlsocket(client, FIONBIO, &mode);
    ioctlsocket(remote, FIONBIO, &mode);

    // Set socket timeouts to detect dead connections
    int timeout_ms = 30000; // 30 seconds
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(remote, SOL_SOCKET, SO_RCVTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));
    setsockopt(remote, SOL_SOCKET, SO_SNDTIMEO, (char*)&timeout_ms, sizeof(timeout_ms));

    // Disable Nagle's algorithm for lower latency
    int flag = 1;
    setsockopt(client, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));
    setsockopt(remote, IPPROTO_TCP, TCP_NODELAY, (char*)&flag, sizeof(int));

    // Set keepalive with more aggressive settings
    int keepalive = 1;
    setsockopt(client, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive, sizeof(int));
    setsockopt(remote, SOL_SOCKET, SO_KEEPALIVE, (char*)&keepalive, sizeof(int));

    // Set TCP keepalive parameters (Windows-specific)
    struct tcp_keepalive ka_settings;
    ka_settings.onoff = 1;
    ka_settings.keepalivetime = 10000;     // Start probing after 10 seconds
    ka_settings.keepaliveinterval = 1000;  // Probe every 1 second
    DWORD bytes_returned;
    WSAIoctl(client, SIO_KEEPALIVE_VALS, &ka_settings, sizeof(ka_settings),
        NULL, 0, &bytes_returned, NULL, NULL);
    WSAIoctl(remote, SIO_KEEPALIVE_VALS, &ka_settings, sizeof(ka_settings),
        NULL, 0, &bytes_returned, NULL, NULL);

    printf("[INFO] Connection established, forwarding traffic...\n");

    conn->bytes_client_to_remote = 0;
    conn->bytes_remote_to_client = 0;

    while (running && conn->active) {
        FD_ZERO(&readfds);
        FD_SET(client, &readfds);
        FD_SET(remote, &readfds);

        // Calculate max fd (Windows doesn't use this but keep for portability reference)
        max_fd = (client > remote ? client : remote) + 1;

        // Wait for data with timeout
        timeout.tv_sec = 1;
        timeout.tv_usec = 0;

        int result = select(max_fd, &readfds, NULL, NULL, &timeout);

        if (result == SOCKET_ERROR) {
            print_error("select() failed");
            break;
        }

        if (result == 0) {
            // Timeout, continue loop to check running flag
            continue;
        }

        // Client -> Remote
        if (FD_ISSET(client, &readfds)) {
            int bytes_received = recv(client, buffer, BUFFER_SIZE, 0);

            if (bytes_received <= 0) {
                if (bytes_received == 0) {
                    printf("[INFO] Client closed connection gracefully\n");
                }
                else {
                    int error = WSAGetLastError();
                    // Handle common errors gracefully
                    switch (error) {
                    case WSAECONNRESET:
                        printf("[INFO] Client connection reset by peer\n");
                        break;
                    case WSAECONNABORTED:
                        printf("[INFO] Client connection aborted\n");
                        break;
                    case WSAENETRESET:
                        printf("[INFO] Network reset caused client disconnect\n");
                        break;
                    case WSAETIMEDOUT:
                        printf("[INFO] Client connection timed out\n");
                        break;
                    default:
                        fprintf(stderr, "[ERROR] recv() from client failed: %d\n", error);
                    }
                }
                break;
            }

            // Forward all data to remote
            int total_sent = 0;
            while (total_sent < bytes_received) {
                int bytes_sent = send(remote, buffer + total_sent,
                    bytes_received - total_sent, 0);

                if (bytes_sent == SOCKET_ERROR) {
                    int error = WSAGetLastError();
                    switch (error) {
                    case WSAECONNRESET:
                        printf("[INFO] Remote connection reset while sending\n");
                        break;
                    case WSAECONNABORTED:
                        printf("[INFO] Remote connection aborted while sending\n");
                        break;
                    default:
                        fprintf(stderr, "[ERROR] send() to remote failed: %d\n", error);
                    }
                    goto cleanup_thread;
                }
                total_sent += bytes_sent;
            }
            conn->bytes_client_to_remote += bytes_received;
        }

        // Remote -> Client
        if (FD_ISSET(remote, &readfds)) {
            int bytes_received = recv(remote, buffer, BUFFER_SIZE, 0);

            if (bytes_received <= 0) {
                if (bytes_received == 0) {
                    printf("[INFO] Remote closed connection gracefully\n");
                }
                else {
                    int error = WSAGetLastError();
                    // Handle common errors gracefully
                    switch (error) {
                    case WSAECONNRESET:
                        printf("[INFO] Remote connection reset by peer\n");
                        break;
                    case WSAECONNABORTED:
                        printf("[INFO] Remote connection aborted\n");
                        break;
                    case WSAENETRESET:
                        printf("[INFO] Network reset caused remote disconnect\n");
                        break;
                    case WSAETIMEDOUT:
                        printf("[INFO] Remote connection timed out\n");
                        break;
                    default:
                        fprintf(stderr, "[ERROR] recv() from remote failed: %d\n", error);
                    }
                }
                break;
            }

            // Forward all data to client
            int total_sent = 0;
            while (total_sent < bytes_received) {
                int bytes_sent = send(client, buffer + total_sent,
                    bytes_received - total_sent, 0);

                if (bytes_sent == SOCKET_ERROR) {
                    int error = WSAGetLastError();
                    switch (error) {
                    case WSAECONNRESET:
                        printf("[INFO] Client connection reset while sending\n");
                        break;
                    case WSAECONNABORTED:
                        printf("[INFO] Client connection aborted while sending\n");
                        break;
                    default:
                        fprintf(stderr, "[ERROR] send() to client failed: %d\n", error);
                    }
                    goto cleanup_thread;
                }
                total_sent += bytes_sent;
            }
            conn->bytes_remote_to_client += bytes_received;
        }
    }

cleanup_thread:
    printf("[INFO] Closing connection (Sent: %llu bytes, Received: %llu bytes, Total: %llu bytes)\n",
        conn->bytes_client_to_remote, conn->bytes_remote_to_client,
        conn->bytes_client_to_remote + conn->bytes_remote_to_client);

    // Graceful shutdown
    shutdown(client, SD_BOTH);
    shutdown(remote, SD_BOTH);
    closesocket(client);
    closesocket(remote);

    EnterCriticalSection(&conn_lock);
    conn->active = 0;
    LeaveCriticalSection(&conn_lock);

    return 0;
}

// Handle new client connection
int handle_connection(SOCKET client_socket, const char* remote_host, int remote_port) {
    SOCKET remote_socket = INVALID_SOCKET;
    struct addrinfo hints, * result = NULL;
    char port_str[16];
    int conn_index = -1;

    // Convert port to string
    snprintf(port_str, sizeof(port_str), "%d", remote_port);

    // Resolve remote address
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;

    if (getaddrinfo(remote_host, port_str, &hints, &result) != 0) {
        print_error("getaddrinfo() failed");
        closesocket(client_socket);
        return -1;
    }

    // Create socket and connect to remote
    remote_socket = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (remote_socket == INVALID_SOCKET) {
        print_error("socket() creation failed");
        freeaddrinfo(result);
        closesocket(client_socket);
        return -1;
    }

    if (connect(remote_socket, result->ai_addr, (int)result->ai_addrlen) == SOCKET_ERROR) {
        print_error("connect() to remote failed");
        freeaddrinfo(result);
        closesocket(remote_socket);
        closesocket(client_socket);
        return -1;
    }

    freeaddrinfo(result);
    printf("[INFO] Connected to remote %s:%d\n", remote_host, remote_port);

    // Find free connection slot
    EnterCriticalSection(&conn_lock);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (!connections[i].active) {
            conn_index = i;
            connections[i].client_socket = client_socket;
            connections[i].remote_socket = remote_socket;
            connections[i].active = 1;
            break;
        }
    }
    LeaveCriticalSection(&conn_lock);

    if (conn_index == -1) {
        fprintf(stderr, "[ERROR] Maximum connections reached\n");
        closesocket(remote_socket);
        closesocket(client_socket);
        return -1;
    }

    // Create forwarding thread
    connections[conn_index].thread_handle = CreateThread(
        NULL, 0, forward_thread, &connections[conn_index], 0, NULL);

    if (connections[conn_index].thread_handle == NULL) {
        fprintf(stderr, "[ERROR] CreateThread() failed: %lu\n", GetLastError());
        EnterCriticalSection(&conn_lock);
        connections[conn_index].active = 0;
        LeaveCriticalSection(&conn_lock);
        closesocket(remote_socket);
        closesocket(client_socket);
        return -1;
    }

    return 0;
}

// Cleanup function
void cleanup() {
    printf("\n[INFO] Shutting down...\n");
    running = 0;

    // Close listening socket
    if (listen_socket != INVALID_SOCKET) {
        closesocket(listen_socket);
        listen_socket = INVALID_SOCKET;
    }

    // Wait for all threads to finish
    EnterCriticalSection(&conn_lock);
    for (int i = 0; i < MAX_CONNECTIONS; i++) {
        if (connections[i].active) {
            connections[i].active = 0;
            if (connections[i].thread_handle != NULL) {
                LeaveCriticalSection(&conn_lock);
                WaitForSingleObject(connections[i].thread_handle, 5000);
                EnterCriticalSection(&conn_lock);
                CloseHandle(connections[i].thread_handle);
            }
        }
    }
    LeaveCriticalSection(&conn_lock);

    DeleteCriticalSection(&conn_lock);
    WSACleanup();
    printf("[INFO] Cleanup complete\n");
}

// Console control handler
BOOL WINAPI console_handler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT) {
        cleanup();
        exit(0);
    }
    return TRUE;
}

int main(int argc, char* argv[]) {
    WSADATA wsa_data;
    struct sockaddr_in server_addr;
    int local_port, remote_port;
    char* remote_host;

    printf("=== Windows TCP Port Forwarder with IP Filtering ===\n\n");

    // Parse arguments
    if (argc < 4 || argc > 6) {
        fprintf(stderr, "Usage: %s <local_port> <remote_host> <remote_port> [allowed_ip] [-v]\n", argv[0]);
        fprintf(stderr, "  -v: Enable verbose mode (show rejected connections)\n\n");
        fprintf(stderr, "Examples:\n");
        fprintf(stderr, "  %s 8080 192.168.1.100 80\n", argv[0]);
        fprintf(stderr, "  %s 8080 192.168.1.100 80 192.168.1.50\n", argv[0]);
        fprintf(stderr, "  %s 8080 192.168.1.100 80 192.168.1.50 -v\n", argv[0]);
        return 1;
    }

    local_port = atoi(argv[1]);
    remote_host = argv[2];
    remote_port = atoi(argv[3]);

    // Parse optional arguments (allowed_ip and -v flag)
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--verbose") == 0) {
            verbose_mode = 1;
        }
        else {
            // Assume it's the allowed IP
            allowed_ip = argv[i];
        }
    }

    if (local_port <= 0 || local_port > 65535 || remote_port <= 0 || remote_port > 65535) {
        fprintf(stderr, "[ERROR] Invalid port number\n");
        return 1;
    }

    printf("[INFO] Configuration:\n");
    printf("  Local port:  %d\n", local_port);
    printf("  Remote host: %s\n", remote_host);
    printf("  Remote port: %d\n", remote_port);
    if (allowed_ip != NULL) {
        printf("  Allowed IP:  %s (filtered mode)\n", allowed_ip);
        printf("  Verbose:     %s\n", verbose_mode ? "ON" : "OFF");
    }
    else {
        printf("  Allowed IP:  ANY (no filtering)\n");
    }
    printf("\n");

    // Initialize Winsock
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data) != 0) {
        fprintf(stderr, "[ERROR] WSAStartup() failed\n");
        return 1;
    }

    // Initialize critical section and connections
    InitializeCriticalSection(&conn_lock);
    memset(connections, 0, sizeof(connections));

    // Set console handler for cleanup
    SetConsoleCtrlHandler(console_handler, TRUE);

    // Create listening socket
    listen_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_socket == INVALID_SOCKET) {
        print_error("socket() creation failed");
        WSACleanup();
        return 1;
    }

    // Set socket options
    int reuse = 1;
    if (setsockopt(listen_socket, SOL_SOCKET, SO_REUSEADDR,
        (char*)&reuse, sizeof(reuse)) == SOCKET_ERROR) {
        print_error("setsockopt() failed");
    }

    // Bind socket
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(local_port);

    if (bind(listen_socket, (struct sockaddr*)&server_addr, sizeof(server_addr)) == SOCKET_ERROR) {
        print_error("bind() failed");
        cleanup();
        return 1;
    }

    // Listen for connections
    if (listen(listen_socket, SOMAXCONN) == SOCKET_ERROR) {
        print_error("listen() failed");
        cleanup();
        return 1;
    }

    printf("[INFO] Listening on port %d...\n", local_port);
    printf("[INFO] Press Ctrl+C to stop\n\n");

    // Accept connections
    while (running) {
        struct sockaddr_in client_addr;
        int client_addr_len = sizeof(client_addr);

        SOCKET client_socket = accept(listen_socket,
            (struct sockaddr*)&client_addr,
            &client_addr_len);

        if (client_socket == INVALID_SOCKET) {
            if (running) {
                print_error("accept() failed");
            }
            break;
        }

        char client_ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, client_ip, INET_ADDRSTRLEN);

        // Check if IP is allowed
        if (!is_ip_allowed(client_ip)) {
            if (verbose_mode) {
                printf("[INFO] Connection from %s:%d REJECTED (IP not allowed)\n",
                    client_ip, ntohs(client_addr.sin_port));
            }
            closesocket(client_socket);
            continue;
        }

        printf("[INFO] New connection from %s:%d ACCEPTED\n",
            client_ip, ntohs(client_addr.sin_port));

        // Handle connection in new thread
        handle_connection(client_socket, remote_host, remote_port);
    }

    cleanup();
    return 0;
}