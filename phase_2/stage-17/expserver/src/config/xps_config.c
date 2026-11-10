#include "xps_config.h"

/* ---------------------------------------------------------------------------------------------
 * destroy helpers. Strings inside the structs point into the Parson JSON value and are freed
 * together with it (xps_config_destroy), so only the structs and vectors are freed here.
 * ------------------------------------------------------------------------------------------- */

static void route_destroy(xps_config_route_t *route) {
  if (route == NULL)
    return;
  vec_deinit(&route->index);
  vec_deinit(&route->upstreams);
  free(route);
}

static void server_destroy(xps_config_server_t *server) {
  if (server == NULL)
    return;

  for (int i = 0; i < server->listeners.length; i++)
    free(server->listeners.data[i]);
  vec_deinit(&server->listeners);

  vec_deinit(&server->hostnames);

  for (int i = 0; i < server->routes.length; i++)
    route_destroy(server->routes.data[i]);
  vec_deinit(&server->routes);

  free(server);
}

/* ---------------------------------------------------------------------------------------------
 * parsing
 * ------------------------------------------------------------------------------------------- */

// Pushes every string of obj[key] (a JSON array of strings) into out
static int parse_string_array(JSON_Object *obj, const char *key, vec_void_t *out, bool required) {
  JSON_Array *arr = json_object_get_array(obj, key);
  if (arr == NULL)
    return required ? E_FAIL : OK;

  for (size_t i = 0; i < json_array_get_count(arr); i++) {
    const char *str = json_array_get_string(arr, i);
    if (str == NULL) {
      logger(LOG_ERROR, "parse_string_array()", "'%s'[%lu] is not a string", key, (unsigned long)i);
      return E_FAIL;
    }
    if (vec_push(out, (void *)str) != 0)
      return E_FAIL;
  }
  return OK;
}

static xps_config_listener_t *parse_listener(JSON_Object *listener_object) {
  assert(listener_object != NULL);

  const char *host = json_object_get_string(listener_object, "host");
  if (host == NULL || json_object_get_value(listener_object, "port") == NULL) {
    logger(LOG_ERROR, "parse_listener()", "listener needs a 'host' (string) and a 'port' (number)");
    return NULL;
  }

  double port = json_object_get_number(listener_object, "port");
  if (port < 1 || port > 65535 || port != (double)(u_int)port) {
    logger(LOG_ERROR, "parse_listener()", "invalid port %.0f", port);
    return NULL;
  }

  xps_config_listener_t *listener = malloc(sizeof(xps_config_listener_t));
  if (listener == NULL) {
    logger(LOG_ERROR, "parse_listener()", "malloc() failed for 'listener'");
    return NULL;
  }
  listener->host = host;
  listener->port = (u_int)port;
  return listener;
}

static xps_config_route_t *parse_route(JSON_Object *route_object) {
  assert(route_object != NULL);

  xps_config_route_t *route = calloc(1, sizeof(xps_config_route_t));
  if (route == NULL) {
    logger(LOG_ERROR, "parse_route()", "calloc() failed for 'route'");
    return NULL;
  }
  vec_init(&route->index);
  vec_init(&route->upstreams);

  route->req_path = json_object_get_string(route_object, "req_path");
  route->type = json_object_get_string(route_object, "type");
  if (route->req_path == NULL || route->req_path[0] != '/' || route->type == NULL) {
    logger(LOG_ERROR, "parse_route()", "route needs 'req_path' (starting with '/') and 'type'");
    route_destroy(route);
    return NULL;
  }

  if (strcmp(route->type, "file_serve") == 0) {
    route->dir_path = json_object_get_string(route_object, "dir_path");
    if (route->dir_path == NULL) {
      logger(LOG_ERROR, "parse_route()", "file_serve route needs 'dir_path'");
      route_destroy(route);
      return NULL;
    }
    if (parse_string_array(route_object, "index", &route->index, false) != OK) {
      route_destroy(route);
      return NULL;
    }
  } else if (strcmp(route->type, "reverse_proxy") == 0) {
    if (parse_string_array(route_object, "upstreams", &route->upstreams, true) != OK ||
        route->upstreams.length == 0) {
      logger(LOG_ERROR, "parse_route()", "reverse_proxy route needs a non-empty 'upstreams'");
      route_destroy(route);
      return NULL;
    }
  } else if (strcmp(route->type, "redirect") == 0) {
    route->redirect_url = json_object_get_string(route_object, "redirect_url");
    route->http_status_code = 302;
    if (json_object_get_value(route_object, "http_status_code") != NULL)
      route->http_status_code = (u_int)json_object_get_number(route_object, "http_status_code");
    if (route->redirect_url == NULL || route->http_status_code < 300 ||
        route->http_status_code > 399) {
      logger(LOG_ERROR, "parse_route()", "redirect route needs 'redirect_url' and a 3xx status");
      route_destroy(route);
      return NULL;
    }
  } else {
    logger(LOG_ERROR, "parse_route()", "unknown route type '%s'", route->type);
    route_destroy(route);
    return NULL;
  }

  return route;
}

