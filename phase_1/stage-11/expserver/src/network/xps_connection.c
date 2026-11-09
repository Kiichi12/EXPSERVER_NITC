#include "xps_connection.h"
#include <sys/epoll.h>

// Function to reverse a string in-place
void strrev(char *str) {
  for (int start = 0, end = strlen(str) - 2; start < end; start++, end--) {
    char temp = str[start];
    str[start] = str[end];
    str[end] = temp;
  }
}

// Function declaration for read callback of listener
void connection_source_handler(void *ptr);
void connection_source_close_handler(void *ptr);
void connection_sink_handler(void *ptr);
void connection_sink_close_handler(void *ptr);
void connection_close(xps_connection_t *connection, bool peer_closed);

void connection_loop_read_handler(void *ptr);

void connection_loop_write_handler(void *ptr);

void connection_loop_close_handler(void *ptr);

xps_connection_t *xps_connection_create(xps_core_t *core, u_int sock_fd) {

  xps_connection_t *connection = malloc(sizeof(xps_connection_t));
  if (connection == NULL) {
    logger(LOG_ERROR, "xps_connection_create()", "malloc() failed for 'connection'");
    return NULL;
  }

  xps_pipe_source_t *source = xps_pipe_source_create(connection, connection_source_handler, connection_source_close_handler);
  if (source == NULL) {
    logger(LOG_ERROR, "xps_connection_create()", "xps_pipe_source_create() failed");
    free(connection);
    return NULL;
  }

  xps_pipe_sink_t *sink = xps_pipe_sink_create(connection, connection_sink_handler, connection_sink_close_handler);
  if (sink == NULL) {
    logger(LOG_ERROR, "xps_connection_create()", "xps_pipe_sink_create() failed");
    xps_pipe_source_destroy(source);
    free(connection);
    return NULL;
  }

  /* attach sock_fd to epoll */
  if (xps_loop_attach(core->loop, sock_fd, EPOLLIN | EPOLLOUT | EPOLLET, connection, connection_loop_read_handler, connection_loop_write_handler, connection_loop_close_handler) != OK) {
    logger(LOG_ERROR, "xps_connection_create()", "xps_loop_attach() failed");
    xps_pipe_source_destroy(source);
    xps_pipe_sink_destroy(sink);
    free(connection->remote_ip);
    free(connection);
    return NULL;
  }

  // Init values
  connection->core = core;
  connection->sock_fd = sock_fd;
  connection->listener = NULL;
  connection->remote_ip = get_remote_ip(sock_fd);
  connection->source = source;
  connection->sink = sink;

  /* add connection to 'connections' list */
  vec_push(&core->connections, connection);

  logger(LOG_DEBUG, "xps_connection_create()", "created connection");
  return connection;

}

void xps_connection_destroy(xps_connection_t *connection) {

  /* validate params */
  assert(connection != NULL);

  /* set connection to NULL in 'connections' list */
  for(int i = 0; i < connection->core->connections.length; i++)
  {
    xps_connection_t *curr = connection->core->connections.data[i];
    if (curr == connection) {
      connection->core->connections.data[i] = NULL;
      connection->core->n_null_connections++;
      break;
    }
  }

  /* detach connection from loop */
  xps_loop_detach(connection->core->loop, connection->sock_fd);

  /* close connection socket FD */
  close(connection->sock_fd);

  // Destroy source and sink (also detaches them from the pipe)
  xps_pipe_source_destroy(connection->source);
  xps_pipe_sink_destroy(connection->sink);

  /* free connection->remote_ip */
  free(connection->remote_ip);

  /* free connection instance */
  free(connection);

  logger(LOG_DEBUG, "xps_connection_destroy()", "destroyed connection");
}


void connection_loop_close_handler(void *ptr) {
    assert(ptr != NULL);

    xps_connection_t *connection = (xps_connection_t *)ptr;

    logger(LOG_INFO, "connection_loop_close_handler()",
           "closing connection, sock_fd: %d",
           connection->sock_fd);

    xps_connection_destroy(connection);
}

void connection_loop_read_handler(void *ptr) {
  assert(ptr != NULL);
  xps_connection_t *connection = (xps_connection_t *)ptr;

  connection->source->ready = true;
}

