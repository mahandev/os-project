#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/types.h>
#ifndef _WIN32
#include <unistd.h>
#endif

#include "net_compat.h"

#define MAX_USERNAME 32
#define MAX_LINE 2048

static socket_handle_t server_fd = NET_INVALID_SOCKET;
static pthread_t receiver_thread;
static volatile sig_atomic_t running = 1;
static pthread_mutex_t stdout_lock = PTHREAD_MUTEX_INITIALIZER;

static void safe_print(const char *fmt, ...) {
    pthread_mutex_lock(&stdout_lock);
    va_list args;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    fflush(stdout);
    pthread_mutex_unlock(&stdout_lock);
}

static ssize_t portable_getline(char **lineptr, size_t *n, FILE *stream) {
#ifdef _WIN32
    if (!lineptr || !n || !stream) {
        return -1;
    }
    if (*lineptr == NULL || *n == 0) {
        *n = 256;
        *lineptr = malloc(*n);
        if (*lineptr == NULL) {
            return -1;
        }
    }
    size_t total = 0;
    while (fgets(*lineptr + total, (int)(*n - total), stream)) {
        size_t chunk = strlen(*lineptr + total);
        total += chunk;
        if (total > 0 && (*lineptr)[total - 1] == '\n') {
            return (ssize_t)total;
        }
        size_t new_size = (*n) * 2;
        char *tmp = realloc(*lineptr, new_size);
        if (!tmp) {
            return -1;
        }
        *lineptr = tmp;
        *n = new_size;
    }
    return (total > 0) ? (ssize_t)total : -1;
#else
    return getline(lineptr, n, stream);
#endif
}

static ssize_t read_line(socket_handle_t fd, char *buffer, size_t max_len) {
    size_t offset = 0;
    while (offset < max_len - 1) {
        char c;
        ssize_t n = recv(fd, &c, 1, 0);
        if (n <= 0) {
            return n;
        }
        if (c == '\r') {
            continue;
        }
        if (c == '\n') {
            break;
        }
        buffer[offset++] = c;
    }
    buffer[offset] = '\0';
    return (ssize_t)offset;
}

static void send_command(const char *fmt, ...) {
    char buffer[MAX_LINE];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    size_t len = strlen(buffer);
    if (len >= sizeof(buffer) - 1) {
        len = sizeof(buffer) - 2;
        buffer[len] = '\0';
    }
    buffer[len] = '\n';
    buffer[len + 1] = '\0';
    send(server_fd, buffer, strlen(buffer), 0);
}

static void handle_server_line(const char *line) {
    if (strncmp(line, "MESSAGE ", 8) == 0) {
        const char *payload = line + 8;
        const char *space = strchr(payload, ' ');
        if (space) {
            char sender[MAX_USERNAME];
            size_t sender_len = space - payload;
            if (sender_len >= sizeof(sender)) {
                sender_len = sizeof(sender) - 1;
            }
            strncpy(sender, payload, sender_len);
            sender[sender_len] = '\0';
            const char *body = space + 1;
            safe_print("Message from %s: %s\n", sender, body);
        } else {
            safe_print("Message: %s\n", payload);
        }
    } else if (strncmp(line, "HISTORY ", 8) == 0) {
        safe_print("%s\n", line + 8);
    } else if (strncmp(line, "INFO ", 5) == 0) {
        safe_print("%s\n", line + 5);
    } else if (strncmp(line, "ERROR ", 6) == 0) {
        safe_print("Server error: %s\n", line + 6);
    } else if (strncmp(line, "OK", 2) == 0) {
        safe_print("%s\n", line);
    } else if (strncmp(line, "USER ", 5) == 0) {
        safe_print("User: %s\n", line + 5);
    } else if (strncmp(line, "USERS_BEGIN", 11) == 0) {
        safe_print("Active users:\n");
    } else if (strncmp(line, "USERS_END", 9) == 0) {
        safe_print("-- end of list --\n");
    } else if (strncmp(line, "BYE", 3) == 0) {
        safe_print("Disconnected by server\n");
        running = 0;
    } else if (strncmp(line, "SHUTDOWN", 8) == 0) {
        safe_print("%s\n", line + 9);
        running = 0;
    } else if (strncmp(line, "WELCOME", 7) == 0) {
        safe_print("%s\n", line);
    } else {
        safe_print("Server: %s\n", line);
    }
}

static void *receiver(void *arg) {
    (void)arg;
    char line[MAX_LINE];
    while (running) {
        ssize_t len = read_line(server_fd, line, sizeof(line));
        if (len <= 0) {
            safe_print("Connection closed by server\n");
            running = 0;
            break;
        }
        handle_server_line(line);
    }
    return NULL;
}

