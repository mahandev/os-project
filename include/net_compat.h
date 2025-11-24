#ifndef NET_COMPAT_H
#define NET_COMPAT_H

#include <errno.h>
#include <stdbool.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
typedef SOCKET socket_handle_t;
#define NET_INVALID_SOCKET INVALID_SOCKET
#define NET_SOCKET_ERROR SOCKET_ERROR
#ifndef SHUT_RDWR
#define SHUT_RDWR SD_BOTH
#endif
static inline int net_init(void) {
    WSADATA data;
    return WSAStartup(MAKEWORD(2, 2), &data);
}
static inline void net_cleanup(void) {
    WSACleanup();
}
static inline int net_close(socket_handle_t sock) {
    return closesocket(sock);
}
static inline bool net_was_interrupted(void) {
    int err = WSAGetLastError();
    return err == WSAEINTR || err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
typedef int socket_handle_t;
#define NET_INVALID_SOCKET -1
#define NET_SOCKET_ERROR -1
static inline int net_init(void) {
    return 0;
}
static inline void net_cleanup(void) {}
static inline int net_close(socket_handle_t sock) {
    return close(sock);
}
static inline bool net_was_interrupted(void) {
    return errno == EINTR;
}
#endif

#endif /* NET_COMPAT_H */