void connection_loop_write_handler(void *ptr) {
  assert(ptr != NULL);
  xps_connection_t *connection = (xps_connection_t *)ptr;

  connection->sink->ready = true;
}

void connection_source_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_source_t *source = ptr;
  xps_connection_t *connection = source->ptr;

  char *buff = malloc(DEFAULT_BUFFER_SIZE);
  if (buff == NULL) {
      logger(LOG_ERROR, "connection_source_handler()", "malloc for buff failed");
      return;
  }

  /*Read from socket using recv()*/
  long read_n = recv(connection->sock_fd, buff, DEFAULT_BUFFER_SIZE, 0);

  // Socket would block
  if (read_n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    // xps_buffer_destroy(buff);
    source->ready = false;
    return;
  }

  //Socket error
  if(read_n < 0) {
    // xps_buffer_destroy(buff);
    logger(LOG_ERROR, "connection_source_handler()", "recv() failed");
    connection_close(connection, false);
    return;
  }

  //Peer closed connection
  if(read_n == 0) {
    // xps_buffer_destroy(buff);
    connection_close(connection, false);
    return;
  }

  buff[read_n] = '\0';

  /* print client message */
  // printf("[CLIENT MESSAGE] %s\n", buff);

  /* reverse client message */
  // strrev(buff);

  xps_buffer_t *buffer = xps_buffer_create(1024, read_n, (u_char *)buff);

  if (xps_pipe_source_write(source, buffer) != OK) {
    logger(LOG_ERROR, "connection_source_handler()", "xps_pipe_source_write() failed");
    xps_buffer_destroy(buffer);
    connection_close(connection, false);
    return;
  }

  // xps_buffer_destroy(buffer);
}

void connection_source_close_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_source_t *source = ptr;
  xps_connection_t *connection = source->ptr;

  if (!connection->source->active && !connection->sink->active)
    connection_close(connection, false);
}

void connection_sink_handler(void *ptr) {
  assert(ptr != NULL);
  xps_pipe_sink_t *sink = ptr;
  xps_connection_t *connection = sink->ptr;
  xps_buffer_t *buff = xps_pipe_sink_read(sink, sink->pipe->buff_list->len);/*read from pipe*/

  if(buff == NULL) {
    logger(LOG_ERROR, "connection_sink_handler()", "xps_pipe_sink_read() failed");
    return;
  }

  // Write to socket
  long write_n = send(connection->sock_fd, buff->data, buff->len, MSG_NOSIGNAL);

  xps_buffer_destroy(buff);

  // Socket would block
  if (write_n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
    sink->ready = false;
    return;
  }

  // Socket error
  if (write_n < 0) {
    logger(LOG_ERROR, "connection_sink_handler()", "send() failed");
    connection_close(connection, false);
    return;
  }

  if (write_n == 0)
    return;

  if (xps_pipe_sink_clear(sink, write_n) != OK)
    logger(LOG_ERROR, "connection_sink_handler()", "failed to clear %d bytes from sink", write_n);
}

void connection_sink_close_handler(void *ptr) {
    assert(ptr != NULL);
    xps_pipe_sink_t *sink = ptr;
    xps_connection_t *connection = sink->ptr;

    if (!connection->source->active && !connection->sink->active)
        connection_close(connection, false);
}

void connection_close(xps_connection_t *connection, bool peer_closed) {
    assert(connection != NULL);
    logger(LOG_INFO, "connection_close()", peer_closed ? "peer closed connection" : "closing connection");
    xps_connection_destroy(connection);
}

// // Function definition for read callback for connection
// void connection_read_handler(void *ptr) {
//   assert(ptr != NULL);
//   xps_connection_t *connection = ptr;

//   /* same code from xps_connection_read_handler() */
//   char *buff = malloc(DEFAULT_BUFFER_SIZE);

//   long read_n = recv(connection->sock_fd, buff, DEFAULT_BUFFER_SIZE, 0);
//   // read_n = recv(conn_sock_fd, buff, sizeof(buff), 0);

  
//   if (read_n < 0) {
//     if(errno == EAGAIN || errno == EWOULDBLOCK){
//         logger(LOG_DEBUG, "xps_connection_read_handler()", "read_ready is set to false(Nothing to read in buffer)");
//         connection->read_ready = false;
//         return;
//     }
//     else{
//       logger(LOG_ERROR, "xps_connection_read_handler()", "recv() failed");
//       perror("Error message");
//       xps_connection_destroy(connection);
//       return;
//     }
    
