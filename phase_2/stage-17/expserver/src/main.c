#include "xps.h"

// Global core pointer (needed by sigint_handler)
static xps_core_t *core = NULL;

void sigint_handler(int signum) {
  logger(LOG_WARNING, "sigint_handler()", "SIGINT received");

  if (core != NULL) {
    xps_core_destroy(core);
    core = NULL;
  }

  exit(EXIT_SUCCESS);
}

int core_create(xps_config_t *config) {
  assert(config != NULL);

  core = xps_core_create(config);
  if (core == NULL) {
    logger(LOG_ERROR, "core_create()", "xps_core_create() failed");
    return -1;
  }

  /* For each listener in config, create a real listener, then dup its
   * fd, transfer ownership of the dup to a "copy" listener attached
   * to the loop, and finally destroy the original. */
  /*Create listeners*/
  for (int i = 0; i < config->_all_listeners.length; i++) {
    xps_config_listener_t *cfg_listener = config->_all_listeners.data[i];

    xps_listener_t *listener = xps_listener_create(cfg_listener->host, cfg_listener->port);
    if (listener == NULL) {
      logger(LOG_ERROR, "core_create()", "xps_listener_create() failed for %s:%u", cfg_listener->host, cfg_listener->port);
      return -1;
    }

    /*Duplicate (use dup(fd) to duplicate file descriptor) and add listeners to cores*/
    int dup_fd = dup(listener->sock_fd);
    if (dup_fd < 0) {
      logger(LOG_ERROR, "core_create()", "dup() failed: %s", strerror(errno));
      xps_listener_destroy(listener);
      return -1;
    }

    xps_listener_t *dup_listener = malloc(sizeof(xps_listener_t));
    if (dup_listener == NULL) {
      logger(LOG_ERROR, "core_create()",
             "malloc() failed for 'dup_listener'");
      close(dup_fd);
      xps_listener_destroy(listener);
      return -1;
    }

    /*Initialize dup_listener values*/
    dup_listener->core    = core;
    dup_listener->host    = str_create(cfg_listener->host);
    dup_listener->port    = cfg_listener->port;
    dup_listener->sock_fd = dup_fd;

    /*Attach dup_listener to loop*/
    if (xps_loop_attach(core->loop, dup_fd, EPOLLIN | EPOLLOUT | EPOLLET, dup_listener, listener_connection_handler, NULL, NULL) != OK) {
      logger(LOG_ERROR, "core_create()", "xps_loop_attach() failed");
      free((void *)dup_listener->host);
      free(dup_listener);
      close(dup_fd);
      xps_listener_destroy(listener);
      return -1;
    }

    /*Add listener to 'listeners' list of core*/
    vec_push(&core->listeners, dup_listener);

    /*Destroy original listener*/
    xps_listener_destroy(listener);
  }

  return 0;
}

void core_destroy(xps_core_t *core) {
  if (core == NULL)
    return;

  xps_core_destroy(core);
}

int main(int argc, char *argv[]) {
  signal(SIGINT, sigint_handler);

  xps_cliargs_t *cliargs = xps_cliargs_create(argc, argv);
  if (cliargs == NULL) {
    logger(LOG_ERROR, "main()", "xps_cliargs_create() failed");
    return EXIT_FAILURE;
  }

  xps_config_t *config = xps_config_create(cliargs->config_path);
  if (config == NULL) {
    logger(LOG_ERROR, "main()", "xps_config_create() failed");
    xps_cliargs_destroy(cliargs);
    return EXIT_FAILURE;
  }

  int ret = core_create(config);
  if (ret == -1) {
    logger(LOG_ERROR, "main()", "core_create() failed");
    xps_config_destroy(config);
    xps_cliargs_destroy(cliargs);
    return EXIT_FAILURE;
  }

  xps_core_start(core);

  
  xps_core_destroy(core);
  xps_config_destroy(config);
  xps_cliargs_destroy(cliargs);

  return EXIT_SUCCESS;
}