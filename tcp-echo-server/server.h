#ifndef __SERVER_H
#define __SERVER_H

#define BACK_LOG 512
#define MAX_MESSAGE_LEN 2048
#define MAX_CONNECTIONS 4096

enum {
    ACCEPT, 
    SEND,
    RECV, 
    PROV_BUF
};

typedef struct info {
    __u32 fd;
    __u16 type;
    __u16 bid;
} info_t;

#endif