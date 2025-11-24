#include "storage.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef STORAGE_USE_SQLITE
#define STORAGE_USE_SQLITE 1
#endif

#define MAX_ERROR_LEN 256

static char last_error[MAX_ERROR_LEN];
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t error_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_error(const char *fmt, const char *detail) {
    pthread_mutex_lock(&error_lock);
    snprintf(last_error, sizeof(last_error), fmt, detail ? detail : "");
    pthread_mutex_unlock(&error_lock);
}

const char *storage_last_error(void) {
    pthread_mutex_lock(&error_lock);
    const char *msg = (*last_error) ? last_error : "unknown error";
    pthread_mutex_unlock(&error_lock);
    return msg;
}

#if STORAGE_USE_SQLITE
#include <sqlite3.h>

static sqlite3 *db_handle = NULL;

int storage_init(const char *path) {
    if (sqlite3_open(path, &db_handle) != SQLITE_OK) {
        set_error("Failed to open database: %s", sqlite3_errmsg(db_handle));
        return -1;
    }
    const char *sql =
        "CREATE TABLE IF NOT EXISTS messages ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "sender TEXT NOT NULL,"
        "receiver TEXT NOT NULL,"
        "body TEXT NOT NULL,"
        "created_at DATETIME DEFAULT CURRENT_TIMESTAMP" ");";
    char *err = NULL;
    if (sqlite3_exec(db_handle, sql, NULL, NULL, &err) != SQLITE_OK) {
        set_error("Failed to create schema: %s", err);
        sqlite3_free(err);
        return -1;
    }
    return 0;
}

void storage_shutdown(void) {
    if (db_handle) {
        sqlite3_close(db_handle);
        db_handle = NULL;
    }
}

int storage_store_message(const char *sender, const char *receiver, const char *body) {
    pthread_mutex_lock(&storage_lock);
    const char *sql = "INSERT INTO messages (sender, receiver, body) VALUES (?, ?, ?);";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db_handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&storage_lock);
        set_error("Failed to prepare insert: %s", sqlite3_errmsg(db_handle));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, sender, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, receiver, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, body, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&storage_lock);
    if (rc != SQLITE_DONE) {
        set_error("Failed to store message: %s", sqlite3_errmsg(db_handle));
        return -1;
    }
    return 0;
}

int storage_fetch_conversation(const char *user_a, const char *user_b, history_callback cb, void *ctx) {
    pthread_mutex_lock(&storage_lock);
    const char *sql =
        "SELECT datetime(created_at), sender, body FROM messages "
        "WHERE (sender=? AND receiver=?) OR (sender=? AND receiver=?) "
        "ORDER BY created_at ASC";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db_handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&storage_lock);
        set_error("Failed to query history: %s", sqlite3_errmsg(db_handle));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, user_a, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user_b, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, user_b, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, user_a, -1, SQLITE_TRANSIENT);

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *timestamp = (const char *)sqlite3_column_text(stmt, 0);
        const char *sender = (const char *)sqlite3_column_text(stmt, 1);
        const char *body = (const char *)sqlite3_column_text(stmt, 2);
        cb(timestamp ? timestamp : "", sender ? sender : "", body ? body : "", ctx);
    }
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&storage_lock);
    return 0;
}

int storage_delete_conversation(const char *user_a, const char *user_b) {
    pthread_mutex_lock(&storage_lock);
    const char *sql =
        "DELETE FROM messages WHERE (sender=? AND receiver=?) OR (sender=? AND receiver=?)";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db_handle, sql, -1, &stmt, NULL) != SQLITE_OK) {
        pthread_mutex_unlock(&storage_lock);
        set_error("Failed to prepare delete: %s", sqlite3_errmsg(db_handle));
        return -1;
    }
    sqlite3_bind_text(stmt, 1, user_a, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, user_b, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, user_b, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 4, user_a, -1, SQLITE_TRANSIENT);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    pthread_mutex_unlock(&storage_lock);
    if (rc != SQLITE_DONE) {
        set_error("Failed to delete history: %s", sqlite3_errmsg(db_handle));
        return -1;
    }
    return 0;
}

#else

#include <errno.h>
#include <limits.h>
#ifdef _WIN32
#define strdup _strdup
#endif

static char storage_path[PATH_MAX];

static void ensure_path_buffer(void) {
    if (storage_path[0] == '\0') {
        strncpy(storage_path, "chat.log", sizeof(storage_path) - 1);
    }
}

