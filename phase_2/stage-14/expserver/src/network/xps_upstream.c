#include "xps_upstream.h"

xps_connection_t *xps_upstream_create(xps_core_t *core, const char *host, u_int port) {
  assert(core != NULL);
  assert(host != NULL);
  assert(is_valid_port(port));

  // Non-blocking socket for the upstream connection
  int sock_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
  if (sock_fd < 0) {
    logger(LOG_ERROR, "xps_upstream_create()", "socket() failed");
    perror("Error message");
    return NULL;
  }

  // Resolve upstream address
  struct addrinfo *addr_info = xps_getaddrinfo(host, port);
  if (addr_info == NULL) {
    logger(LOG_ERROR, "xps_upstream_create()", "xps_getaddrinfo() failed");
    close(sock_fd);
    return NULL;
  }

  // Connect (non-blocking: EINPROGRESS is expected)
  int connect_error = connect(sock_fd, addr_info->ai_addr, addr_info->ai_addrlen);
  int saved_errno = errno;
  freeaddrinfo(addr_info);

  if (!(connect_error == 0 || saved_errno == EINPROGRESS)) {
    errno = saved_errno;
    logger(LOG_ERROR, "xps_upstream_create()", "connect() failed");
    perror("Error message");
    close(sock_fd);
    return NULL;
  }

  // Wrap the socket in a connection instance (own source + sink)
  xps_connection_t *connection = xps_connection_create(core, sock_fd);
  if (connection == NULL) {
    logger(LOG_ERROR, "xps_upstream_create()", "xps_connection_create() failed");
    close(sock_fd);
    return NULL;
  }

  return connection;
}