#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define MAX_USERNAME 32
#define MAX_MESSAGE 1024
#define LISTEN_BACKLOG 16
#define DEFAULT_DB_PATH "chat.db"

typedef struct client_session {
    int socket_fd;
    pthread_t thread;
    char username[MAX_USERNAME];
    bool authenticated;
    pthread_mutex_t send_lock;
    struct client_session *next;
} client_session_t;

static client_session_t *clients_head = NULL;
static pthread_mutex_t clients_lock = PTHREAD_MUTEX_INITIALIZER;
static sqlite3 *db = NULL;
static pthread_mutex_t db_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile sig_atomic_t server_running = 1;
static int listener_fd = -1;

static void fatal(const char *msg) {
    perror(msg);
    exit(EXIT_FAILURE);
}

static void send_formatted(client_session_t *session, const char *fmt, ...) {
    char buffer[2048];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    size_t len = strlen(buffer);
    if (len < sizeof(buffer) - 1) {
        buffer[len++] = '\n';
    }
    buffer[len] = '\0';

    pthread_mutex_lock(&session->send_lock);
    ssize_t written = send(session->socket_fd, buffer, strlen(buffer), 0);
    (void)written; // best-effort send; errors handled by read loop
    pthread_mutex_unlock(&session->send_lock);
}

static void broadcast_shutdown_message(void) {
    pthread_mutex_lock(&clients_lock);
    client_session_t *cur = clients_head;
    while (cur) {
        if (cur->authenticated) {
            send_formatted(cur, "SHUTDOWN Server shutting down...");
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&clients_lock);
}

static void handle_signal(int signum) {
    (void)signum;
    server_running = 0;
    if (listener_fd != -1) {
        close(listener_fd);
        listener_fd = -1;
    }
}

static void install_signal_handlers(void) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
}

static int init_database(const char *path) {
    if (sqlite3_open(path, &db) != SQLITE_OK) {
        fprintf(stderr, "Failed to open database: %s\n", sqlite3_errmsg(db));
        return -1;
    }
    const char *full_sql =
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "sender TEXT NOT NULL,"
        "receiver TEXT NOT NULL,"
        "body TEXT NOT NULL,"
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP" ");";
    char *err = NULL;
    if (sqlite3_exec(db, full_sql, NULL, NULL, &err) != SQLITE_OK) {
        fprintf(stderr, "Failed to init schema: %s\n", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

static int store_message(const char *sender, const char *receiver, const char *body) {
    const char *sql = "INSERT INTO messages (sender, receiver, body) VALUES (?, ?, ?);";
    pthread_mutex_lock(&db_lock);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_lock);
        return -1;
    }
    sqlite3_bind_text(stmt, 1, sender, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, receiver, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, body, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    return rc == SQLITE_DONE ? 0 : -1;
}

static void send_conversation(client_session_t *session, const char *other_user) {
    const char *sql =
        "SELECT sender, body, datetime(created_at) FROM messages "
        "WHERE (sender=? AND receiver=?) OR (sender=? AND receiver=?) "
        "ORDER BY created_at ASC";
    pthread_mutex_lock(&db_lock);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_lock);
        send_formatted(session, "ERROR Failed to query history: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, session->username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, other_user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, other_user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session->username, -1, SQLITE_TRANSIENT);

    bool any = false;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const unsigned char *sender = sqlite3_column_text(stmt, 0);
        const unsigned char *body = sqlite3_column_text(stmt, 1);
        const unsigned char *ts = sqlite3_column_text(stmt, 2);
        send_formatted(session, "HISTORY %s %s %s", ts, sender, body);
        any = true;
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    if (!any) {
        send_formatted(session, "INFO No messages with %s", other_user);
    } else {
        send_formatted(session, "OK History end");
    }
}

static void delete_conversation(client_session_t *session, const char *other_user) {
    const char *sql =
        "DELETE FROM messages WHERE (sender=? AND receiver=?) OR (sender=? AND receiver=?)";
    pthread_mutex_lock(&db_lock);
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&db_lock);
        send_formatted(session, "ERROR Failed to delete history: %s", sqlite3_errmsg(db));
        return;
    }
    sqlite3_bind_text(stmt, 1, session->username, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, other_user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, other_user, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, session->username, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&db_lock);
    if (rc == SQLITE_DONE) {
        send_formatted(session, "OK Deleted history with %s", other_user);
    } else {
        send_formatted(session, "ERROR Failed to delete history");
    }
}

