/*
 * proxyserver_server.h
 * All rights reserved.
 */

#ifndef _PROXYSERVER_SERVER_H
#define _PROXYSERVER_SERVER_H

struct proxyserver_socket {
    /* Socket */
    int s;
    enum { NOTINIT = 0, CONNECTED = 1 } state;
    enum { CLIENT = 0, SERVER = 1 } type;
    int client, server;
};

struct proxyserver_server {
    /* Sockets, indexed by fd. */
    struct proxyserver_socket *sockets;
    int maxsockets;

    /* epoll descriptor */
    int epfd;

    /* Master socket */
    int ms;
};

void* runserver(void *ctx);

#define FATAL_ERROR_CHECK(x) ({ \
    int _retval = (x); \
    if (_retval < 0) { \
        printf("Fatal error at %s:%d: %s: %s\n", __FILE__, __LINE__, #x, strerror(errno)); \
        exit(1); \
    } \
    _retval; \
})


#define ERROR_CHECK(x) ({ \
    int _retval = (x); \
    if (_retval < 0) { \
        printf("Error at %s:%d: %s: %s\n", __FILE__, __LINE__, #x, strerror(errno)); \
    } \
    _retval; \
})

#define TEMP_FAILURE_RETRY(expression) \
  (__extension__                                                              \
    ({ long int __result;                                                     \
       do __result = (long int) (expression);                                 \
       while (__result == -1L && errno == EINTR);                             \
       __result; }))

#endif /* _PROXYSERVER_SERVER_H */