static xps_config_server_t *parse_server(JSON_Object *server_object) {
  assert(server_object != NULL);

  xps_config_server_t *server = calloc(1, sizeof(xps_config_server_t));
  if (server == NULL) {
    logger(LOG_ERROR, "parse_server()", "calloc() failed for 'server'");
    return NULL;
  }
  vec_init(&server->listeners);
  vec_init(&server->hostnames);
  vec_init(&server->routes);

  // listeners (required)
  JSON_Array *listeners = json_object_get_array(server_object, "listeners");
  if (listeners == NULL || json_array_get_count(listeners) == 0) {
    logger(LOG_ERROR, "parse_server()", "server needs a non-empty 'listeners' array");
    server_destroy(server);
    return NULL;
  }
  for (size_t i = 0; i < json_array_get_count(listeners); i++) {
    JSON_Object *listener_object = json_array_get_object(listeners, i);
    xps_config_listener_t *listener = listener_object ? parse_listener(listener_object) : NULL;
    if (listener == NULL || vec_push(&server->listeners, listener) != 0) {
      free(listener);
      server_destroy(server);
      return NULL;
    }
  }

  // hostnames (optional: none means "match any host")
  if (parse_string_array(server_object, "hostnames", &server->hostnames, false) != OK) {
    server_destroy(server);
    return NULL;
  }

  // routes (optional)
  JSON_Array *routes = json_object_get_array(server_object, "routes");
  if (routes != NULL) {
    for (size_t i = 0; i < json_array_get_count(routes); i++) {
      JSON_Object *route_object = json_array_get_object(routes, i);
      xps_config_route_t *route = route_object ? parse_route(route_object) : NULL;
      if (route == NULL || vec_push(&server->routes, route) != 0) {
        route_destroy(route);
        server_destroy(server);
        return NULL;
      }
    }
  }

  return server;
}

// Collects every distinct host:port of every server into config->_all_listeners
static int parse_all_listener(xps_config_t *config) {
  for (int i = 0; i < config->servers.length; i++) {
    xps_config_server_t *server = config->servers.data[i];
    for (int j = 0; j < server->listeners.length; j++) {
      xps_config_listener_t *listener = server->listeners.data[j];

      bool exists = false;
      for (int k = 0; k < config->_all_listeners.length; k++) {
        xps_config_listener_t *known = config->_all_listeners.data[k];
        if (known->port == listener->port && strcmp(known->host, listener->host) == 0) {
          exists = true;
          break;
        }
      }
      if (!exists && vec_push(&config->_all_listeners, listener) != 0)
        return E_FAIL;
    }
  }
  return OK;
}

