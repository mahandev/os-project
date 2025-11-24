#ifndef STORAGE_H
#define STORAGE_H

#include <stdbool.h>

typedef void (*history_callback)(const char *timestamp, const char *sender, const char *body, void *ctx);

int storage_init(const char *path);
void storage_shutdown(void);
int storage_store_message(const char *sender, const char *receiver, const char *body);
int storage_fetch_conversation(const char *user_a, const char *user_b, history_callback cb, void *ctx);
int storage_delete_conversation(const char *user_a, const char *user_b);
const char *storage_last_error(void);

#endif /* STORAGE_H */
