#include "xps_listener.h"

void listener_connection_handler(void *ptr);


xps_listener_t *xps_listener_create(xps_core_t *core, const char *host, u_int port) {
  assert(host != NULL);
  assert(is_valid_port(port)); // Will be explained later

  // Create socket instance
  int sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (sock_fd < 0) {
    logger(LOG_ERROR, "xps_listener_create()", "socket() failed");
    perror("Socket Instance Error message");
    return NULL;
  }

  // Make address reusable
  const int enable = 1;
  if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
    logger(LOG_ERROR, "xps_listener_create()", "setsockopt() failed");
    perror("Socket Options Error message");
    close(sock_fd);
    return NULL;
  }

  // Setup listener address
  struct addrinfo *addr_info = xps_getaddrinfo(host, port); 
  if (addr_info == NULL) {
    logger(LOG_ERROR, "xps_listener_create()", "xps_getaddrinfo() failed");
    close(sock_fd);
    return NULL;
  }

  // Binding to port
  if (bind(sock_fd, addr_info->ai_addr, addr_info->ai_addrlen) < 0) {
    logger(LOG_ERROR, "xps_listener_create()", "failed to bind() to %s:%u", host, port);
    perror("Error message");
    freeaddrinfo(addr_info); 
    close(sock_fd);
    return NULL;
  }
  freeaddrinfo(addr_info); 

  // Listening on port
  if (listen(sock_fd, DEFAULT_BACKLOG) < 0) {
    logger(LOG_ERROR, "xps_listener_create()", "listen() failed");
    perror("Error message");
    close(sock_fd);
    return NULL;
  }

  // Create & allocate memory for a listener instance
  xps_listener_t *listener = malloc(sizeof(xps_listener_t));
  if (listener == NULL) {
    logger(LOG_ERROR, "xps_listener_create()", "malloc() failed for 'listener'");
    close(sock_fd);
    return NULL;
  }

  // Init values
  listener->core = core;
  listener->host = host;
  listener->port = port;
  listener->sock_fd = sock_fd;

  // Attach listener to loop
  xps_loop_attach(core->loop, sock_fd, EPOLLIN | EPOLLOUT | EPOLLET, listener, listener_connection_handler, NULL, NULL); 

  logger(LOG_DEBUG, "xps_listener_create()", "created listener on port %d", port);

  return listener;
}

void xps_listener_destroy(xps_listener_t *listener) {

  // Validate params
  assert(listener != NULL);

  // Detach listener from loop
  xps_loop_detach(listener->core->loop, listener->sock_fd);

  // Set listener to NULL in 'listeners' list
  for (int i = 0; i < listener->core->listeners.length; i++) {
    xps_listener_t *curr = listener->core->listeners.data[i];
    if (curr == listener) {
      listener->core->listeners.data[i] = NULL;
      listener->core->n_null_listeners++;
      break;
    }
  }

  // Close socket
  close(listener->sock_fd);

  logger(LOG_DEBUG, "xps_listener_destroy()", "destroyed listener on port %d", listener->port);

  // Free listener instance
  free(listener);

}

// Function definition for read callback for listener
void listener_connection_handler(void *ptr) {
  assert(ptr != NULL);
  xps_listener_t *listener = ptr;

  logger(LOG_DEBUG, "listener_connection_handler()", "listener: %p, sock_fd: %d", listener, listener->sock_fd);

  struct sockaddr conn_addr;
  socklen_t conn_addr_len = sizeof(conn_addr);
  while(1){
    // Accepting connection
    int conn_sock_fd = accept(listener->sock_fd, &conn_addr, &conn_addr_len);
    if (conn_sock_fd < 0) {
      // Check if it's a temporary error
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        logger(LOG_DEBUG, "listener_connection_handler()", "No pending connections (EAGAIN)");
        break;  // Just return, will be called again when there's data
      }

      // Check if the socket is invalid
      if (errno == EBADF) {
        logger(LOG_ERROR, "listener_connection_handler()", "Invalid socket fd: %d", 
                listener->sock_fd);
        // The listener socket is invalid - destroy the listener
        xps_listener_destroy(listener);
        return;
      }

      perror("Error message");
      logger(LOG_ERROR, "listener_connection_handler()", "accept() failed: %s (errno: %d)", 
                strerror(errno), errno);
      return;
    }

    // Set the new connection socket to non-blocking
    int flags = fcntl(conn_sock_fd, F_GETFL, 0);
    if (flags < 0) {
      logger(LOG_ERROR, "listener_connection_handler()", "fcntl F_GETFL failed to make socket non-blocking");
      close(conn_sock_fd);
      return;
    }
    if (fcntl(conn_sock_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
      logger(LOG_ERROR, "listener_connection_handler()", "fcntl F_SETFL failed to make socket non-blocking");
      close(conn_sock_fd);
      return;
    }

    // Creating connection instance
    xps_connection_t *client = xps_connection_create(listener->core, conn_sock_fd);
    if (client == NULL) {
      logger(LOG_ERROR, "xps_listener_connection_handler()", "xps_connection_create() failed");
      close(conn_sock_fd);
      return;
    }
    client->listener = listener;

    xps_session_t *session = xps_session_create(listener->core, client);
    if (session == NULL) {
      logger(LOG_ERROR, "listener_connection_handler()", "xps_session_create() failed");
      xps_connection_destroy(client);
      return;
    }

    // if(listener->port == 8001){
    //   // Reverse proxy: connect to upstream server
    //   xps_connection_t *upstream = xps_upstream_create(listener->core, "127.0.0.1", 3000);
    //   if (upstream == NULL) {
    //     logger(LOG_ERROR, "listener_connection_handler()", "xps_upstream_create() failed");
    //     xps_connection_destroy(client);
    //     continue;
    //   }

    //   // client.source -> upstream.sink  (request)
    //   xps_pipe_t *req_pipe = xps_pipe_create(listener->core, DEFAULT_PIPE_BUFF_THRESH, client->source, upstream->sink);
    //   // upstream.source -> client.sink  (response)
    //   xps_pipe_t *res_pipe = xps_pipe_create(listener->core, DEFAULT_PIPE_BUFF_THRESH, upstream->source, client->sink);

    //   if (req_pipe == NULL || res_pipe == NULL) {
    //     logger(LOG_ERROR, "listener_connection_handler()", "xps_pipe_create() failed");
    //     xps_connection_destroy(upstream);
    //     xps_connection_destroy(client);
    //     continue;
    //   }
    // }
    // else if (listener->port == 8002) {
    //   int error;
    //   xps_file_t *file = xps_file_create(listener->core, "public/sample.txt", &error);
    //   if(file == NULL)
    //   {
    //     logger(LOG_ERROR, "listener_connection_handler()", "xps_file_create() failed");
    //     xps_connection_destroy(client);
    //     continue;
    //   }
    //   if(xps_pipe_create(listener->core, DEFAULT_PIPE_BUFF_THRESH, file->source, client->sink) == NULL) {
    //     logger(LOG_ERROR, "listener_connection_handler()", "xps_pipe_create() failed");
    //     xps_file_destroy(file);
    //     xps_connection_destroy(client);
    //     continue;
    //   }
    // }
    // else{
    //   //Creates pipe for the connection created
    //   xps_pipe_create(listener->core, DEFAULT_PIPE_BUFF_THRESH, client->source, client->sink);
    // }
    

  logger(LOG_INFO, "xps_listener_connection_handler()", "new connection");
  }

}