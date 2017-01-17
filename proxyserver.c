/*
 * proxyserver_server.c
 * Srikishen Shanmugam (srikishan@gmail.com)
 * All rights reserved.
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#ifdef TIMER_FD
#include <sys/timerfd.h>
#endif
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <linux/net.h>
#include <sys/epoll.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/prctl.h>
#include "proxyserver.h"

#define EPOLL_TIMEOUT 200
#define MAXEVENTS 100
#define MAX_SIZE  1000
#define PROXYSERVER_ADDRESS "127.0.0.1"
#define PROXYSERVER_PORT 1099
#define SERVER_IP "127.0.0.1"
#define SERVER_PORT 5000

typedef struct stats_ {
    int msg_total;
    int msg_req;
    int msg_ack;
    int msg_nak;
} stats;

uint32_t local_port = 0, fwd_port = 0;
int sec_index = 0;
stats g_sec[10], g_tot_stats, g_stats;
// lock btw stats update thead and main epoll thread
pthread_mutex_t stat_lock = PTHREAD_MUTEX_INITIALIZER;

/* Forward declarations... */
static void socket_event(struct proxyserver_server *server, int s, int event);

static int
event_op (int epfd, int fd, int op, int events) {
    struct epoll_event ee;
    ee.events = events;
    ee.data.fd = fd;
    return ERROR_CHECK(epoll_ctl(epfd, op, fd, &ee));
}

static struct proxyserver_socket *
alloc_sockets (int *max) 
{
    struct rlimit rl;
    struct proxyserver_socket *s;
    int i;

    FATAL_ERROR_CHECK(getrlimit(RLIMIT_NOFILE, &rl));
    FATAL_ERROR_CHECK(((s=calloc(rl.rlim_cur, sizeof(*s)))?0:-1));
    *max = rl.rlim_cur;
    for (i = 0; i < *max; i++) {
        s[i].s = i;
    }
    return s;
}

int
mkserversocket (uint32_t ip, in_port_t port)
{
    struct sockaddr_in sin;
    int s;
    int opt;

    memset(&sin, 0, sizeof(struct sockaddr_in));

    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(ip);
    sin.sin_family = AF_INET;

    s = FATAL_ERROR_CHECK(TEMP_FAILURE_RETRY(
                      socket(AF_INET, SOCK_STREAM/*|SOCK_NONBLOCK*/, 0)));
    FATAL_ERROR_CHECK(TEMP_FAILURE_RETRY(fcntl(s, F_SETFL, O_NONBLOCK)));
    opt = 1;
    ERROR_CHECK(setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                           (char*)&opt, sizeof(opt)));
    FATAL_ERROR_CHECK(TEMP_FAILURE_RETRY(
               bind(s, (struct sockaddr *)&sin,sizeof(struct sockaddr_in))));
    FATAL_ERROR_CHECK(TEMP_FAILURE_RETRY(listen(s, 100)));
    return s;
}


int
mkclientsocket_to_server (struct proxyserver_server *server,
                          int other_s, uint32_t ip, in_port_t port)
{
    struct sockaddr_in sin;
    int s, opt;

    memset(&sin, 0, sizeof(struct sockaddr_in));
    sin.sin_port = htons(port);
    sin.sin_addr.s_addr = htonl(ip);
    sin.sin_family = AF_INET;

    s = FATAL_ERROR_CHECK(TEMP_FAILURE_RETRY(socket(AF_INET, SOCK_STREAM, 0)));
    opt = 1;
    ERROR_CHECK(setsockopt(s, SOL_SOCKET, SO_REUSEADDR,
                           (char*)&opt, sizeof(opt)));
    if (ERROR_CHECK(TEMP_FAILURE_RETRY(
              connect(s, (struct sockaddr *)&sin, sizeof(sin)))) < 0) {
        close(s);
        return -1;
    }
    FATAL_ERROR_CHECK(TEMP_FAILURE_RETRY(fcntl(s, F_SETFL, O_NONBLOCK)));
    if (event_op(server->epfd, s, EPOLL_CTL_ADD, EPOLLIN) < 0) {
        close(s);
        printf("EPOLLIN failed after new conn %d", s);
        return -1;
    }
    server->sockets[s].state = CONNECTED;
    server->sockets[s].type = SERVER;
    //link each other
    server->sockets[s].client = other_s;
    server->sockets[other_s].server  = s;

    return s;
}

int 
new_connection (int ms) 
{
    struct sockaddr_un un;
    socklen_t unl = sizeof(un);
    int s = ERROR_CHECK(accept(ms, (struct sockaddr*)&un,
                              (socklen_t*)&unl));
    printf("Accepted new connection on fd %d",s);
    return s;
}   

