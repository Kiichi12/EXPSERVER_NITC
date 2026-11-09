#include "xps.h"

// Global variables
// int epoll_fd;
// struct epoll_event events[MAX_EPOLL_EVENTS];
// vec_void_t listeners;
// vec_void_t connections;
xps_core_t *core;

void sigint_handler(int signum){
  logger(LOG_WARNING, "sigint_handler()", "SIGINT received");

  xps_core_destroy(core);

  exit(EXIT_SUCCESS);
}

int main() {

  signal(SIGINT, sigint_handler);
  
  core = xps_core_create();

  xps_loop_t *loop = xps_loop_create(core);

  core->loop = loop;
  
  xps_core_start(core);

  // // Init lists
  // vec_init(&listeners);
  // vec_init(&connections);

  // // Create listeners on ports 8001, 8002, 8003
  // for (int port = 8001; port <= 8004; port++) {
  //   xps_listener_t *listener = xps_listener_create(epoll_fd, "0.0.0.0", port);
  //   vec_push(&listeners, listener);
  //   logger(LOG_INFO, "main()", "Server listening on port %u", port);
  // }

  /* run the event loop using xps_loop_run() */
  // xps_loop_run(epoll_fd);

}
