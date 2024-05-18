#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "liburing.h"
#include "server.h"

info_t conns[MAX_CONNECTIONS];
char bufs[MAX_CONNECTIONS][MAX_MESSAGE_LEN] = {0};

void add_accept(struct io_uring *ring, int fd, 
        struct sockaddr *client_addr, socklen_t *client_len, unsigned int flags) 
{
    struct io_uring_sqe *sqe;
    sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);
    io_uring_sqe_set_flags(sqe, flags);

    info_t* info = &conns[fd];
    info->fd = fd;
    info->type = ACCEPT;
    
    io_uring_sqe_set_data(sqe, info);
    return;
}

void add_recv(struct io_uring *ring, int fd, size_t msg_size, unsigned int flags)
{
    struct io_uring_sqe *sqe;
    sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, fd, &bufs[fd], msg_size, 0);
    io_uring_sqe_set_flags(sqe, flags);

    info_t* info = &conns[fd];
    info->fd = fd;
    info->type = RECV;

    io_uring_sqe_set_data(sqe, info);
    return;
}

void add_send(struct io_uring *ring, int fd, size_t msg_size, unsigned int flags) 
{
    struct io_uring_sqe *sqe;
    sqe = io_uring_get_sqe(ring);
    io_uring_prep_send(sqe, fd, &bufs[fd], msg_size, 0);
    io_uring_sqe_set_flags(sqe, flags);

    info_t* info = &conns[fd];
    info->fd = fd;
    info->type = SEND;

    io_uring_sqe_set_data(sqe, info);
    return;
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        printf("usage: ./server [port]\n");
        return 0;
    }

    uint32_t port = (uint32_t)strtol(argv[1], NULL, 10);
    struct sockaddr_in server_addr, client_addr;
    socklen_t client_len = sizeof(client_addr);

    //setup socket
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

    // bind and listen
    if (bind(listen_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        perror("Bind()");
        return 1;
    }

    if (listen(listen_fd, BACK_LOG) < 0) {
        perror("Listen()");
        return 1;
    }
    printf("server listening on port %u\n", port);
    
    // initialize io_uring
    struct io_uring_params params;
    struct io_uring ring;

    memset(&params, 0, sizeof(params));

    if (io_uring_queue_init_params(4096, &ring, &params) < 0) {
        perror("io_uring_queue_init_params()");
        return 1;
    }
    
    // check if IORING_FEAT_FAST_POLL is supported
    if (!(params.features & IORING_FEAT_FAST_POLL)) {
        fprintf(stderr, "IORING_FEAT_FAST_POLL is not supported in the kernel\n");
        return 0;
    }


    add_accept(&ring, listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);

    while (true) {
        struct io_uring_cqe* cqe;
        // tell kernel we have put a SQE on the submission ring
        io_uring_submit(&ring);
        
        // wait for new CQE to become available
        int ret = io_uring_wait_cqe(&ring, &cqe);
        if (ret != 0) {
            perror("io_uring_wait_cqe()");
            return 1;
        }
        
        // check how many CQE's are on the CQ ring at this moment
        struct io_uring_cqe *cqes[BACK_LOG];
        int cqe_count = io_uring_peek_batch_cqe(&ring, cqes, sizeof(cqes)/sizeof(cqes[0]));

        // go through all the CQEs
        for (int i = 0; i < cqe_count; i++) {
            struct io_uring_cqe *cqe = cqes[i];
            info_t *info = (info_t *)io_uring_cqe_get_data(cqe);
            int type = info->type;
            switch(type) {
            case ACCEPT: {
                int conn_fd = cqe->res;
                io_uring_cqe_seen(&ring, cqe);
                // new connected client, read data from socket and re-add accept to monitor for new connections
                add_recv(&ring, conn_fd, MAX_MESSAGE_LEN, 0);
                add_accept(&ring, listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);
                break;
            }
            case RECV: {
                int recv_sz = cqe->res;
                if (recv_sz <= 0) {
                    // no bytes available on socket, client must be disconnected
                    io_uring_cqe_seen(&ring, cqe);
                    shutdown(info->fd, SHUT_RDWR);
                }else {
                    // bytes have been read into bufs, now add write to socket sqe
                    io_uring_cqe_seen(&ring, cqe);
                    add_send(&ring, info->fd, recv_sz, 0);
                }
                break;
            }
            case SEND: {
                // write to socket completed, re-add socket read
                io_uring_cqe_seen(&ring, cqe);
                add_recv(&ring, info->fd, MAX_MESSAGE_LEN, 0);
                break;
            }
            default: 
                break;
            }
        }
    }

    return 0;
}
