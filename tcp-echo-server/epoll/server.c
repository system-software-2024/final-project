#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <arpa/inet.h>
#include "server.h"

int main(int argc, char *argv[]) 
{
    if (argc < 2) {
        printf("./server [port]\n");
        return 0;
    }

    uint32_t port = (uint32_t)strtol(argv[1], NULL, 10);
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    char buf[MAX_MESSAGE_LEN];
    memset(buf, 0, sizeof(buf));

    int listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_fd < 0) {
        perror("Socket()");
        return 1;
    }
    const int val = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &val, sizeof(val));
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = INADDR_ANY;
    
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind()");
        return 1;
    }

    if (listen(listen_fd, BACK_LOG) < 0) {
        perror("Listen()");
        return 1;
    }

    printf("server listening on port %u\n", port);

    struct epoll_event ev, events[MAX_EVENTS];
    int new_events, conn_fd, epoll_fd;
    epoll_fd = epoll_create(MAX_EVENTS);
    if (epoll_fd < 0) {
        perror("Epoll()");
        return 1;
    }
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;

    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev) == -1) {
        fprintf(stderr, "Error adding new listening socket to epoll\n");
        return 1;
    }

    while (true) {
        new_events = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
        if (new_events == -1) {
            perror("Epoll_wait()");
            return 1;
        }
        for (int i = 0; i < new_events; i++) {
            if (events[i].data.fd == listen_fd) {
                conn_fd = accept4(listen_fd, (struct sockaddr *)&client_addr, &client_len, SOCK_NONBLOCK);
                if (conn_fd == -1) {
                    fprintf(stderr, "Error accepting new connection\n");
                    return 1;
                }
                ev.events = EPOLLIN | EPOLLET;
                ev.data.fd = conn_fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, conn_fd, &ev) == -1) {
                    fprintf(stderr, "Error adding new event to epoll\n");
                    return 1;
                }
            }else {
                int new_sock_fd = events[i].data.fd;
                int recv_sz = recv(new_sock_fd, buf, MAX_MESSAGE_LEN, 0);
                if (recv_sz <= 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, new_sock_fd, NULL);
                    shutdown(new_sock_fd, SHUT_RDWR);
                }else {
                    send(new_sock_fd, buf, recv_sz, 0);
                }
            }
        }
    }

    return 0;

}