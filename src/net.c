/*
* cityslicker by Joakim Hamren
* 
* To the extent possible under law, the person who associated CC0 with
* cityslicker has waived all copyright and related or neighboring rights
* to cityslicker.
*
* You should have received a copy of the CC0 legalcode along with this
* work. If not, see <http://creativecommons.org/publicdomain/zero/1.0/>.
*/

#include "net.h"
#include "cs.h"
#include "util.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

#define NET_SERVER_BACKLOG 50
#define NET_EPOLL_HINT 1024

static void net_file_event_destroy(net_file_event *fe) {
    free(fe);
}

static net_file_event *net_file_event_create(void * ptr, net_file_event_type_t type) {
    net_file_event *fe = malloc(sizeof *fe);
    if(!fe) return NULL;
    fe->ptr = ptr;
    fe->type = type;
    fe->mask = 0;
    return fe;
}

static net_client *net_client_create(int fd, struct sockaddr_in addr) {
    net_client *c = malloc(sizeof *c);
    if(!c) return NULL;

    c->fd = fd;
    strcpy(c->ip, inet_ntoa(addr.sin_addr));
    c->port = ntohs(addr.sin_port);
    return c;
}

static void net_client_close(net_server *s, net_client *c) {
    log_info("Client %s:%d disconnected.\n", c->ip, c->port);
    epoll_ctl(s->epfd, EPOLL_CTL_DEL, c->fd, NULL);
    close(c->fd);
    free(c);
}

static net_client *net_server_accept(net_server *s) {
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(struct sockaddr_in);
    int fd;
    net_client *c = NULL;
    net_file_event *fe = NULL;
    
    if((fd = accept(s->fd, (struct sockaddr *) &addr, &addrlen)) == -1)
        goto err;

    if(fcntl(fd, F_SETFL, O_NONBLOCK) != 0)
        goto err;

    if((c = net_client_create(fd, addr)) == NULL)
        goto err;

    if((fe = net_file_event_create((void *) c, net_file_event_type_client)) == NULL)
        goto err;

    struct epoll_event ee;
    memset(&ee, 0, sizeof(struct epoll_event));
    ee.events |= EPOLLIN;
    ee.events |= EPOLLOUT;
    ee.data.u64 = 0;
    ee.data.ptr = (void *) fe;

    if(epoll_ctl(s->epfd, EPOLL_CTL_ADD, fd, &ee) != 0)
        goto err;

    return c;

err:
    if(c) 
        net_client_close(s, c);
    else if(fd)
        close(fd);

    if(fe) 
        net_file_event_destroy(fe);

    return NULL;
}

static int net_client_write(net_client *c) {
    if(c->buflen) {
        int n = write(c->fd, c->buf, c->buflen);
        if(n == -1)
            return -1;
        c->buflen = 0;
        return n;
    }

    return 0;
}

static int net_client_read(net_client *c) {
    int num_read;
    int datalen = 16;
    char data[datalen];
    num_read = read(c->fd, data, datalen);
    if(num_read == 0) {
        return 0;
    }

    if(num_read != datalen) {
        log_info("Got some strange input, need 16 bytes!\n");
        return -1;
    }

    float *coords = (float *) &data;
    world *res = world_get_cities_in_bounding_box(
        loaded_world, 
        (double) coords[0], (double) coords[1], (double) coords[2], (double) coords[3]
    );

    int i;
    int *ids = (int *) &c->buf;
    for(i = 0; i < res->length; i++) {
        *(ids++) = res->cities[i]->id;
    }

    c->buflen = sizeof(int) * res->length;

    return num_read;
}

static net_server *net_server_create(void) {
    net_server *s = malloc(sizeof *s);
    if(!s) return NULL;
    s->events = malloc(sizeof(struct epoll_event) * NET_MAX_EVENTS);
    if(!s->events) {
        free(s);
        return NULL;
    }
    s->max_events = NET_MAX_EVENTS;
    return s;
}

static void net_server_destroy(net_server *s) {
    if(!s) return;
    free(s->events);
    if(s->fd) close(s->fd);
    if(s->epfd) close(s->epfd);
    free(s);
}

net_server *net_server_start(int port) {
    int optval = 1, res;
    net_file_event *fe = NULL;

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(struct sockaddr_in));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);

    signal(SIGPIPE, SIG_IGN);

    net_server *s = net_server_create();
    if(!s) goto err;

    s->fd = socket(AF_INET, SOCK_STREAM, 0);
    if(s->fd == -1) goto err;

    setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));
    res = bind(s->fd, (const struct sockaddr *) &addr, sizeof(struct sockaddr_in));
    if(res != 0) goto err;

    res = listen(s->fd, NET_SERVER_BACKLOG);
    if(res != 0) goto err;

    // create epoll instance and add the server fd
    int epfd = epoll_create(NET_EPOLL_HINT);
    if(epfd == -1) goto err;

    s->epfd = epfd;

    fe = net_file_event_create((void *) s, net_file_event_type_server);
    if(!fe) goto err;

    struct epoll_event ee;
    memset(&ee, 0, sizeof(struct epoll_event));
    ee.events |= EPOLLIN;
    ee.data.u64 = 0;
    ee.data.ptr = (void *) fe;

    res = epoll_ctl(epfd, EPOLL_CTL_ADD, s->fd, &ee);
    if(res == -1) goto err;

    return s;

err:
    log_error("Failed to start server: %s\n", strerror(errno));
    if(s) net_server_destroy(s);
    if(fe) net_file_event_destroy(fe);
    return NULL;
}

int net_poll(net_server *s) {
    int num_events = epoll_wait(s->epfd, s->events, s->max_events, -1);
    if(num_events == -1 && errno != EINTR) {
        log_fatal("An error occured while waiting for fd events: %s.", strerror(errno));
    }
    int i, num_read;
    for(i = 0; i < num_events; i++) {
        net_file_event *fe = (net_file_event *) s->events[i].data.ptr;

        if(fe->type == net_file_event_type_client) {
            if(s->events[i].events & EPOLLIN) {
                if((num_read = net_client_read((net_client *)fe->ptr)) <= 0) {
                    if(num_read == -1)
                        log_error("Failed to read data from client. Closing client.\n");
                    net_client_close(s, (net_client *)fe->ptr);
                    net_file_event_destroy(fe);
                    continue;
                }
            }
            if (s->events[i].events & EPOLLOUT) {
                if(net_client_write((net_client *)fe->ptr) == -1) {
                    log_error("Failed to write data from client. Closing client.\n");
                    net_client_close(s, (net_client *)fe->ptr);
                    net_file_event_destroy(fe);
                    continue;
                }
            }
            if (s->events[i].events & EPOLLHUP || s->events[i].events & EPOLLERR) {
                net_client_close(s, (net_client *)fe->ptr);
                net_file_event_destroy(fe);
            }
        }
        else if(fe->type == net_file_event_type_server && s->events[i].events & EPOLLIN) {
            net_client *c = net_server_accept((net_server *)fe->ptr);
            if(!c) {
                log_info("Failed to accept client connection: %s\n", strerror(errno));
            } else {
                log_info("Client %s:%d connected\n", c->ip, c->port);
            }
        }
    }
    return num_events;
}
