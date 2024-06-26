#ifndef __SERVER_H
#define __SERVER_H

#define BACK_LOG 512
#define MAX_MESSAGE_LEN 2048
#define MAX_CONNECTIONS 4096

enum {
    ACCEPT,
    POLL_LISTEN,
    POLL_NEW_CONNECTION, 
    SEND,
    RECV,
};

typedef struct info {
    __u32 fd;
    __u16 type;
} info_t;

#endif