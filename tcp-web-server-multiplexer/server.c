#include<stdio.h>
#include<stdlib.h>
#include <sys/socket.h>
#include <unistd.h> 
#include <poll.h>
#include <arpa/inet.h>
#include "picohttpparser.h"
#include <errno.h>
#include <string.h>
#define ERR_EXIT(a){ perror(a); exit(1); }


struct {
    int listenfd;   
    struct sockaddr_in addr;
    socklen_t addrlen;
    int maxconn;
} server;

enum state {
    ACCEPT_CONNECTION,
    HTTP_REQUEST,
    HTTP_RESPONSE
};

struct {
    struct pollfd *polls;
    enum state *states;
    int size;
} poll_queue;


char* msg = "HTTP/1.1 200 OK\r\nServer: Assdi2024Server/1.0\r\nContent-Type: text/html\r\nContent-Length: 98\r\n\r\n<!DOCTYPE html><head><title>Hello, World!</title></head><body><h1>Hello, World!</h1></body></html>";


void add_queue(int fd, short events, enum state state) {
    poll_queue.polls[poll_queue.size].fd = fd;
    poll_queue.polls[poll_queue.size].events = events;
    poll_queue.polls[poll_queue.size].revents = 0;
    //  = {fd, events, 0};
    poll_queue.states[poll_queue.size++] = state;
}

void remove_queue(int index) {
    poll_queue.polls[index] = poll_queue.polls[--poll_queue.size];
    poll_queue.states[index] = poll_queue.states[poll_queue.size];
}

void queue_init() {
    poll_queue.size = 0;
    poll_queue.polls =  (struct pollfd *) malloc(sizeof(struct pollfd)* server.maxconn);
    poll_queue.states =  (enum state *) malloc(sizeof(enum state)* server.maxconn);
    add_queue(server.listenfd, POLLIN, ACCEPT_CONNECTION);
}

void accept_connection() {
    struct sockaddr_in  client_addr;
    int client_addr_len = sizeof(client_addr);
    int fd = accept(server.listenfd, (struct sockaddr *)&client_addr, (socklen_t*)&client_addr_len);
    add_queue(fd, POLLIN, HTTP_REQUEST);
}


void init_server(int argc, char **argv){
    if(argc != 2){
        exit(-1);
    }
    // Get socket file descriptor
    if((server.listenfd = socket(AF_INET , SOCK_STREAM , 0)) < 0){
        ERR_EXIT("socket()");
    } 

    // Set server address information
    bzero(&server.addr, sizeof(server.addr)); // erase the data
    server.addr.sin_family = AF_INET;
    server.addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server.addr.sin_port = htons(atoi(argv[1]));
    server.addrlen = sizeof(server.addr);
    server.maxconn = getdtablesize();
    // Bind the server file descriptor to the server address
    if(bind(server.listenfd, (struct sockaddr *)&server.addr , sizeof(server.addr)) < 0){
        ERR_EXIT("bind()");
    }
    // Listen on the server file descriptor
    if(listen(server.listenfd , 1000) < 0){
        ERR_EXIT("listen()");
    }
}



void handle_request(int fd){
    char buf[4096];
    char bmethod[512];
    bzero(bmethod, 512);

    const char *method, *path;
    int pret, minor_version;
    struct phr_header headers[100];
    size_t buflen = 0, prevbuflen = 0, method_len, path_len, num_headers;
    ssize_t rret;
    while(1){
        while ((rret = read(fd, buf + buflen, sizeof(buf) - buflen)) == -1 && errno == EINTR);
        prevbuflen = buflen;
        buflen += rret;
        /* parse the request */
        num_headers = sizeof(headers) / sizeof(headers[0]);
        pret = phr_parse_request(buf, buflen, &method, &method_len, &path, &path_len,
                                &minor_version, headers, &num_headers, prevbuflen);
        if (pret > 0)
            break; /* successfully parsed the request */
    }
    sprintf(bmethod, "%.*s",(int)method_len, method);

    // printf("request is %d bytes long\n", pret);
    // printf("method is %.*s\n", (int)method_len, method);
    // printf("path is %.*s\n", (int)path_len, path);
    // printf("HTTP version is 1.%d\n", minor_version);

    if(strcmp(bmethod, "GET") == 0){
        add_queue(fd, POLLOUT, HTTP_RESPONSE);
    }
}

void handle_response(int fd){
    write(fd, msg, strlen(msg));
    add_queue(fd, POLLIN, HTTP_REQUEST);
}

int main(int argc, char *argv[]){
    init_server(argc, argv);
    queue_init();
    while(1) {
        //cerr << "poll size: " << poll_queue.size << '\n';
        if(poll(poll_queue.polls, poll_queue.size, -1) < 0) perror("polling");
        short revents;
        for(int i = 0; i < poll_queue.size; ++i) if(revents = poll_queue.polls[i].revents) {
            // perror("state");
            if(poll_queue.states[i] == ACCEPT_CONNECTION) {
                accept_connection();
                continue;
            }
            if(revents & (POLLHUP | POLLERR | POLLNVAL)) close(poll_queue.polls[i].fd);
            else switch(poll_queue.states[i]) {
                case HTTP_REQUEST:
                    // fprintf(stderr, "recevive from request %d\n", poll_queue.polls[i].fd);
                    handle_request(poll_queue.polls[i].fd);
                    break;
                case HTTP_RESPONSE:
                    handle_response(poll_queue.polls[i].fd);
                    break;
                default:
                    break;
            }
            remove_queue(i--);
        }
    }
}
