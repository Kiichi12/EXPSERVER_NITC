#include "xps_http_req.h"

static int http_process_request_line(xps_http_req_t *http_req, xps_buffer_t *buff) {
  int error = xps_http_parse_request_line(http_req, buff);
  if (error != OK)
    return error;

  http_req->request_line = str_from_ptrs((char *)http_req->request_line_start, (char *)http_req->request_line_end);
  http_req->method = str_from_ptrs((char *)http_req->method_start, (char *)http_req->method_end);
  http_req->uri = str_from_ptrs((char *)http_req->uri_start, (char *)http_req->uri_end);
  http_req->schema = str_from_ptrs((char *)http_req->schema_start, (char *)http_req->schema_end);
  http_req->host = str_from_ptrs((char *)http_req->host_start, (char *)http_req->host_end);
  http_req->path = str_from_ptrs((char *)http_req->path_start, (char *)http_req->path_end);
  http_req->pathname = str_from_ptrs((char *)http_req->pathname_start, (char *)http_req->pathname_end);
  http_req->http_version = str_from_ptrs((char *)http_req->http_major, (char *)http_req->http_minor);

  if (!http_req->request_line || !http_req->method || !http_req->uri || !http_req->schema ||
      !http_req->host || !http_req->path || !http_req->pathname || !http_req->http_version)
    return E_FAIL;

  http_req->port = -1;
  char *port_str = str_from_ptrs((char *)http_req->port_start, (char *)http_req->port_end);
  if (port_str == NULL)
    return E_FAIL;

  if (port_str[0] == '\0') {
    http_req->port = (strcmp(http_req->schema, "https") == 0) ? 443 : 80;
  } else {
    long p = strtol(port_str, NULL, 10);
    if (p < 1 || p > 65535) {
      free(port_str);
      return E_FAIL;
    }
    http_req->port = (int)p;
  }
  free(port_str);

  logger(LOG_DEBUG, "http_process_request_line()", "REQUEST LINE: %s", http_req->request_line);
  logger(LOG_DEBUG, "http_process_request_line()", "METHOD: %s", http_req->method);
  logger(LOG_DEBUG, "http_process_request_line()", "URI: %s", http_req->uri);
  logger(LOG_DEBUG, "http_process_request_line()", "SCHEMA: %s", http_req->schema);
  logger(LOG_DEBUG, "http_process_request_line()", "HOST: %s", http_req->host);
  logger(LOG_DEBUG, "http_process_request_line()", "PORT: %d", http_req->port);
  logger(LOG_DEBUG, "http_process_request_line()", "PATH: %s", http_req->path);
  logger(LOG_DEBUG, "http_process_request_line()", "PATHNAME: %s", http_req->pathname);
  logger(LOG_DEBUG, "http_process_request_line()", "HTTP VERSION: %s", http_req->http_version);

  return OK;
}

static int http_process_headers(xps_http_req_t *http_req, xps_buffer_t *buff) {
  assert(http_req != NULL);
  assert(buff != NULL);

  int error;
  while (1) {
    error = xps_http_parse_header_line(http_req, buff);
    if (error == E_FAIL || error == E_AGAIN)
      break;

    // OK or E_NEXT: a header may be ready (none if the section was empty)
    if (http_req->header_key_start != NULL) {
      xps_keyval_t *header = malloc(sizeof(xps_keyval_t));
      if (header == NULL) {
        logger(LOG_ERROR, "http_process_headers()", "malloc() failed for 'header'");
        error = E_FAIL;
        break;
      }
      header->key = str_from_ptrs((char *)http_req->header_key_start, (char *)http_req->header_key_end);
      header->val = str_from_ptrs((char *)http_req->header_val_start, (char *)http_req->header_val_end);
      if (header->key == NULL || header->val == NULL) {
        free(header->key);
        free(header->val);
        free(header);
        error = E_FAIL;
        break;
      }
      vec_push(&http_req->headers, header);

      http_req->header_key_start = http_req->header_key_end = NULL;
      http_req->header_val_start = http_req->header_val_end = NULL;
    }

    if (error == OK) {
      logger(LOG_DEBUG, "http_process_headers()", "HEADERS");
      for (int i = 0; i < http_req->headers.length; i++) {
        xps_keyval_t *header = http_req->headers.data[i];
        logger(LOG_DEBUG, "http_process_headers()", "%s: %s", header->key, header->val);
      }
      return OK;
    }
    // E_NEXT: continue with next header
  }

  // error: free every header parsed so far
  for (int i = 0; i < http_req->headers.length; i++) {
    xps_keyval_t *header = http_req->headers.data[i];
    free(header->key);
    free(header->val);
    free(header);
  }
  vec_clear(&http_req->headers);
  return error;
}