xps_config_t *xps_config_create(const char *config_path) {
  assert(config_path != NULL);

  xps_config_t *config = calloc(1, sizeof(xps_config_t));
  if (config == NULL) {
    logger(LOG_ERROR, "xps_config_create()", "calloc() failed for 'config'");
    return NULL;
  }
  vec_init(&config->servers);
  vec_init(&config->_all_listeners);
  config->config_path = str_create(config_path);

  config->_config_json = json_parse_file(config_path);
  if (config->_config_json == NULL) {
    logger(LOG_ERROR, "xps_config_create()", "failed to parse JSON file %s", config_path);
    xps_config_destroy(config);
    return NULL;
  }

  JSON_Object *root_object = json_value_get_object(config->_config_json);
  if (root_object == NULL) {
    logger(LOG_ERROR, "xps_config_create()", "config root must be a JSON object");
    xps_config_destroy(config);
    return NULL;
  }

  config->server_name = json_object_get_string(root_object, "server_name");
  if (config->server_name == NULL)
    config->server_name = "eXpServer";

  JSON_Array *servers = json_object_get_array(root_object, "servers");
  if (servers == NULL || json_array_get_count(servers) == 0) {
    logger(LOG_ERROR, "xps_config_create()", "config needs a non-empty 'servers' array");
    xps_config_destroy(config);
    return NULL;
  }

  for (size_t i = 0; i < json_array_get_count(servers); i++) {
    JSON_Object *server_object = json_array_get_object(servers, i);
    xps_config_server_t *server = server_object ? parse_server(server_object) : NULL;
    if (server == NULL || vec_push(&config->servers, server) != 0) {
      logger(LOG_ERROR, "xps_config_create()", "failed to parse server %lu", (unsigned long)i);
      server_destroy(server);
      xps_config_destroy(config);
      return NULL;
    }
  }

  if (parse_all_listener(config) != OK) {
    xps_config_destroy(config);
    return NULL;
  }

  logger(LOG_INFO, "xps_config_create()", "loaded %d server(s), %d listener(s) from %s",
         config->servers.length, config->_all_listeners.length, config_path);
  return config;
}

void xps_config_destroy(xps_config_t *config) {
  assert(config != NULL);

  for (int i = 0; i < config->servers.length; i++)
    server_destroy(config->servers.data[i]);
  vec_deinit(&config->servers);
  vec_deinit(&config->_all_listeners); // entries are owned by the servers

  if (config->_config_json != NULL)
    json_value_free(config->_config_json); // also frees every string the structs pointed at

  free((char *)config->config_path);
  free(config);
}

/* ---------------------------------------------------------------------------------------------
 * lookup
 * ------------------------------------------------------------------------------------------- */

// Copies the host part of a Host header ("example.com:8080" -> "example.com")
static void host_without_port(const char *host_header, char *out, size_t out_size) {
  size_t n = 0;
  if (host_header[0] == '[') { // IPv6 literal: [::1]:8080
    while (host_header[n] != '\0' && n + 1 < out_size) {
      out[n] = host_header[n];
      n++;
      if (host_header[n - 1] == ']')
        break;
    }
  } else {
    while (host_header[n] != '\0' && host_header[n] != ':' && n + 1 < out_size) {
      out[n] = host_header[n];
      n++;
    }
  }
  out[n] = '\0';
}

// Resolves the file for a file_serve route. lookup->file_path stays NULL if nothing can be served.
static void lookup_file_serve(xps_config_route_t *route, const char *pathname,
                              xps_config_lookup_t *lookup) {
  char *root = realpath(route->dir_path, NULL);
  char *resource_path = path_join(route->dir_path, pathname);
  char *resolved = resource_path ? realpath(resource_path, NULL) : NULL;
  free(resource_path);

  if (root == NULL || resolved == NULL) {
    logger(LOG_WARNING, "xps_config_lookup()", "realpath() failed for %s, %s", route->dir_path, pathname);
    free(root);
    free(resolved);
    return;
  }

  // The resolved path (symlinks and ".." removed) has to stay inside the route's directory
  size_t root_len = strlen(root);
  bool inside = strncmp(resolved, root, root_len) == 0 &&
                (resolved[root_len] == '\0' || resolved[root_len] == '/' || root_len == 1);
  free(root);
  if (!inside) {
    logger(LOG_WARNING, "xps_config_lookup()", "path escapes dir_path: %s", resolved);
    free(resolved);
    return;
  }

  if (is_file(resolved)) {
    lookup->file_path = resolved;
    return;
  }

  if (is_dir(resolved)) {
    // Serve the first index file that exists
    for (int i = 0; i < route->index.length; i++) {
      char *index_path = path_join(resolved, route->index.data[i]);
      if (index_path != NULL && is_file(index_path)) {
        lookup->file_path = index_path;
        free(resolved);
        return;
      }
      free(index_path);
    }
    // No index file: hand the directory over to directory browsing (xps_directory_browsing)
    lookup->dir_path = resolved; // freed by xps_config_lookup_destroy()
    return;
  }

  free(resolved);
}