static client_session_t *find_client_by_name(const char *username) {
    client_session_t *cur = clients_head;
    while (cur) {
        if (cur->authenticated && strncmp(cur->username, username, MAX_USERNAME) == 0) {
            return cur;
        }
        cur = cur->next;
    }
    return NULL;
}

static bool username_available(const char *username) {
    return find_client_by_name(username) == NULL;
}

static void add_client(client_session_t *session) {
    pthread_mutex_lock(&clients_lock);
    session->next = clients_head;
    clients_head = session;
    pthread_mutex_unlock(&clients_lock);
}

static void remove_client(client_session_t *session) {
    pthread_mutex_lock(&clients_lock);
    client_session_t **cur = &clients_head;
    while (*cur) {
        if (*cur == session) {
            *cur = session->next;
            break;
        }
        cur = &((*cur)->next);
    }
    pthread_mutex_unlock(&clients_lock);
}

static void notify_user_list(client_session_t *session) {
    pthread_mutex_lock(&clients_lock);
    client_session_t *cur = clients_head;
    send_formatted(session, "USERS_BEGIN");
    while (cur) {
        if (cur->authenticated) {
            send_formatted(session, "USER %s", cur->username);
        }
        cur = cur->next;
    }
    pthread_mutex_unlock(&clients_lock);
    send_formatted(session, "USERS_END");
}

static void deliver_message(const char *sender, const char *receiver, const char *body) {
    if (store_message(sender, receiver, body) != 0) {
        fprintf(stderr, "Failed to persist message from %s to %s\n", sender, receiver);
    }
    pthread_mutex_lock(&clients_lock);
    client_session_t *target = find_client_by_name(receiver);
    if (target) {
        send_formatted(target, "MESSAGE %s %s", sender, body);
    }
    pthread_mutex_unlock(&clients_lock);
}