//   }

//   if (read_n == 0) {
//     logger(LOG_INFO, "connection_read_handler()", "peer closed connection");
//     xps_connection_destroy(connection);
//     return;
//   }

//   buff[read_n] = '\0';

//   /* print client message */
//   printf("[CLIENT MESSAGE] %s\n", buff);

//   /* reverse client message */
//   strrev(buff);

//   xps_buffer_t *buffer = xps_buffer_create(1024, read_n, (u_char *)buff);
//   xps_buffer_list_append(connection->write_buff_list, buffer);
//   connection->write_ready = true;
//   // free(buff);

//   // free(buff);
//   // // Sending reversed message to client
//   // long bytes_written = 0;
//   // long message_len = read_n;
//   // while (bytes_written < message_len) {
//   //   long write_n = send(connection->sock_fd, buff + bytes_written, message_len - bytes_written, 0);
//   //   if (write_n < 0) {
//   //     logger(LOG_ERROR, "xps_connection_read_handler()", "send() failed");
//   //     perror("Error message");
//   //     xps_connection_destroy(connection);
//   //     return;
//   //   }
//   //   bytes_written += write_n;
//   // }

// }


// void xps_connection_write_handler(xps_connection_t *connection) {
//   // Sending reversed message to client
//   xps_buffer_t *buff = xps_buffer_list_read(connection->write_buff_list, connection->write_buff_list->len);
//   long bytes_written = 0;
//   long message_len = buff->len;
//   while (bytes_written < message_len) {
//     long write_n = send(connection->sock_fd, buff->data + bytes_written, message_len - bytes_written, 0);
//     if (write_n == -1){
//       if(errno == EAGAIN || errno == EWOULDBLOCK){
//         connection->write_ready = false;
//         return;
//       }
//       else{
//         xps_connection_destroy(connection);
//         return;
//       }
//     }
//     bytes_written += write_n;
    
//   }
//   // xps_buffer_list_clear(connection->write_buff_list, bytes_written);
// }

// void connection_write_handler(void *ptr) {
//   assert(ptr != NULL);
  
//   xps_connection_t *connection = (xps_connection_t *)ptr;
//   if (connection->write_buff_list == NULL ||
//         connection->write_buff_list->len == 0) {
//         return;
//   }

//   xps_connection_write_handler(connection);
//   printf("clearing %ld bytes\n", connection->write_buff_list->len);
//   if(connection->write_buff_list->len > 0)
//     xps_buffer_list_clear(connection->write_buff_list, connection->write_buff_list->len);
// }
// void xps_connection_read_handler(xps_connection_t *connection) {

//   /* validate params */
//   assert(connection != NULL);

//   char buff[DEFAULT_BUFFER_SIZE];

//   long read_n = recv(connection->sock_fd, buff, sizeof(buff), 0);
//   // read_n = recv(conn_sock_fd, buff, sizeof(buff), 0);

//   if (read_n < 0) {
//     logger(LOG_ERROR, "xps_connection_read_handler()", "recv() failed");
//     perror("Error message");
//     xps_connection_destroy(connection);
//     return;
//   }

//   if (read_n == 0) {
//     logger(LOG_INFO, "connection_read_handler()", "peer closed connection");
//     xps_connection_destroy(connection);
//     return;
//   }

//   buff[read_n] = '\0';

//   /* print client message */
//   printf("[CLIENT MESSAGE] %s\n", buff);

//   /* reverse client message */
//   strrev(buff);

//   // Sending reversed message to client
//   long bytes_written = 0;
//   long message_len = read_n;
//   while (bytes_written < message_len) {
//     long write_n = send(connection->sock_fd, buff + bytes_written, message_len - bytes_written, 0);
//     if (write_n < 0) {
//       logger(LOG_ERROR, "xps_connection_read_handler()", "send() failed");
//       perror("Error message");
//       xps_connection_destroy(connection);
//       return;
//     }
//     bytes_written += write_n;
//   }

// }


//   }

// }

