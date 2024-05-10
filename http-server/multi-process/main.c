#include <stdio.h>
#include <netinet/in.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <stdlib.h>
#include <signal.h>
#include <liburing.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/utsname.h>
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
            "HTTP/1.1 200 OK\r\n"
            "Server: Assdi2024Server/1.0\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 98\r\n"
            "\r\n"
            "<!DOCTYPE html><head><title>Hello, World!</title></head><body><h1>Hello, World!</h1></body></html>";

static const char* bad_request =
            "HTTP/1.1 400 Bad Request\r\n"
            "Server: Assdi2024Server/1.0\r\n"
            "Content-Type: text/html\r\n"
            "Content-Length: 96\r\n"
            "Connection: close\r\n"
            "\r\n"
            "<!DOCTYPE html><head><title>Bad Request!</title></head><body><h1>Bad Request!</h1></body></html>";

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

static void send_bad_request(int sock)
{
    const char *buf = bad_request;
    size_t buflen = strlen(buf);

    while (buflen > 0) {
        ssize_t sret = send(sock, buf, buflen, 0);
        if (sret < 0)
            return;
        buf += sret;
        buflen -= sret;
    }
}

static void send_response(int sock)
{
    const char *buf = response;
    size_t buflen = strlen(buf);

    while (buflen > 0) {
        ssize_t sret = send(sock, buf, buflen, 0);
        if (sret < 0)
            return;
        buf += sret;
        buflen -= sret;
    }
}

static void handle_client(int sock)
{
    char buf[BUF_SZ];
    size_t buflen = 0, prevbuflen;

    while (1) {
        /* Receive Request */
        ssize_t rret = recv(sock, buf + buflen, BUF_SZ - buflen, 0);

        if (rret < 0 || (rret == 0 && buflen == 0))
            break;

        prevbuflen = buflen;
        buflen += rret;

        struct phr_header headers[50];
        const char *method, *path;
        size_t method_len, path_len, num_headers = 50;
        int minor_version;

        int pret = phr_parse_request(buf, buflen, &method, &method_len, &path, &path_len,
                                     &minor_version, headers, &num_headers, prevbuflen);
        if (pret == -1) {
            /* parse error */
            send_bad_request(sock);
            break;
        } else if (pret == -2) {
            /* request is incomplete */
            if (buflen == BUF_SZ) {
                /* request is too long */
                send_bad_request(sock);
                break;
            }
            if (rret == 0) /* EOF */
                break;
            continue;
        }

        /* Request is complete */
        send_response(sock);
        memmove(buf, buf + pret, buflen - pret);
        buflen -= pret;
        prevbuflen = 0;
    }
}

int main(int argc, char *argv[])
{
    int port = DEFAULT_SERVER_PORT;
    if (argc > 1)
        port = atoi(argv[1]);

    int sock = setup_listening_socket(port);
    printf("Listening on port %d\n", port);
    signal(SIGCHLD, SIG_IGN);

    int client_sock;

    while ((client_sock = accept(sock, NULL, NULL)) >= 0) {
        int fret = fork();
        if (fret == -1) {
            perror("fork");
            close(client_sock);
            continue;
        } else if (fret == 0) {
            close(sock);
            handle_client(client_sock);
            close(client_sock);
            return 0;
        }
        close(client_sock);
    }

    fprintf(stderr, "server exiting\n");
    return 0;
}
