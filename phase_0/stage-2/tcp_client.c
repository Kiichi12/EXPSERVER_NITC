#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define _GNU_SOURCE
#define SERVER_PORT 8080
#define BUFF_SIZE 10000

int main() {
    // Creating listening sock
    int client_sock_fd = socket(AF_INET,SOCK_STREAM, 0);

    // Creating an object of struct socketaddr_in
    struct sockaddr_in server_addr;

    // Setting up server addr
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    server_addr.sin_port = htons(SERVER_PORT);

    // Connect to tcp server
    if (connect(client_sock_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
        printf("[ERROR] Failed to connect to tcp server\n");
        exit(1);
    } 
    else {
        printf("[INFO] Connected to tcp server\n");
    }

    while (1) {
        // Get message from client terminal
        char *line;
        size_t line_len = 0, read_n;
        read_n = getline(&line, &line_len, stdin);

        send(client_sock_fd, line, strlen(line), 0);
        
        char buff[BUFF_SIZE];
        memset(buff, 0, BUFF_SIZE);

        // Read message from client to buffer
        read_n = recv(client_sock_fd, buff, sizeof(buff), 0);

        /* close the connection and exit if read_n <= 0 */
        if (read_n <= 0) {
            close(client_sock_fd);
            exit(0);
        }
        
        // Print message from cilent
        printf("[SERVER MESSAGE] %s\n", buff);
    }
    return 0;
}
// gcc -Wall -Wextra -std=c99 -o bin/tcp_client tcp_client.c