static ssize_t read_line(int fd, char *buffer, size_t max_len) {
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

static void trim_newline(char *str) {
    size_t len = strlen(str);
    if (len == 0) {
        return;
    }
    char *end = str + len - 1;
    while (end >= str && (*end == '\n' || *end == '\r' || *end == ' ')) {
        *end = '\0';
        --end;
    }
}

static void *client_worker(void *arg) {
    client_session_t *session = (client_session_t *)arg;
    send_formatted(session, "WELCOME Provide AUTH <username>");
    char line[2048];
    while (server_running) {
        ssize_t len = read_line(session->socket_fd, line, sizeof(line));
        if (len <= 0) {
            break;
        }
        if (!session->authenticated) {
            if (strncmp(line, "AUTH ", 5) == 0) {
                char *username = line + 5;
                trim_newline(username);
                if (strlen(username) == 0 || strlen(username) >= MAX_USERNAME) {
                    send_formatted(session, "ERROR Invalid username length");
                    continue;
                }
                pthread_mutex_lock(&clients_lock);
                bool available = username_available(username);
                if (available) {
                    strncpy(session->username, username, sizeof(session->username));
                    session->authenticated = true;
                    pthread_mutex_unlock(&clients_lock);
                    send_formatted(session, "OK Authenticated as %s", session->username);
                } else {
                    pthread_mutex_unlock(&clients_lock);
                    send_formatted(session, "ERROR Username taken");
                }
            } else {
                send_formatted(session, "ERROR Authenticate first using AUTH <username>");
            }
            continue;
        }

        if (strncmp(line, "SEND ", 5) == 0) {
            char *rest = line + 5;
            char *space = strchr(rest, ' ');
            if (!space) {
                send_formatted(session, "ERROR Usage: SEND <user> <message>");
                continue;
            }
            *space = '\0';
            const char *target = rest;
            const char *message = space + 1;
            if (strlen(message) == 0) {
                send_formatted(session, "ERROR Message cannot be empty");
                continue;
            }
            deliver_message(session->username, target, message);
            send_formatted(session, "OK Message queued");
            continue;
        }

        if (strncmp(line, "GET ", 4) == 0) {
            const char *other = line + 4;
            if (strlen(other) == 0) {
                send_formatted(session, "ERROR Usage: GET <user>");
                continue;
            }
            send_conversation(session, other);
            continue;
        }

        if (strncmp(line, "DELETE ", 7) == 0) {
            const char *other = line + 7;
            if (strlen(other) == 0) {
                send_formatted(session, "ERROR Usage: DELETE <user>");
                continue;
            }
            delete_conversation(session, other);
            continue;
        }

        if (strcmp(line, "USERS") == 0) {
            notify_user_list(session);
            continue;
        }

        if (strcmp(line, "QUIT") == 0) {
            send_formatted(session, "BYE");
            break;
        }

        send_formatted(session, "ERROR Unknown command");
    }

    close(session->socket_fd);
    pthread_mutex_destroy(&session->send_lock);
    if (session->authenticated) {
        printf("User %s disconnected\n", session->username);
    }
    remove_client(session);
    free(session);
    return NULL;
}

static void accept_loop(uint16_t port) {
    listener_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listener_fd == -1) {
        fatal("socket");
    }
    int opt = 1;
    setsockopt(listener_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);
    if (bind(listener_fd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        fatal("bind");
    }
    if (listen(listener_fd, LISTEN_BACKLOG) == -1) {
        fatal("listen");
    }
    printf("Server listening on port %u\n", port);
    while (server_running) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_fd = accept(listener_fd, (struct sockaddr *)&client_addr, &client_len);
        if (client_fd == -1) {
            if (errno == EINTR) {
                continue;
            }
            perror("accept");
            break;
        }
        client_session_t *session = calloc(1, sizeof(client_session_t));
        if (!session) {
            fprintf(stderr, "Out of memory\n");
            close(client_fd);
            continue;
        }
        session->socket_fd = client_fd;
        pthread_mutex_init(&session->send_lock, NULL);
        add_client(session);
        if (pthread_create(&session->thread, NULL, client_worker, session) != 0) {
            fprintf(stderr, "Failed to create worker thread\n");
            remove_client(session);
            close(client_fd);
            pthread_mutex_destroy(&session->send_lock);
            free(session);
            continue;
        }
        pthread_detach(session->thread);
        printf("Incoming connection accepted\n");
    }
}

int main(int argc, char **argv) {
    if (argc < 2 || argc > 3) {
        fprintf(stderr, "Usage: %s <port> [db_path]\n", argv[0]);
        return EXIT_FAILURE;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);
    const char *db_path = (argc == 3) ? argv[2] : DEFAULT_DB_PATH;

    install_signal_handlers();
    if (init_database(db_path) != 0) {
        return EXIT_FAILURE;
    }

    accept_loop(port);
    broadcast_shutdown_message();

    if (listener_fd != -1) {
        close(listener_fd);
    }
    pthread_mutex_lock(&clients_lock);
    client_session_t *cur = clients_head;
    while (cur) {
        close(cur->socket_fd);
        cur = cur->next;
    }
    pthread_mutex_unlock(&clients_lock);
    if (db) {
        sqlite3_close(db);
    }
    printf("Server shutdown complete\n");
    return EXIT_SUCCESS;
}