xps_config_lookup_t *xps_config_lookup(xps_config_t *config, xps_http_req_t *http_req,
                                       xps_connection_t *client, int *error) {
  assert(config != NULL);
  assert(http_req != NULL);
  assert(client != NULL);
  assert(error != NULL);

  *error = E_FAIL;

  const char *pathname = http_req->pathname[0] == '\0' ? "/" : http_req->pathname;

  // Host: header first, absolute-form request target as fallback
  char host_name[256] = "";
  const char *host_header = xps_http_get_header(&http_req->headers, "Host");
  if (host_header != NULL)
    host_without_port(host_header, host_name, sizeof(host_name));
  else if (http_req->host != NULL)
    snprintf(host_name, sizeof(host_name), "%s", http_req->host);

  // Step 1: find the server block matching the client's listener and the requested host
  xps_config_server_t *server = NULL;
  for (int i = 0; i < config->servers.length && server == NULL; i++) {
    xps_config_server_t *curr = config->servers.data[i];

    bool has_matching_listener = false;
    for (int j = 0; j < curr->listeners.length; j++) {
      xps_config_listener_t *l = curr->listeners.data[j];
      if (l->port == client->listener->port && strcmp(l->host, client->listener->host) == 0) {
        has_matching_listener = true;
        break;
      }
    }
    if (!has_matching_listener)
      continue;

    // no hostnames configured = matches every host
    bool has_matching_hostname = curr->hostnames.length == 0;
    for (int j = 0; j < curr->hostnames.length && !has_matching_hostname; j++) {
      if (strcasecmp(host_name, curr->hostnames.data[j]) == 0)
        has_matching_hostname = true;
    }

    if (has_matching_hostname)
      server = curr;
  }

  if (server == NULL) {
    *error = E_NOTFOUND;
    return NULL;
  }

  // Step 2: longest-prefix route match ("/api" beats "/" for "/api/users", "/ap" only gets "/")
  xps_config_route_t *route = NULL;
  int best_match_len = -1; // signed on purpose: a size_t -1 would never be beaten
  for (int i = 0; i < server->routes.length; i++) {
    xps_config_route_t *current_route = server->routes.data[i];
    int route_path_len = (int)strlen(current_route->req_path);

    if (str_starts_with(pathname, current_route->req_path) && route_path_len > best_match_len) {
      route = current_route;
      best_match_len = route_path_len;
    }
  }

  if (route == NULL) {
    *error = E_NOTFOUND;
    return NULL;
  }

  // Step 3: fill the lookup
  xps_config_lookup_t *lookup = calloc(1, sizeof(xps_config_lookup_t));
  if (lookup == NULL) {
    logger(LOG_ERROR, "xps_config_lookup()", "calloc() failed for 'lookup'");
    return NULL;
  }
  vec_init(&lookup->ip_whitelist);
  vec_init(&lookup->ip_blacklist);

  if (strcmp(route->type, "file_serve") == 0) {
    lookup->type = REQ_FILE_SERVE;
    lookup_file_serve(route, pathname, lookup);
  } 
  else if (strcmp(route->type, "reverse_proxy") == 0) {
    lookup->type = REQ_REVERSE_PROXY;
    // first upstream for now (load balancing comes in stage 20)
    lookup->upstream = route->upstreams.length > 0 ? route->upstreams.data[0] : NULL;
  } 
  else if (strcmp(route->type, "redirect") == 0) {
    lookup->type = REQ_REDIRECT;
    lookup->http_status_code = route->http_status_code;
    lookup->redirect_url = route->redirect_url;
  } 
  else {
    lookup->type = REQ_INVALID;
    xps_config_lookup_destroy(lookup, NULL);
    return NULL;
  }

  *error = OK;
  return lookup;
}

void xps_config_lookup_destroy(xps_config_lookup_t *config_lookup, xps_core_t *core) {
  assert(config_lookup != NULL);

  free(config_lookup->file_path);
  free(config_lookup->dir_path);
  vec_deinit(&config_lookup->ip_whitelist);
  vec_deinit(&config_lookup->ip_blacklist);
  free(config_lookup);
}