xps_buffer_t *xps_http_req_serialize(xps_http_req_t *http_req) {
  assert(http_req != NULL);

  xps_buffer_t *headers_str = xps_http_serialize_headers(&http_req->headers);
  if (headers_str == NULL)
    return NULL;

  size_t rl_len = strlen(http_req->request_line);
  // request line + CRLF + headers (each ends in CRLF) + CRLF
  size_t final_len = rl_len + 2 + headers_str->len + 2;

  u_char *data = malloc(final_len + 1);
  if (data == NULL) {
    logger(LOG_ERROR, "xps_http_req_serialize()", "malloc() failed for 'data'");
    xps_buffer_destroy(headers_str);
    return NULL;
  }

  u_char *p = data;
  memcpy(p, http_req->request_line, rl_len);
  p += rl_len;
  memcpy(p, "\r\n", 2);
  p += 2;
  memcpy(p, headers_str->data, headers_str->len);
  p += headers_str->len;
  memcpy(p, "\r\n", 2);
  p += 2;
  *p = '\0';

  xps_buffer_destroy(headers_str);

  xps_buffer_t *buff = xps_buffer_create(final_len + 1, final_len, data);
  if (buff == NULL) {
    free(data);
    return NULL;
  }
  return buff;
}

xps_http_req_t *xps_http_req_create(xps_core_t *core, xps_buffer_t *buff, int *error) {
  assert(buff != NULL);
  assert(error != NULL);

  *error = E_FAIL;

  xps_http_req_t *http_req = malloc(sizeof(xps_http_req_t));
  if (http_req == NULL) {
    logger(LOG_ERROR, "xps_http_req_create()", "malloc() failed for 'http_req'");
    return NULL;
  }
  memset(http_req, 0, sizeof(xps_http_req_t));
  vec_init(&http_req->headers);
  http_req->parser_state = RL_START;

  int err = http_process_request_line(http_req, buff);
  if (err != OK) {
    *error = err;
    xps_http_req_destroy(core, http_req);
    return NULL;
  }

  err = http_process_headers(http_req, buff);
  if (err != OK) {
    *error = err;
    xps_http_req_destroy(core, http_req);
    return NULL;
  }

  // Header length
  http_req->header_len = (size_t)(buff->pos - buff->data);

  // Body length is retrieved from header Content-Length
  http_req->body_len = 0;
  const char *body_len_str = xps_http_get_header(&http_req->headers, "Content-Length");
  if (body_len_str != NULL)
    http_req->body_len = (size_t)strtoul(body_len_str, NULL, 10);

  *error = OK;
  return http_req;
}

void xps_http_req_destroy(xps_core_t *core, xps_http_req_t *http_req) {
  assert(http_req != NULL);

  free(http_req->request_line);
  free(http_req->method);
  free(http_req->uri);
  free(http_req->schema);
  free(http_req->host);
  free(http_req->path);
  free(http_req->pathname);
  free(http_req->http_version);

  for (int i = 0; i < http_req->headers.length; i++) {
    xps_keyval_t *header = http_req->headers.data[i];
    if (header == NULL)
      continue;
    free(header->key);
    free(header->val);
    free(header);
  }
  vec_deinit(&http_req->headers);

  free(http_req);
}