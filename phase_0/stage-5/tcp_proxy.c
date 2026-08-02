#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <sys/epoll.h>
#include <stdbool.h>

#define PORT 8080
#define BUFF_SIZE 10000
#define MAX_ACCEPT_BACKLOG 5
#define MAX_EPOLL_EVENTS 10
#define UPSTREAM_PORT 3000
#define MAX_SOCKS 10

int listen_sock_fd, epoll_fd;
struct epoll_event events[MAX_EPOLL_EVENTS];
int route_table[MAX_SOCKS][2], route_table_size = 0;

// Function to reverse a string in-place
void strrev(char *str) {
  for (int start = 0, end = strlen(str) - 2; start < end; start++, end--) {
    char temp = str[start];
    str[start] = str[end];
    str[end] = temp;
  }
}

bool isClientConnFd(int fd) {
    for (int i = 0; i < route_table_size; i++) {
        if (route_table[i][0] == fd) {
            return true;
        }
    }
    return false;
}

int create_loop() {
    /* return new epoll instance */
    int epoll_fd = epoll_create1(0);
    return epoll_fd;
}

void loop_attach(int epoll_fd, int fd, int eventFlag) {
    /* attach fd to epoll */
    struct epoll_event event;
    event.events = eventFlag;
    event.data.fd = fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &event);
}

int create_server() {
    /* create listening socket and return it */
    listen_sock_fd = socket(AF_INET, SOCK_STREAM, 0);

    // Setting sock opt reuse addr
    int enable = 1;
    setsockopt(listen_sock_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int)); 

    struct sockaddr_in server_addr;

    // Setting up server addr
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(PORT);

    // Binding listening sock to port
    if (bind(listen_sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        printf("[ERROR] Failed to bind socket to port %d\n", PORT);
        close(listen_sock_fd);
        exit(1);
    }

    return listen_sock_fd;
}

int connect_upstream() {

  int upstream_sock_fd = socket(AF_INET, SOCK_STREAM, 0); /* create a upstrem socket */

  struct sockaddr_in upstream_addr;
  /* add upstream server details */
  upstream_addr.sin_family = AF_INET;
  upstream_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
  upstream_addr.sin_port = htons(UPSTREAM_PORT);

  if(connect(upstream_sock_fd, (struct sockaddr *)&upstream_addr, sizeof(upstream_addr)) < 0) {
    printf("[ERROR] Failed to connect to upstream server\n");
    close(upstream_sock_fd);
    exit(1);
  }

  return upstream_sock_fd;
}

void accept_connection() {

  struct sockaddr_in client_addr;
  socklen_t client_addr_len;
  int conn_sock_fd = accept(listen_sock_fd, (struct sockaddr *)&client_addr, &client_addr_len);

  printf("[INFO] Client connected from %s:%d\n", inet_ntoa(client_addr.sin_addr), ntohs(client_addr.sin_port));
  

  /* add conn_sock_fd to loop using loop_attach() */
  loop_attach(epoll_fd, conn_sock_fd, EPOLLIN);

  // create connection to upstream server
  int upstream_sock_fd = connect_upstream();

  /* add upstream_sock_fd to loop using loop_attach() */
  loop_attach(epoll_fd, upstream_sock_fd, EPOLLIN);

  // add conn_sock_fd and upstream_sock_fd to routing table
  route_table[route_table_size][0] = conn_sock_fd;
  route_table[route_table_size][1] = upstream_sock_fd;
  route_table_size += 1;

}

void handle_client(int conn_sock_fd) {
    char buff[BUFF_SIZE];
    memset(buff, 0, BUFF_SIZE);

    // Read message from client to buffer
    ssize_t read_n = recv(conn_sock_fd, buff, sizeof(buff), 0);

    // Client closed connection or error occurred
    if (read_n < 0) {
        printf("[INFO] Error occured. Closing server\n");
        close(conn_sock_fd);
        exit(1);
    }
    else if (read_n == 0) {
        printf("[INFO] Client Disconnected.\n");
        close(conn_sock_fd);
        return;
    }

    // Print message from client
    printf("[CLIENT MESSAGE] %s", buff);


    /* find the right upstream socket from the route table */
    int upstream_sock_fd = -1;
    for(int i = 0; i < route_table_size; i++) {
        if (route_table[i][0] == conn_sock_fd) {
            // found the upstream socket
            upstream_sock_fd = route_table[i][1];
            break;
        }
    }

    // sending client message to upstream
    int bytes_written = 0;
    int message_len = read_n;
    while (bytes_written < message_len) {
        int n = send(upstream_sock_fd, buff + bytes_written, message_len - bytes_written, 0);
        bytes_written += n;
    }

}

void handle_upstream(int upstream_sock_fd) {
    // Create buffer to store client message
    char buff[BUFF_SIZE];
    memset(buff, 0, BUFF_SIZE);

    // Read message from client to buffer
    ssize_t read_n = recv(upstream_sock_fd, buff, sizeof(buff), 0);

    // Client closed connection or error occurred
    if (read_n < 0) {
        printf("[INFO] Error occured. Closing server\n");
        close(upstream_sock_fd);
        exit(1);
    }
    else if (read_n == 0) {
        printf("[INFO] Client Disconnected.\n");
        close(upstream_sock_fd);   
        return;   
    }

    // Print message from client
    printf("[CLIENT MESSAGE IN UPSTREAM] %s", buff);

    int client_conn_fd = -1;
    for(int i = 0; i < route_table_size; i++) {
        if (route_table[i][1] == upstream_sock_fd) {
            // found the upstream socket
            client_conn_fd = route_table[i][0];
            break;
        }
    }

    // Send reversed string back to client
    send(client_conn_fd, buff, read_n, 0);
}

void loop_run(int epoll_fd) {
    while (1) {

        printf("[DEBUG] Epoll wait\n");
        int n_ready_fds = epoll_wait(epoll_fd, events, MAX_EPOLL_EVENTS, -1);
        int curr_fd = -1;
        for (int i = 0; i < n_ready_fds; i++) {
            curr_fd = events[i].data.fd;
            if (curr_fd == listen_sock_fd)
                accept_connection(); 
            else if (isClientConnFd(curr_fd))
                handle_client(curr_fd); 
            else
                handle_upstream(curr_fd); 
        }

    }
}


int main() {

    listen_sock_fd = create_server();

    // Starting to listen
    listen(listen_sock_fd, MAX_ACCEPT_BACKLOG);
    printf("[INFO] Server listening on port %d\n", PORT);

    epoll_fd = create_loop();

    /* attach server to event loop using loop_attach() */
    loop_attach(epoll_fd, listen_sock_fd, EPOLLIN);

    /* start event loop with loop_run() */
    loop_run(epoll_fd);

}