/* Read and write wrappers... */
int
readall (int fd1, int fd2) 
{
    char buf[MAX_SIZE];
    int n1, n2;

    n1 = ERROR_CHECK(TEMP_FAILURE_RETRY(read(fd1,buf,MAX_SIZE)));
    if (n1 <= 0) {
        printf("read failed on fd %d, err %d, errno %s", fd1, n1, strerror(errno));
        return n1;
    } else {
        pthread_mutex_lock(&stat_lock);
        g_stats.msg_total++;
        g_tot_stats.msg_total++;
        if (strncmp(buf, "REQ", 3) == 0) {
            g_tot_stats.msg_req++;
            g_stats.msg_req++;
        } else if (strncmp(buf, "ACK", 3) == 0) {
            g_tot_stats.msg_ack++;
            g_stats.msg_ack++;
        } else if (strncmp(buf, "NAK", 3) == 0) {
            g_tot_stats.msg_nak++;
            g_stats.msg_nak++;
        } else {
            // continuation msg, so reduce the count
            g_tot_stats.msg_total--;
            g_stats.msg_total--;
        }
        pthread_mutex_unlock(&stat_lock);
        // write to the other socket
        if (fd2 != 0) {
            n2 = ERROR_CHECK(TEMP_FAILURE_RETRY(write(fd2,buf,n1)));
            if (n2 <= 0) {
                printf("write to other fd %d failed, err %d", fd2, n2);
                return -1;
            }
        } else {
            return n1;
        } 
    }

    return n2;
}

static int
read_message (struct proxyserver_server *server, int s) 
{
    int other_s;
 
    if (server->sockets[s].type == CLIENT) { 
        other_s = server->sockets[s].server;
        if (other_s == 0) {
            uint32_t ip_server = ntohl(inet_addr(SERVER_IP));
            // server died, re-connect
            other_s = mkclientsocket_to_server(server, s, ip_server, fwd_port);
            if (other_s < 0) {
                printf("new connection failed err %d", other_s);
                other_s = 0;
            } else {
                printf("Est conn to server at %d for client %d\n", other_s, s);
            }
            server->sockets[s].server = other_s;
        }
    } else {
        other_s = server->sockets[s].client;
    }

    
    if (readall(s, other_s) <= 0) {
        printf("readall for client %d failed", s);
        return -1;
    }

    return 0;
}

static void
socket_event (struct proxyserver_server *server, int s, int event) 
{
    int tmp_s;
    int doclose = 0;

    /* In case of error or hangup, close the descriptor */
    if (event & (EPOLLHUP|EPOLLERR)) {
        if (event & EPOLLERR) {
            int err;
            socklen_t len = sizeof(err);
            if (getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &len) < 0) {
                printf("Failed, getsockopt\n");
            }
        }
        /* Close the descriptor */
        printf("closing connection on fd %d for client\n",s);
        //printf("total %d, req %d, ack, %d, nak %d\n", g_tot_stats.msg_total, g_tot_stats.msg_req, g_tot_stats.msg_ack, g_tot_stats.msg_nak);
        event_op(server->epfd, s, EPOLL_CTL_DEL, 0);
        close(s);

        if (server->sockets[s].type == CLIENT) { 
            tmp_s = server->sockets[s].server;
            if (tmp_s) {
                server->sockets[tmp_s].client = 0;
                event_op(server->epfd, tmp_s, EPOLL_CTL_DEL, 0);
                close(tmp_s);
                server->sockets[tmp_s].state = NOTINIT;
            }
        } else {
            tmp_s = server->sockets[s].client;
            if (tmp_s) {
                server->sockets[tmp_s].server = 0;
            }
        }
        server->sockets[s].state = NOTINIT;
        return;
    }

    if (event & EPOLLIN) {
        /* TODO: read in non-blocking mode */
        if (read_message(server, s) < 0) {
            doclose = 1;
        }
    }

    if (doclose) {
        socket_event(server, s, EPOLLHUP);
    }
}

void
display_stats (int sig)
{
    int i, j, k;
    stats copy[10];
    int reqt = 0, ackt = 0, nakt = 0;

    // get the prev 1sec window
    k = (sec_index - 1) == 0 ? 9 : (sec_index - 1); 
    printf("Total msg - %d\n", g_tot_stats.msg_total);
    printf("REQ - %d, ACK - %d, NAK - %d \n", g_tot_stats.msg_req, g_tot_stats.msg_ack, g_tot_stats.msg_nak);
    printf("Avg rate in 1sec\n");
    printf("Req rate - %d, resp rate - %d\n", g_sec[k].msg_req, g_sec[k].msg_ack+g_sec[k].msg_nak); 
    // if curr index is 8(sec_index), the last 10 windows are 7,6..0,9,8
    // which is same as 8,9,0...7
    for (i=0, j=sec_index; i<10; i++) {
        //printf("index %d, req %d, ack %d, nak %d\n", j, g_sec[j].msg_req, g_sec[j].msg_ack, g_sec[j].msg_nak);
        // take a snapshot
        copy[i] = g_sec[j];
        j = (j+1)%10;
    }
 
    // total and compute avg
    for (i=0; i<10; i++) {
        reqt += copy[i].msg_req;
        ackt += copy[i].msg_ack;
        nakt += copy[i].msg_nak;
    }
    printf("Avg rate in 10 sec\n");
    printf("REQ rate - %f, resp rate - %f\n", ((float)reqt/10), (((float)ackt+(float)nakt)/10));
}

