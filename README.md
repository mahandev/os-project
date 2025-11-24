# Multi-User Client-Server Messaging Application

Hands-on Operating Systems project implementing a persistent, multi-user chat system in C with POSIX sockets, threads, synchronization, and SQLite storage.

## Features
- Multi-threaded server supporting 5+ concurrent clients with unique usernames.
- Real-time messaging routed through the server; online users receive pushes instantly.
- Persistent history via SQLite (no client-side storage).
- Retrieval (`getmessages`) and deletion (`deletemessages`) of full conversation history.
- Live user list and graceful shutdown broadcast.
- Thread-safe client CLI with asynchronous receiver thread.

## Repository layout
```
├── Makefile               # build + helper targets
├── README.md
├── docs/
│   └── design.md          # architecture & concurrency design
├── include/
├── src/
│   ├── client/client.c    # CLI implementation
│   └── server/server.c    # multi-threaded server
└── tests/
    └── protocol_smoke.py  # automated socket-level smoke test
```

## Building
Requirements: gcc/clang, pthreads, SQLite3 development headers, Python 3 (for tests).

```bash
make            # builds bin/server and bin/client
make clean      # removes binaries and chat.db
```

## Running
Start the server (choose any free port, default via `PORT` variable is 5555):
```bash
PORT=5555 make run-server
```

Launch clients (each in its own terminal tab/window):
```bash
PORT=5555 SERVER=127.0.0.1 USER=alice make run-client
PORT=5555 SERVER=127.0.0.1 USER=bob make run-client
```

### Client commands
| Command | Description |
| --- | --- |
| `sendmessage <user> <message>` | Send text to another user. |
| `getmessages <user>` | Stream full conversation history with `<user>`. |
| `deletemessages <user>` | Delete stored history with `<user>`. |
| `getuserlist` | List connected users. |
| `quit` | Disconnect gracefully. |

Server shutdown (Ctrl+C) broadcasts `Server shutting down…` and disconnects all clients.

## Testing
Compile first (`make`). Then run the automated end-to-end smoke test:
```bash
python3 tests/protocol_smoke.py
```
The script boots the server, simulates two clients, verifies live delivery, history fetch, deletion, and user list, then tears everything down.

## Design & documentation
Details on architecture, threading model, synchronization, database schema, and testing plan live in `docs/design.md`. Keep that document updated if you extend the system (e.g., new commands, alternative storage backends).
