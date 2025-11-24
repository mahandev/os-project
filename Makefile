CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -pthread -Iinclude
LDFLAGS_SERVER ?= -lsqlite3 -pthread
LDFLAGS_CLIENT ?= -pthread
BIN_DIR ?= bin
WINDOWS_BIN_DIR ?= $(BIN_DIR)/windows
WINDOWS_CC ?= x86_64-w64-mingw32-gcc
WINDOWS_CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -Iinclude
WINDOWS_LDFLAGS_SERVER ?= -lws2_32 -lpthread
WINDOWS_LDFLAGS_CLIENT ?= -lws2_32 -lpthread
PORT ?= 5555
SERVER ?= 127.0.0.1
USER ?= demo
SERVER_SRCS := src/server/server.c src/server/storage.c
CLIENT_SRC := src/client/client.c

all: $(BIN_DIR)/server $(BIN_DIR)/client

windows: $(WINDOWS_BIN_DIR)/server.exe $(WINDOWS_BIN_DIR)/client.exe

windows-server: $(WINDOWS_BIN_DIR)/server.exe

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(BIN_DIR)/server: $(SERVER_SRCS) | $(BIN_DIR)
	$(CC) $(CFLAGS) $(SERVER_SRCS) -o $@ $(LDFLAGS_SERVER)

$(BIN_DIR)/client: $(CLIENT_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_CLIENT)

$(WINDOWS_BIN_DIR):
	@mkdir -p $(WINDOWS_BIN_DIR)

$(WINDOWS_BIN_DIR)/server.exe: $(SERVER_SRCS) | $(WINDOWS_BIN_DIR)
	$(WINDOWS_CC) $(WINDOWS_CFLAGS) -DSTORAGE_USE_SQLITE=0 $(SERVER_SRCS) -o $@ $(WINDOWS_LDFLAGS_SERVER)

$(WINDOWS_BIN_DIR)/client.exe: $(CLIENT_SRC) | $(WINDOWS_BIN_DIR)
	$(WINDOWS_CC) $(WINDOWS_CFLAGS) $< -o $@ $(WINDOWS_LDFLAGS_CLIENT)

clean:
	rm -rf $(BIN_DIR) chat.db

run-server: $(BIN_DIR)/server
	$(BIN_DIR)/server $(PORT)

run-client: $(BIN_DIR)/client
	$(BIN_DIR)/client $(SERVER) $(PORT) $(USER)

.PHONY: all clean run-server run-client windows windows-server