// collect stats is spawned as a separate thread with sleep impl
// bcos couldnt find <sys/timerfd.h> in my IDE. If that worked
// do not need a separate thread as the periodic timer fd expiry
// could have been handled in the epoll thread itself. In that
// case no do not read the stat_lock either
void*
collect_stats (void *ctx)
{
    while (1) {
        // can replace sleep with msec if more finer granularity of stats 
        sleep(1);
        pthread_mutex_lock(&stat_lock);
        // simple circular buffer impl of rolling stats
        g_sec[sec_index] = g_stats;
        memset(&g_stats, 0, sizeof(g_stats));
        sec_index = (sec_index+1) % 10;
        pthread_mutex_unlock(&stat_lock);
    }
}

int
main (int argc, char *argv[])
{
    int s, s1;
    int i, nev;
    struct epoll_event ev[MAXEVENTS];
    struct proxyserver_server server;
    uint32_t ip_local, ip_server;
    //uint64_t exp, tot_exp = 0;
    //struct itimerspec new_value;
    //int tfd;
    int opt;
    pthread_t tid;

    while ((opt = getopt(argc, argv, "l:f:")) != -1) {
        switch (opt) {
        case 'l':
            local_port = atoi(optarg); 
            break;
        case 'f':
            fwd_port = atoi(optarg);
            break;
        default:
            printf("Usage: proxy -l <local port> -f <forward port> \n");
            exit(0);
        }
    }

    if (optind != argc) {
        printf("Wrong args, Usage: proxy -l <local port> -f <forward port> \n");
        exit(EXIT_FAILURE);
    }

    // assign default local, fwd port if not passed in by caller
    if (local_port == 0) {
        local_port = PROXYSERVER_PORT;
    }
    if (fwd_port == 0) {
        fwd_port = SERVER_PORT;
    }
    printf("Using local port %d, fwd port %d\n", local_port, fwd_port);
    memset(&server, 0, sizeof(server));
    ip_local = ntohl(inet_addr(PROXYSERVER_ADDRESS));
    ip_server = ntohl(inet_addr(SERVER_IP));

    signal(SIGUSR1, display_stats);
    server.sockets = alloc_sockets(&server.maxsockets);
    printf("Server bind on Ip addr 0x%x, port %x, "
                            "max sockets %d", 
                            ip_local, local_port, server.maxsockets);
    server.epfd = epoll_create(10);
    server.ms = mkserversocket(ip_local, local_port);
    event_op(server.epfd, server.ms,  EPOLL_CTL_ADD, EPOLLIN);

    if (pthread_create(&tid, NULL, collect_stats, (void*)0)) {
        printf("thread create failed\n");
    }

#if 0
    if ((tfd= timerfd_create(CLOCK_MONOTONIC,0)) < 0) {
        printf("Timer create error\n");
        return -1;
    }
    FATAL_ERROR_CHECK(TEMP_FAILURE_RETRY(fcntl(tfd, F_SETFL, O_NONBLOCK)));

    new_value.it_interval.tv_sec = 1;
    new_value.it_interval.tv_nsec = 0;
    new_value.it_value.tv_sec = 1;
    new_value.it_value.tv_nsec = 0;

    if (timerfd_settime(tfd, 0, &new_value, NULL) < 0) {
        printf("timerfd_settime error\n");
    }
    event_op(server.epfd, tfd,  EPOLL_CTL_ADD, EPOLLIN);
#endif
    while (1) {
        nev = epoll_wait(server.epfd, ev, MAXEVENTS, EPOLL_TIMEOUT);
        if (nev < 0) {
            continue; 
        }
        for (i = 0; i < nev; i++) {
            if (ev[i].data.fd == server.ms) {
                printf("Accept a new connection");   
                /*
                 * Accept a new connection and set it up for read events 
                 * in epoll 
                 */
                s = new_connection(server.ms);
                if (s < 0) {
                    printf("new connection failed err %d", s);
                    continue;
                }
                if (event_op(server.epfd, s, EPOLL_CTL_ADD, EPOLLIN) < 0) {
                    close(s);
                    printf("EPOLLIN failed after new conn %d", s);
                    continue;
                }
                server.sockets[s].state = CONNECTED;
                server.sockets[s].type = CLIENT;
                // establish the connection to actual server
                s1 = mkclientsocket_to_server(&server, s, ip_server, fwd_port);
                if (s1 < 0) {
                    printf("new connection failed err %d", s1);
                    continue;
                } else {
                    printf("Est conn to server at %d for client %d\n", s1, s);
                }
            } /*else if (ev[i].data.fd == tfd) {
                s = read(tfd, &exp, sizeof(uint64_t));
                tot_exp += exp;
                printf("Exp %llu , total %llu\n", exp, tot_exp);
            }*/ else {
                if (ev[i].data.fd >= server.maxsockets) {
                    printf("fd %d > maxsockets %d", ev[i].data.fd, 
                                               server.maxsockets);
                    abort();
                }
                socket_event(&server, ev[i].data.fd, ev[i].events);
            }
        }
    }

    return 0;
}


