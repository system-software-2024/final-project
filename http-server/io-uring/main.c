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

static bool should_close_connection(struct phr_header *headers, size_t num_headers)
{
    for (size_t i = 0; i < num_headers; i++) {
        if (headers[i].name_len == 10 && strncasecmp(headers[i].name, "connection", 10) == 0) {
            if (headers[i].value_len == 5 && strncasecmp(headers[i].value, "close", 5) == 0)
                return true;
        }
    }
    return false;
}

struct conn {
    struct io_uring *ring;
    int sock;
    int buflen, prevlen;
    bool shutdown;
    bool reading, writing;
    char buf[BUF_SZ];
    int sendbuf_sz;
    const char *sendbuf;
};

struct req {
    struct conn *conn;
    int type;
    char *buf;
    int pos, len;
};

static void add_accept_request(struct io_uring *ring, int sock)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(ring);
    struct req *request = calloc(1, sizeof(*request));
    request->type = EVENT_TYPE_ACCEPT;
    io_uring_prep_accept(sqe, sock, NULL, NULL, 0);
    io_uring_sqe_set_data(sqe, request);
}

static void add_read_request(struct conn *conn)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(conn->ring);
    struct req *request = calloc(1, sizeof(*request));
    request->type = EVENT_TYPE_READ;
    request->conn = conn;
    io_uring_prep_recv(sqe, conn->sock, conn->buf + conn->buflen, BUF_SZ - conn->buflen, 0);
    io_uring_sqe_set_data(sqe, request);
    conn->reading = true;
}

static void close_connection(struct conn *conn)
{
    struct io_uring_sqe* sqe = io_uring_get_sqe(conn->ring);
    io_uring_prep_close(sqe, conn->sock);
    io_uring_sqe_set_data(sqe, NULL);
    free(conn);
}

static void add_write_request(struct conn *conn, const char *buf, int buflen)
{
    struct io_uring_sqe *sqe = io_uring_get_sqe(conn->ring);
    struct req *request = calloc(1, sizeof(*request));
    request->type = EVENT_TYPE_WRITE;
    request->conn = conn;
    io_uring_prep_send(sqe, conn->sock, buf, buflen, 0);
    io_uring_sqe_set_data(sqe, request);
    conn->writing = true;
}

static void handle_accept(struct io_uring *ring, struct io_uring_cqe* cqe)
{   
    if (cqe->res < 0)
        return;

    struct conn *conn = calloc(1, sizeof(*conn));
    conn->sock = cqe->res;
    conn->ring = ring;
    add_read_request(conn);
}

static void check_and_close_conn(struct conn *conn)
{
    if (!conn->reading && !conn->writing)
        close_connection(conn);
}

static void send_bad_request(struct conn *conn)
{
    add_write_request(conn, bad_request, strlen(bad_request));
    conn->shutdown = true;
}

/* call at the end of read/write */
static void handle_conn(struct conn *conn)
{
    const char *method, *path;
    size_t method_len, path_len, num_headers = 50;
    int minor_version;
    struct phr_header headers[50];
    bool cont = true;

    int pret = phr_parse_request(conn->buf, conn->buflen, &method, &method_len,
                                 &path, &path_len, &minor_version, headers, &num_headers, 0);

    if (pret == -2) {
        if (conn->buflen == BUF_SZ) {
            send_bad_request(conn);
        } else if (!conn->reading) {
            add_read_request(conn);
        }
        return;
    }

    /* Error Handling */
    if (pret < 0 || method_len != 3 || memcmp(method, "GET", 3) != 0) {
        send_bad_request(conn);
        return;
    }

    cont = !conn->shutdown && minor_version == 1 &&
            !should_close_connection(headers, num_headers);

    /* Move remaining buffers */
    memmove(conn->buf, conn->buf + pret, conn->buflen - pret);
    conn->buflen -= pret;

    /* Normal Response */
    add_write_request(conn, response, strlen(response));

    /* Check whether to close the connection */
    conn->shutdown = !cont;

    if (cont && !conn->reading) {
        add_read_request(conn);
    }
}

static void handle_read(struct io_uring_cqe* cqe)
{
    struct req *request = io_uring_cqe_get_data(cqe);
    struct conn *conn = request->conn;
    conn->reading = false;

    if (cqe->res <= 0) {
        conn->shutdown = true;
        check_and_close_conn(conn);
        return;
    }

    conn->buflen += cqe->res;

    if (!conn->writing) {
        handle_conn(conn);
    }

    check_and_close_conn(conn);
}

static void handle_write(struct io_uring_cqe* cqe)
{
    struct req *request = io_uring_cqe_get_data(cqe);
    struct conn *conn = request->conn;
    conn->writing = false;

    if (cqe->res <= 0) {
        conn->shutdown = true;
        check_and_close_conn(conn);
        return;
    }

    if (cqe->res < conn->sendbuf_sz) {
        conn->sendbuf += cqe->res;
        conn->sendbuf_sz -= cqe->res;
        add_write_request(conn, conn->sendbuf, conn->sendbuf_sz);
    } else if (!conn->shutdown && !conn->reading) {
        handle_conn(conn);
    }

    check_and_close_conn(conn);
}

void server_loop(int sock)
{
    struct io_uring ring;

    io_uring_queue_init(QUEUE_DEPTH, &ring, 0);
    add_accept_request(&ring, sock);

    while (1) {
        struct io_uring_cqe* cqe;
        io_uring_submit_and_wait(&ring, 1);

        while(io_uring_peek_cqe(&ring, &cqe) == 0) {
            if (io_uring_sq_space_left(&ring) < MAX_SQE_PER_LOOP) {
                break;
            }

            struct req *request = io_uring_cqe_get_data(cqe);

            if (!request) {
                io_uring_cqe_seen(&ring, cqe);
                continue;
            }

            int type = request->type;

            switch(type) {
            case EVENT_TYPE_ACCEPT:
                handle_accept(&ring, cqe);
                add_accept_request(&ring, sock);
                break;
            case EVENT_TYPE_READ:
                handle_read(cqe);
                break;
            case EVENT_TYPE_WRITE:
                handle_write(cqe);
                break;
            }

            io_uring_cqe_seen(&ring, cqe);
            free(request);
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
