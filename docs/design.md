# Multi-User Client-Server Messaging System

## 1. Overview
The system is a classic client-server chat platform implemented fully in C on Linux/macOS. A single multi-threaded server process coordinates all communication, persistence, and session management. Each client is an interactive CLI that connects to the server over TCP sockets, authenticates with a unique username, and exchanges commands encoded as human-readable text. All chat history lives exclusively on the server in an embedded SQLite database; clients hold no local state beyond their current session.

Key OS concepts exercised:
- **Process management** – Separate server/client processes; server forks no children but spawns threads per connection.
- **Concurrency & threading** – Server maintains a detached POSIX thread per client plus a dispatcher loop that accepts new sockets; client uses a reader thread to display live notifications while the main thread handles user input.
- **Synchronization** – Mutexes guard global data (active user table, socket map, SQLite handle). Condition variables coordinate broadcast/shutdown events.
- **IPC** – TCP socket protocol carries commands and notifications between processes.
- **I/O management** – Server employs non-blocking accept loop with `select()` timeout to watch for shutdown signals; client reader thread uses blocking `recv()` to avoid busy-waiting.
- **Persistence** – SQLite database for durable message history with ACID guarantees; all CRUD is centralized on server.
- **Signal handling** – Server traps `SIGINT/SIGTERM` to initiate graceful broadcast shutdown; clients react to special `SERVER_SHUTDOWN` notification.

## 2. Architecture
```
+-------------------------+                  +-------------------------+
|        Client CLI       |                  |        Client CLI       |
|  main thread (input)    |                  |  main thread (input)    |
|  reader thread (recv)   |<----TCP---->     |  reader thread (recv)   |
+-------------------------+                  +-------------------------+
             \                                      /
              \                                    /
               v                                  v
                +--------------------------------+
                |  Multi-threaded Chat Server    |
                |  listener thread (accept)      |
                |  worker thread per connection  |
                |  shared state (user table, DB) |
                +--------------------------------+
                             |
                             v
                   +-----------------+
                   | SQLite Database |
                   +-----------------+
```

## 3. Server design
### 3.1 Modules
- `main.c`: boots networking stack, loads config, opens SQLite DB, sets signal handlers.
- `connection.c`: accepts sockets, spawns worker threads, tracks active clients.
- `commands.c`: parses textual commands (e.g., `sendmessage <user> <text>`) and invokes relevant handlers.
- `storage.c`: wraps SQLite CRUD logic for storing, fetching, deleting messages.
- `broadcast.c`: pushes asynchronous notifications (message delivery, shutdown) to connected clients.
- `shutdown.c`: coordinates graceful teardown, flushes DB, joins threads.

### 3.2 Data structures
```c
typedef struct {
    char username[MAX_NAME];
    int socket_fd;
    pthread_t thread_id;
    bool online;
} active_user_t;
```
- Active users stored in hash map keyed by username for O(1) lookup.
- Mutex `users_lock` protects the map; condition variable `shutdown_cv` wakes workers when shutdown initiated.
- SQLite handle shared; `sqlite_mutex` serializes DB access to avoid concurrent writer conflicts (SQLite is serialized by default but the extra lock keeps code portable if compile-time options change).

### 3.3 Threading model
1. **Listener thread**: Accepts incoming sockets in a blocking loop. After verifying username uniqueness, spawns a detached worker thread.
2. **Worker threads**: Responsible for one client connection. They:
   - Read newline-delimited commands.
   - Execute server-side logic (send, get, delete, list, quit).
   - Push asynchronous notifications to their client (incoming messages, shutdown broadcast).
3. **Broadcaster**: Logical role implemented via helper that iterates active user map when pushing events (e.g., server shutdown message).

### 3.4 Synchronization
- `users_lock` protects the active user map for add/remove/list operations.
- `db_lock` wraps SQLite operations.
- Each socket send uses `send_lock` per client to avoid interleaved writes when both the worker thread and broadcast helper send concurrently.

## 4. Client design
### 4.1 Components
- `main.c`: Parses CLI args, establishes TCP connection, authenticates username.
- **Input thread (main)**: Reads user commands, formats protocol strings, sends to server.
- **Receiver thread**: Blocks on `recv()` and prints notifications immediately (messages, errors, shutdown).
- Shared queue not required because output is immediate. A mutex on stdout prevents message interleaving.

### 4.2 Commands
- `connect <server_ip> <port> <username>` handled externally; client binary uses CLI options.
- `sendmessage <user> <message>`
- `getmessages <user>`
- `deletemessages <user>`
- `getuserlist`
- `quit`

### 4.3 Error handling
- Lost connection triggers receiver thread shutdown and notifies input loop.
- Invalid commands elicit human-readable error messages.

## 5. Application protocol
Simple newline-delimited text. Example exchange:
```
CLIENT: AUTH john
SERVER: OK
CLIENT: SEND alice Hey there!
SERVER: DELIVERED
# when alice online, her receiver thread prints
SERVER->ALICE: MESSAGE john Hey there!
```
Responses always start with a keyword (`OK`, `ERROR`, `MESSAGE`, `SHUTDOWN`), simplifying parsing. Message bodies are quoted or transmitted after a space until newline.

## 6. Persistence layer
- SQLite database `chat.db` with table:
```sql
CREATE TABLE IF NOT EXISTS messages (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    sender TEXT NOT NULL,
    receiver TEXT NOT NULL,
    body TEXT NOT NULL,
    created_at DATETIME DEFAULT CURRENT_TIMESTAMP
);
```
- `store_message(sender, receiver, body)` inserts row per delivery attempt.
- `fetch_conversation(user_a, user_b)` returns ordered history for `getmessages`.
- `delete_conversation(user_a, user_b)` removes all rows both directions.

## 7. Shutdown handling
- Server captures `SIGINT/SIGTERM`, sets `server_running=false`, stops accepting new connections, broadcasts `SHUTDOWN Server shutting down...` to all clients, closes DB, and joins threads.
- Clients receiving `SHUTDOWN` print the notice, close sockets, and exit.

## 8. Testing strategy
1. **Unit tests (logic level)** – Use lightweight C test harness (or simple assertions compiled into `test_server.c`) to verify storage helpers and command parsing without real sockets.
2. **Integration tests** – Shell script that starts server, spawns multiple clients via `expect`, and validates message exchange, history retrieval, deletion, and shutdown broadcast.
3. **Stress tests** – Run `./client` in a loop to simulate >5 concurrent users; monitor server output for race conditions or crashes.
4. **Manual demo checklist** – Provided in README; includes steps for live presentation per assignment rubric.

## 9. Responsibilities & workflow
- Repo uses `Makefile` targets: `make server`, `make client`, `make test`, `make run-server`, `make run-client`.
- Recommended development flow: implement storage layer first, then networking, finally CLI polish.
- Code style: POSIX C11, `-Wall -Wextra -pthread`.