static void cleanup(void) {
    if (server_fd != NET_INVALID_SOCKET) {
        net_close(server_fd);
        server_fd = NET_INVALID_SOCKET;
    }
}

static void handle_sigint(int signum) {
    (void)signum;
    running = 0;
    if (server_fd != NET_INVALID_SOCKET) {
        shutdown(server_fd, SHUT_RDWR);
    }
}

static void usage(const char *prog) {
    fprintf(stderr, "Usage: %s <server_ip> <port> <username>\n", prog);
}

int main(int argc, char **argv) {
    if (argc != 4) {
        usage(argv[0]);
        return EXIT_FAILURE;
    }
    const char *server_ip = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *username = argv[3];
    if (strlen(username) == 0 || strlen(username) >= MAX_USERNAME) {
        fprintf(stderr, "Username must be 1-%d characters\n", MAX_USERNAME - 1);
        return EXIT_FAILURE;
    }

    if (net_init() != 0) {
        fprintf(stderr, "Failed to initialize networking\n");
        return EXIT_FAILURE;
    }

    #ifndef _WIN32
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_sigint;
    sigaction(SIGINT, &sa, NULL);
#else
    signal(SIGINT, handle_sigint);
#endif

    server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd == NET_INVALID_SOCKET) {
        perror("socket");
        net_cleanup();
        return EXIT_FAILURE;
    }

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    if (inet_pton(AF_INET, server_ip, &addr.sin_addr) != 1) {
        fprintf(stderr, "Invalid server IP\n");
        cleanup();
        net_cleanup();
        return EXIT_FAILURE;
    }

    if (connect(server_fd, (struct sockaddr *)&addr, sizeof(addr)) == NET_SOCKET_ERROR) {
        perror("connect");
        cleanup();
        net_cleanup();
        return EXIT_FAILURE;
    }

    char line[MAX_LINE];
    if (read_line(server_fd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "Failed to read server greeting\n");
        cleanup();
        net_cleanup();
        return EXIT_FAILURE;
    }
    printf("%s\n", line);
    send_command("AUTH %s", username);
    if (read_line(server_fd, line, sizeof(line)) <= 0) {
        fprintf(stderr, "Server closed during auth\n");
        cleanup();
        net_cleanup();
        return EXIT_FAILURE;
    }
    if (strncmp(line, "OK", 2) != 0) {
        fprintf(stderr, "Authentication failed: %s\n", line);
        cleanup();
        net_cleanup();
        return EXIT_FAILURE;
    }
    printf("%s\n", line);

    if (pthread_create(&receiver_thread, NULL, receiver, NULL) != 0) {
        fprintf(stderr, "Failed to create receiver thread\n");
        cleanup();
        return EXIT_FAILURE;
    }

    char *input = NULL;
    size_t input_len = 0;
    while (running) {
        printf("client> ");
        fflush(stdout);
        ssize_t read = portable_getline(&input, &input_len, stdin);
        if (read == -1) {
            break;
        }
        if (read > 0 && input[read - 1] == '\n') {
            input[read - 1] = '\0';
        }
        if (strncmp(input, "sendmessage ", 12) == 0) {
            char *rest = input + 12;
            char *space = strchr(rest, ' ');
            if (!space) {
                printf("Usage: sendmessage <user> <message>\n");
                continue;
            }
            *space = '\0';
            const char *target = rest;
            const char *message = space + 1;
            send_command("SEND %s %s", target, message);
        } else if (strncmp(input, "getmessages ", 12) == 0) {
            const char *user = input + 12;
            send_command("GET %s", user);
        } else if (strncmp(input, "deletemessages ", 15) == 0) {
            const char *user = input + 15;
            send_command("DELETE %s", user);
        } else if (strcmp(input, "getuserlist") == 0) {
            send_command("USERS");
        } else if (strcmp(input, "quit") == 0) {
            send_command("QUIT");
            running = 0;
            break;
        } else if (strlen(input) == 0) {
            continue;
        } else {
            printf("Unknown command. Use sendmessage/getmessages/deletemessages/getuserlist/quit\n");
        }
    }
    free(input);
    running = 0;
    if (server_fd != NET_INVALID_SOCKET) {
        shutdown(server_fd, SHUT_RDWR);
    }
    pthread_join(receiver_thread, NULL);
    cleanup();
    net_cleanup();
    return EXIT_SUCCESS;
}
