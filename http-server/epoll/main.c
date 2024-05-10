#include <stdio.h>
#include <stdbool.h>
#include <netinet/in.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/utsname.h>
#include <sys/epoll.h>
#include "picohttpparser.h"

#define SERVER_STRING           "Server: zerohttpd/0.1\r\n"
#define DEFAULT_SERVER_PORT     8000
#define QUEUE_DEPTH             256
#define BUF_SZ                  8192

#define EVENT_TYPE_ACCEPT       0
#define EVENT_TYPE_READ         1
#define EVENT_TYPE_WRITE        2

#define MIN_KERNEL_VERSION      5
#define MIN_MAJOR_VERSION       5

#define MAX_SQE_PER_LOOP        5

static const char* response =
            "HTTP/1.0 200 OK\r\n"
            "Server: Assdi2024Server/1.0\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 98\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<!DOCTYPE html><head><title>Hello, World!</title></head><body><h1>Hello, World!</h1></body></html>";

static const char* bad_request =
            "HTTP/1.0 400 Bad Request\r\n"
            "Server: Assdi2024Server/1.0\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 96\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<!DOCTYPE html><head><title>Bad Request!</title></head><body><h1>Bad Request!</h1></body></html>";

struct conn {
    int epoll_fd;
    int sock;
    bool shutdown;
    /* recv buf */
    int buflen, prevbuflen;
    char reqbuf[BUF_SZ];
    /* send buf */
    int sendbuf_sz;
    const char *sendbuf;
};

static void shutdown_conn(struct conn *conn)
{
    conn->shutdown = true;
    epoll_ctl(conn->epoll_fd, EPOLL_CTL_MOD, conn->sock, &(struct epoll_event){.events = EPOLLOUT, .data.ptr = conn});    
}

static void close_conn(struct conn *conn)
{
    close(conn->sock);
    free(conn);
}

static void send_response(struct conn *conn, const char *response)
{
    conn->sendbuf = response;
    conn->sendbuf_sz = strlen(response);
    conn->prevbuflen = 0;
    epoll_ctl(conn->epoll_fd, EPOLL_CTL_MOD, conn->sock, &(struct epoll_event){.events = EPOLLOUT, .data.ptr = conn});
}

static void send_bad_request(struct conn *conn)
{
    send_response(conn, bad_request);
    shutdown_conn(conn);
}

static void accept_connetion(int epoll_fd, int sock)
{
    int fd = accept(sock, NULL, NULL);
    if (fd < 0) {
        perror("accept");
    }

    struct conn *conn = calloc(1, sizeof(*conn));
    conn->epoll_fd = epoll_fd;
    conn->sock = fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &(struct epoll_event){.events = EPOLLIN, .data.ptr = conn});
}

static void attempt_recv(struct conn *conn)
{
    int ret = recv(conn->sock, conn->reqbuf + conn->buflen, BUF_SZ - conn->buflen, 0);
    if (ret <= 0) {
        close_conn(conn);
        return;
    }

    conn->prevbuflen = conn->buflen;
    conn->buflen += ret;
    int pret, minor_version;
    const char *method, *path;
    struct phr_header headers[50];
    size_t method_len, path_len, num_headers = 50;
    pret = phr_parse_request(conn->reqbuf, conn->buflen, &method, &method_len, &path, &path_len,
                             &minor_version, headers, &num_headers, conn->prevbuflen);

    if (pret == -2) {
        if (conn->buflen == BUF_SZ)
            send_bad_request(conn);
        return;
    } else if (pret == -1) {
        send_bad_request(conn);
        return;
    }

    if (method_len != 3 || memcmp(method, "GET", 3) != 0) {
        send_bad_request(conn);
        return;
    }

    memmove(conn->reqbuf, conn->reqbuf + pret, conn->buflen - pret);
    conn->buflen -= pret;
    conn->prevbuflen = 0;

    conn->shutdown = true;
    send_response(conn, response);
}

static void attempt_send(struct conn *conn)
{
    if (conn->sendbuf_sz == 0)
        goto end;
    int ret = send(conn->sock, conn->sendbuf, conn->sendbuf_sz, 0);
    if (ret <= 0) {
        close_conn(conn);
    }
end:
    if (conn->shutdown) {
        close_conn(conn);
    } else {
        epoll_ctl(conn->epoll_fd, EPOLL_CTL_MOD, conn->sock,
                &(struct epoll_event){.events = EPOLLIN, .data.ptr = conn});
    }
}

void server_loop(int sock)
{
    int epoll_fd = epoll_create1(0);
    struct epoll_event ev[QUEUE_DEPTH];

    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, sock, &(struct epoll_event){.events = EPOLLIN, .data.fd = sock});

    while (1) {
        int ret = epoll_wait(epoll_fd, ev, QUEUE_DEPTH, -1);
        if (ret < 0) {
            perror("epoll_wait");
            exit(1);
        }
        for (int i = 0; i < ret; i++) {
            int fd = ev[i].data.fd;
            if (fd == sock) {
                accept_connetion(epoll_fd, sock);
            } else if (ev[i].events & EPOLLIN) {
                attempt_recv((struct conn *)ev[i].data.ptr);
            } else if (ev[i].events & EPOLLOUT) {
                attempt_send((struct conn *)ev[i].data.ptr);
            }
        }
    }
}

static int setup_listening_socket(int port)
{
    int listen_sock = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_sock < 0) {
        perror("socket");
        exit(1);
    }

    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &(int){1}, sizeof(int));

    struct sockaddr_in server_addr;
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listen_sock, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("bind");
        exit(1);
    }

    if (listen(listen_sock, 128) < 0) {
        perror("listen");
        exit(1);
    }

    return listen_sock;
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_SERVER_PORT;
    if (argc > 1)
        port = atoi(argv[1]);

    int sock = setup_listening_socket(port);

    printf("Listening on port %d\n", port);
    server_loop(sock);
    fprintf(stderr, "server exiting\n");
    return 0;
}
