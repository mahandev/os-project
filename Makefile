CC ?= gcc
CFLAGS ?= -std=c11 -Wall -Wextra -pedantic -pthread -Iinclude
LDFLAGS_SERVER ?= -lsqlite3 -pthread
LDFLAGS_CLIENT ?= -pthread
BIN_DIR ?= bin
PORT ?= 5555
SERVER ?= 127.0.0.1
USER ?= demo
SERVER_SRC := src/server/server.c
CLIENT_SRC := src/client/client.c

all: $(BIN_DIR)/server $(BIN_DIR)/client

$(BIN_DIR):
	@mkdir -p $(BIN_DIR)

$(BIN_DIR)/server: $(SERVER_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_SERVER)

$(BIN_DIR)/client: $(CLIENT_SRC) | $(BIN_DIR)
	$(CC) $(CFLAGS) $< -o $@ $(LDFLAGS_CLIENT)

clean:
	rm -rf $(BIN_DIR) chat.db

run-server: $(BIN_DIR)/server
	$(BIN_DIR)/server $(PORT)

run-client: $(BIN_DIR)/client
	$(BIN_DIR)/client $(SERVER) $(PORT) $(USER)

.PHONY: all clean run-server run-client