static int portable_getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream) {
        return -1;
    }
    if (*lineptr == NULL || *n == 0) {
        *n = 256;
        *lineptr = malloc(*n);
        if (!*lineptr) {
            return -1;
        }
    }
    size_t total = 0;
    while (fgets(*lineptr + total, (int)(*n - total), stream)) {
        size_t chunk = strlen(*lineptr + total);
        total += chunk;
        if (total > 0 && (*lineptr)[total - 1] == '\n') {
            return (int)total;
        }
        size_t new_size = (*n) * 2;
        char *tmp = realloc(*lineptr, new_size);
        if (!tmp) {
            return -1;
        }
        *lineptr = tmp;
        *n = new_size;
    }
    return (total > 0) ? (int)total : -1;
}

static void format_timestamp(char *buffer, size_t len) {
    time_t now = time(NULL);
#ifdef _WIN32
    struct tm tm_now;
    localtime_s(&tm_now, &now);
    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", &tm_now);
#else
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(buffer, len, "%Y-%m-%d %H:%M:%S", &tm_now);
#endif
}

int storage_init(const char *path) {
    ensure_path_buffer();
    if (path) {
        strncpy(storage_path, path, sizeof(storage_path) - 1);
    }
    FILE *file = fopen(storage_path, "a");
    if (!file) {
        set_error("Failed to open log file: %s", strerror(errno));
        return -1;
    }
    fclose(file);
    return 0;
}

void storage_shutdown(void) {}

int storage_store_message(const char *sender, const char *receiver, const char *body) {
    pthread_mutex_lock(&storage_lock);
    FILE *file = fopen(storage_path, "a");
    if (!file) {
        pthread_mutex_unlock(&storage_lock);
        set_error("Failed to append to log: %s", strerror(errno));
        return -1;
    }
    char ts[32];
    format_timestamp(ts, sizeof(ts));
    fprintf(file, "%s|%s|%s|%s\n", ts, sender, receiver, body);
    fclose(file);
    pthread_mutex_unlock(&storage_lock);
    return 0;
}

int storage_fetch_conversation(const char *user_a, const char *user_b, history_callback cb, void *ctx) {
    pthread_mutex_lock(&storage_lock);
    FILE *file = fopen(storage_path, "r");
    if (!file) {
        pthread_mutex_unlock(&storage_lock);
        set_error("Failed to read log: %s", strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t len = 0;
    while (portable_getline(&line, &len, file) != -1) {
        char *ts = strtok(line, "|");
        char *sender = strtok(NULL, "|");
        char *receiver = strtok(NULL, "|");
        char *body = strtok(NULL, "\n");
        if (!ts || !sender || !receiver || !body) {
            continue;
        }
        bool match =
            (strcmp(sender, user_a) == 0 && strcmp(receiver, user_b) == 0) ||
            (strcmp(sender, user_b) == 0 && strcmp(receiver, user_a) == 0);
        if (match) {
            cb(ts, sender, body, ctx);
        }
    }
    free(line);
    fclose(file);
    pthread_mutex_unlock(&storage_lock);
    return 0;
}

int storage_delete_conversation(const char *user_a, const char *user_b) {
    pthread_mutex_lock(&storage_lock);
    enum { TMP_EXTRA = 8 };
    char tmp_path[PATH_MAX + TMP_EXTRA];
    size_t base_len = strnlen(storage_path, sizeof(storage_path));
    if (base_len + 4 >= sizeof(tmp_path)) {
        pthread_mutex_unlock(&storage_lock);
        set_error("Log path too long for temp file", "");
        return -1;
    }
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", storage_path);
    FILE *src = fopen(storage_path, "r");
    if (!src) {
        pthread_mutex_unlock(&storage_lock);
        set_error("Failed to read log: %s", strerror(errno));
        return -1;
    }
    FILE *dst = fopen(tmp_path, "w");
    if (!dst) {
        fclose(src);
        pthread_mutex_unlock(&storage_lock);
        set_error("Failed to open temp log: %s", strerror(errno));
        return -1;
    }
    char *line = NULL;
    size_t len = 0;
    while (portable_getline(&line, &len, src) != -1) {
        char *copy = strdup(line);
        if (!copy) {
            continue;
        }
        char *sender = strtok(copy, "|"); // timestamp
        sender = strtok(NULL, "|");
        char *receiver = strtok(NULL, "|");
        bool match = sender && receiver &&
            ((strcmp(sender, user_a) == 0 && strcmp(receiver, user_b) == 0) ||
             (strcmp(sender, user_b) == 0 && strcmp(receiver, user_a) == 0));
        free(copy);
        if (!match) {
            fputs(line, dst);
        }
    }
    free(line);
    fclose(src);
    fclose(dst);
#ifdef _WIN32
    remove(storage_path);
#endif
    if (rename(tmp_path, storage_path) != 0) {
        pthread_mutex_unlock(&storage_lock);
        set_error("Failed to replace log file: %s", strerror(errno));
        return -1;
    }
    pthread_mutex_unlock(&storage_lock);
    return 0;
}

#endif
