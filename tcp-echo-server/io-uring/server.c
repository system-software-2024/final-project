#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "liburing.h"
#include "server.h"

char bufs[MAX_CONNECTIONS][MAX_MESSAGE_LEN] = {0};
int group_id = 1337;

void add_accept(struct io_uring *ring, int fd, 
        struct sockaddr *client_addr, socklen_t *client_len, unsigned int flags) 
{
    struct io_uring_sqe *sqe;
    sqe = io_uring_get_sqe(ring);
    io_uring_prep_accept(sqe, fd, client_addr, client_len, 0);
    io_uring_sqe_set_flags(sqe, flags);

    info_t info = {
        .fd = fd,
        .type = ACCEPT
    };

    memcpy(&sqe->user_data, &info, sizeof(info_t));
    return;
}

void add_recv(struct io_uring *ring, int fd, unsigned int gid, size_t msg_size, unsigned int flags)
{
    struct io_uring_sqe *sqe;
    sqe = io_uring_get_sqe(ring);
    io_uring_prep_recv(sqe, fd, NULL, msg_size, 0);
    io_uring_sqe_set_flags(sqe, flags);
    sqe->buf_group = gid;
    
    info_t info = {
        .fd = fd,
        .type = RECV
    };
    memcpy(&sqe->user_data, &info, sizeof(info_t));
    return;
}

void add_send(struct io_uring *ring, int fd, __u16 bid, size_t msg_size, unsigned int flags) 
{
    struct io_uring_sqe *sqe;
    sqe = io_uring_get_sqe(ring);
    io_uring_prep_send(sqe, fd, &bufs[bid], msg_size, 0);
    io_uring_sqe_set_flags(sqe, flags);

    info_t info = {
        .fd = fd, 
        .type = SEND,
        .bid = bid
    };

    memcpy(&sqe->user_data, &info, sizeof(info_t));
    return;
}

void add_provide_buf(struct io_uring *ring, __u16 bid, unsigned int gid)
{
    struct io_uring_sqe *sqe;
    sqe = io_uring_get_sqe(ring);
    io_uring_prep_provide_buffers(sqe, bufs[bid], MAX_MESSAGE_LEN, 1, gid, bid);

    info_t info = {
        .fd = 0,
        .type = PROV_BUF,
    };

    memcpy(&sqe->user_data, &info, sizeof(info_t));
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

    if (io_uring_queue_init_params(2048, &ring, &params) < 0) {
        perror("io_uring_queue_init_params()");
        return 1;
    }
    
    // check if IORING_FEAT_FAST_POLL is supported
    if (!(params.features & IORING_FEAT_FAST_POLL)) {
        fprintf(stderr, "IORING_FEAT_FAST_POLL is not supported in the kernel\n");
        return 0;
    }

    // check if buffer selection is supported
    struct io_uring_probe *probe;
    probe = io_uring_get_probe_ring(&ring);
    if (!probe || !io_uring_opcode_supported(probe, IORING_OP_PROVIDE_BUFFERS)) {
        fprintf(stderr, "Buffer Selection is not supported in the kernel\n");
        return 0;
    }
    io_uring_free_probe(probe);

    // register buffer for buffer selection
    struct io_uring_sqe *sqe;
    struct io_uring_cqe *cqe;

    sqe = io_uring_get_sqe(&ring);
    io_uring_prep_provide_buffers(sqe, bufs, MAX_MESSAGE_LEN, MAX_CONNECTIONS, group_id, 0);        
    io_uring_submit(&ring);
   
    io_uring_wait_cqe(&ring, &cqe);
    if (cqe->res < 0) {
        fprintf(stderr, "cqe->res: %d\n", cqe->res);
        return 1;
    }
    io_uring_cqe_seen(&ring, cqe);
    // add first accept SQE to monitor for new incoming connections
    add_accept(&ring, listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);

    while (true) {
        io_uring_submit_and_wait(&ring, 1);
        struct io_uring_cqe* cqe;
        unsigned int head;
        unsigned int count = 0;

        io_uring_for_each_cqe(&ring, head, cqe) {
            
            ++count;
            info_t info;
            memcpy(&info, &cqe->user_data, sizeof(info_t));

            int type = info.type;
            if (cqe->res == -ENOBUFS) {
                fprintf(stderr, "buffers in automatic buffer selection is empty\n");
                return 1;
            }
            switch (type) {
            case PROV_BUF: {
                if (cqe->res < 0) {
                    fprintf(stderr, "cqe->res: %d\n", cqe->res);
                    return 1;
                }
                break;
            }
            case ACCEPT: {
                int conn_fd = cqe->res;
                // only read when there is no error
                if (conn_fd >= 0) {
                    add_recv(&ring, conn_fd, group_id, MAX_MESSAGE_LEN, IOSQE_BUFFER_SELECT);
                }
                // new connected client. Read data from socket and re-add accept to monitor for new connections.
                add_accept(&ring, listen_fd, (struct sockaddr *)&client_addr, &client_len, 0);
                break;
            }
            case RECV: {
                int recv_sz = cqe->res;
                int bid = (cqe->flags) >> 16;
                if (cqe->res <= 0) {
                    // read failed, re-add the buffer
                    add_provide_buf(&ring, bid, group_id);
                    close(info.fd);
                } else {
                    // have been received to bufs, send the same data to SQE
                    add_send(&ring, info.fd, bid, recv_sz, 0); 
                }
                break;
            }
            case SEND: {
                // write complete, re-add the buffer
                add_provide_buf(&ring, info.bid, group_id);
                // add recv to the existing connection
                add_recv(&ring, info.fd, group_id, MAX_MESSAGE_LEN, IOSQE_BUFFER_SELECT);
                break;
            }
            default: 
                break;
            }
        }
        io_uring_cq_advance(&ring, count);
    }

    return 0;
}
