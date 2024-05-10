#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <arpa/inet.h>
#include <signal.h>
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
    pid_t pid;

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

    signal(SIGCHLD, SIG_IGN);
    
    printf("server listening on port %u\n", port);
    while (true) {
        int conn_fd = accept(listen_fd, (struct sockaddr *)&client_addr, &client_len);
        if (conn_fd == -1) {
            fprintf(stderr, "Error accepting new connection\n");
            return 1;
        }
        if ((pid = fork()) == 0) {
            close(listen_fd);
            while (true) {
                int recv_sz = recv(conn_fd, buf, MAX_MESSAGE_LEN, 0);
                if (recv_sz < 0)
                    break;
                send(conn_fd, buf, recv_sz, 0);
                if (recv_sz == 0)
                    break;
                bzero(buf, sizeof(buf));
            }
            close(conn_fd);
            return 0;
        }
    }

    close(listen_fd);
    return 0;
}
