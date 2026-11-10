#include "xps_http_res.h"

static const char *http_res_status_text(u_int code) {
  switch (code) {
    case HTTP_OK: return "OK";
    case HTTP_CREATED: return "Created";
    case HTTP_MOVED_PERMANENTLY: return "Moved Permanently";
    case HTTP_MOVED_TEMPORARILY: return "Found";
    case HTTP_NOT_MODIFIED: return "Not Modified";
    case HTTP_TEMPORARY_REDIRECT: return "Temporary Redirect";
    case HTTP_PERMANENT_REDIRECT: return "Permanent Redirect";
    case HTTP_BAD_REQUEST: return "Bad Request";
    case HTTP_UNAUTHORIZED: return "Unauthorized";
    case HTTP_FORBIDDEN: return "Forbidden";
    case HTTP_NOT_FOUND: return "Not Found";
    case HTTP_REQUEST_TIME_OUT: return "Request Timeout";
    case HTTP_TOO_MANY_REQUESTS: return "Too Many Requests";
    case HTTP_INTERNAL_SERVER_ERROR: return "Internal Server Error";
    case HTTP_NOT_IMPLEMENTED: return "Not Implemented";
    case HTTP_BAD_GATEWAY: return "Bad Gateway";
    case HTTP_SERVICE_UNAVAILABLE: return "Service Unavailable";
    case HTTP_GATEWAY_TIMEOUT: return "Gateway Timeout";
    case HTTP_HTTP_VERSION_NOT_SUPPORTED: return "HTTP Version Not Supported";
    default: return NULL;
  }
}

xps_http_res_t *xps_http_res_create(xps_core_t *core, u_int code) {
  assert(core != NULL);

  const char *status_text = http_res_status_text(code);
  if (status_text == NULL) {
    logger(LOG_ERROR, "xps_http_res_create()", "unsupported status code %u", code);
    return NULL;
  }

  xps_http_res_t *http_res = malloc(sizeof(xps_http_res_t));
  if (http_res == NULL) {
    logger(LOG_ERROR, "xps_http_res_create()",
           "failed to alloc memory for http_res. malloc() returned NULL");
    return NULL;
  }

  // Init values
  snprintf(http_res->response_line, sizeof(http_res->response_line), "HTTP/1.1 %u %s", code,
           status_text);
  vec_init(&http_res->headers);
  http_res->body = NULL;

  // Date header (IMF-fixdate, always GMT)
  char time_buf[64];
  time_t now = time(NULL);
  struct tm tm_buf;
  if (gmtime_r(&now, &tm_buf) == NULL ||
      strftime(time_buf, sizeof(time_buf), "%a, %d %b %Y %H:%M:%S GMT", &tm_buf) == 0) {
    logger(LOG_ERROR, "xps_http_res_create()", "failed to format Date header");
    xps_http_res_destroy(http_res);
    return NULL;
  }

  if (xps_http_set_header(&http_res->headers, "Date", time_buf) != OK ||
      xps_http_set_header(&http_res->headers, "Server", SERVER_NAME) != OK ||
      xps_http_set_header(&http_res->headers, "Access-Control-Allow-Origin", "*") != OK) {
    logger(LOG_ERROR, "xps_http_res_create()", "xps_http_set_header() failed");
    xps_http_res_destroy(http_res);
    return NULL;
  }

  logger(LOG_DEBUG, "xps_http_res_create()", "created response: %s", http_res->response_line);

  return http_res;
}

void xps_http_res_destroy(xps_http_res_t *http_res) {
  assert(http_res != NULL);

  for (int i = 0; i < http_res->headers.length; i++) {
    xps_keyval_t *header = http_res->headers.data[i];
    if (header == NULL)
      continue;
    free(header->key);
    free(header->val);
    free(header);
  }
  vec_deinit(&http_res->headers);

  if (http_res->body != NULL)
    xps_buffer_destroy(http_res->body);

  free(http_res);

  logger(LOG_DEBUG, "xps_http_res_destroy()", "destroyed response");
}

// Takes ownership of buff
void xps_http_res_set_body(xps_http_res_t *http_res, xps_buffer_t *buff) {
  assert(http_res != NULL);

  if (http_res->body != NULL && http_res->body != buff)
    xps_buffer_destroy(http_res->body);
  http_res->body = buff;
}

xps_buffer_t *xps_http_res_serialize(xps_http_res_t *http_res) {
  assert(http_res != NULL);

  // Serialize headers
  xps_buffer_t *headers_str = xps_http_serialize_headers(&http_res->headers);
  if (headers_str == NULL) {
    logger(LOG_ERROR, "xps_http_res_serialize()", "failed to serialize headers");
    return NULL;
  }

  // response line CRLF headers(each ends in CRLF) CRLF body
  size_t line_len = strlen(http_res->response_line);
  size_t body_len = http_res->body != NULL ? http_res->body->len : 0;
  size_t final_len = line_len + 2 + headers_str->len + 2 + body_len;

  xps_buffer_t *buff = xps_buffer_create(final_len, final_len, NULL);
  if (buff == NULL) {
    logger(LOG_ERROR, "xps_http_res_serialize()", "failed to create buffer instance");
    xps_buffer_destroy(headers_str);
    return NULL;
  }

  u_char *p = buff->data;
  memcpy(p, http_res->response_line, line_len);
  p += line_len;
  memcpy(p, "\r\n", 2);
  p += 2;
  memcpy(p, headers_str->data, headers_str->len);
  p += headers_str->len;
  memcpy(p, "\r\n", 2);
  p += 2;
  if (body_len > 0)
    memcpy(p, http_res->body->data, body_len);

  xps_buffer_destroy(headers_str);

  logger(LOG_DEBUG, "xps_http_res_serialize()", "serialized response (%lu bytes)",
         (unsigned long)final_len);

  return buff;
}