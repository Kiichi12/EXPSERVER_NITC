#include "xps_http.h"

static bool http_strcmp(u_char *str, const char *method, size_t length) {
  return strncmp((const char *)str, method, length) == 0;
}

static bool http_match_method(u_char *start, size_t len, xps_http_method_t *out) {
  if (len == 3 && http_strcmp(start, "GET", 3))
    *out = HTTP_GET;
  else if (len == 3 && http_strcmp(start, "PUT", 3))
    *out = HTTP_PUT;
  else if (len == 4 && http_strcmp(start, "HEAD", 4))
    *out = HTTP_HEAD;
  else if (len == 4 && http_strcmp(start, "POST", 4))
    *out = HTTP_POST;
  else if (len == 5 && http_strcmp(start, "TRACE", 5))
    *out = HTTP_TRACE;
  else if (len == 6 && http_strcmp(start, "DELETE", 6))
    *out = HTTP_DELETE;
  else if (len == 7 && http_strcmp(start, "OPTIONS", 7))
    *out = HTTP_OPTIONS;
  else if (len == 7 && http_strcmp(start, "CONNECT", 7))
    *out = HTTP_CONNECT;
  else
    return false;
  return true;
}

int xps_http_parse_request_line(xps_http_req_t *http_req, xps_buffer_t *buff) {
  assert(http_req != NULL);
  assert(buff != NULL);

  xps_http_parser_state_t parser_state = http_req->parser_state;
  u_char *end = buff->data + buff->len;
  u_char *p_ch;

  for (p_ch = buff->pos; p_ch < end; p_ch++) {
    char ch = *p_ch;

    switch (parser_state) {
      case RL_START:
        // ignore leading CR / LF
        if (ch == CR || ch == LF)
          break;
        if (ch < 'A' || ch > 'Z')
          return E_FAIL;
        http_req->request_line_start = p_ch;
        http_req->method_start = p_ch;
        parser_state = RL_METHOD;
        break;

      case RL_METHOD:
        if (ch == ' ') {
          size_t method_len = p_ch - http_req->method_start;
          if (!http_match_method(http_req->method_start, method_len, &http_req->method_n))
            return E_FAIL;
          http_req->method_end = p_ch;
          parser_state = RL_SP_AFTER_METHOD;
        } else if (ch < 'A' || ch > 'Z') {
          return E_FAIL;
        }
        break;

      case RL_SP_AFTER_METHOD:
        if (ch == ' ')
          break;
        if (ch == '/') {
          // origin-form: no schema, host or port
          http_req->schema_start = http_req->schema_end = p_ch;
          http_req->host_start = http_req->host_end = p_ch;
          http_req->port_start = http_req->port_end = p_ch;
          http_req->uri_start = p_ch;
          http_req->path_start = p_ch;
          http_req->pathname_start = p_ch;
          parser_state = RL_PATH;
        } else {
          char c = ch | 0x20; // lower case
          if (c >= 'a' && c <= 'z') {
            http_req->schema_start = p_ch;
            http_req->uri_start = p_ch;
            parser_state = RL_SCHEMA;
          } else {
            return E_FAIL;
          }
        }
        break;

      case RL_SCHEMA:
        if (ch >= 'a' && ch <= 'z')
          break;
        if (ch == ':') {
          http_req->schema_end = p_ch;
          parser_state = RL_SCHEMA_SLASH;
          break;
        }
        return E_FAIL;

      case RL_SCHEMA_SLASH:
        if (ch == '/') {
          parser_state = RL_SCHEMA_SLASH_SLASH;
          break;
        }
        return E_FAIL;

      case RL_SCHEMA_SLASH_SLASH:
        if (ch == '/') {
          http_req->host_start = p_ch + 1;
          parser_state = RL_HOST;
          break;
        }
        return E_FAIL;

      case RL_HOST:
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
            ch == '-' || ch == '.')
          break;
        if (ch == ':') {
          http_req->host_end = p_ch;
          http_req->port_start = p_ch + 1;
          parser_state = RL_PORT;
        } else if (ch == '/') {
          http_req->host_end = p_ch;
          http_req->port_start = http_req->port_end = p_ch;
          http_req->path_start = p_ch;
          http_req->pathname_start = p_ch;
          parser_state = RL_PATH;
        } else if (ch == ' ') {
          http_req->host_end = p_ch;
          http_req->uri_end = p_ch;
          http_req->port_start = http_req->port_end = p_ch;
          http_req->path_start = http_req->path_end = p_ch;
          http_req->pathname_start = http_req->pathname_end = p_ch;
          parser_state = RL_VERSION_START;
        } else {
          return E_FAIL;
        }
        break;

      case RL_PORT:
        if (ch >= '0' && ch <= '9')
          break;
        if (ch == '/') {
          http_req->port_end = p_ch;
          http_req->path_start = p_ch;
          http_req->pathname_start = p_ch;
          parser_state = RL_PATH;
        } else if (ch == ' ') {
          http_req->port_end = p_ch;
          http_req->uri_end = p_ch;
          http_req->path_start = http_req->path_end = p_ch;
          http_req->pathname_start = http_req->pathname_end = p_ch;
          parser_state = RL_VERSION_START;
        } else {
          return E_FAIL;
        }
        break;

      case RL_PATH:
        if (ch == ' ') {
          http_req->path_end = p_ch;
          http_req->pathname_end = p_ch;
          http_req->uri_end = p_ch;
          parser_state = RL_VERSION_START;
        } else if (ch == '?' || ch == '&' || ch == '=' || ch == '#') {
          // pathname ends here, the rest is the query part of 'path'
          http_req->pathname_end = p_ch;
          parser_state = RL_PATHNAME;
        } else if (ch == CR || ch == LF || (u_char)ch < 0x20) {
          return E_FAIL;
        }
        break;

      case RL_PATHNAME:
        if (ch == ' ') {
          http_req->path_end = p_ch;
          http_req->uri_end = p_ch;
          parser_state = RL_VERSION_START;
        } else if (ch == CR || ch == LF || (u_char)ch < 0x20) {
          return E_FAIL;
        }
        break;

      case RL_VERSION_START:
        if (ch == ' ')
          break;
        if (ch == 'H') {
          parser_state = RL_VERSION_H;
          break;
        }
        return E_FAIL;

      case RL_VERSION_H:
        if (ch != 'T')
          return E_FAIL;
        parser_state = RL_VERSION_HT;
        break;

      case RL_VERSION_HT:
        if (ch != 'T')
          return E_FAIL;
        parser_state = RL_VERSION_HTT;
        break;

      case RL_VERSION_HTT:
        if (ch != 'P')
          return E_FAIL;
        parser_state = RL_VERSION_HTTP;
        break;

      case RL_VERSION_HTTP:
        if (ch != '/')
          return E_FAIL;
        parser_state = RL_VERSION_HTTP_SLASH;
        break;

      case RL_VERSION_HTTP_SLASH:
        if (ch != '1')
          return E_FAIL;
        http_req->http_major = p_ch;
        parser_state = RL_VERSION_MAJOR;
        break;

      case RL_VERSION_MAJOR:
        if (ch != '.')
          return E_FAIL;
        parser_state = RL_VERSION_DOT;
        break;

      case RL_VERSION_DOT:
        if (ch != '0' && ch != '1')
          return E_FAIL;
        parser_state = RL_VERSION_MINOR;
        break;

      case RL_VERSION_MINOR:
        // http_minor marks the END of the version, so that
        // str_from_ptrs(http_major, http_minor) gives e.g. "1.1"
        if (ch == CR) {
          http_req->http_minor = p_ch;
          http_req->request_line_end = p_ch;
          parser_state = RL_CR;
        } else if (ch == LF) {
          http_req->http_minor = p_ch;
          http_req->request_line_end = p_ch;
          http_req->parser_state = H_START;
          buff->pos = p_ch + 1;
          return OK;
        } else {
          return E_FAIL;
        }
        break;

      case RL_CR:
        if (ch != LF)
          return E_FAIL;
        http_req->parser_state = H_START;
        buff->pos = p_ch + 1;
        return OK;

      default:
        return E_FAIL;
    }
  }

  http_req->parser_state = parser_state;
  buff->pos = p_ch;
  return E_AGAIN;
}

static bool http_is_name_char(char ch) {
  return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') ||
         ch == '-' || ch == '_';
}

int xps_http_parse_header_line(xps_http_req_t *http_req, xps_buffer_t *buff) {
  assert(http_req != NULL);
  assert(buff != NULL);

  xps_http_parser_state_t parser_state = http_req->parser_state;
  u_char *end = buff->data + buff->len;
  u_char *p_ch;

  for (p_ch = buff->pos; p_ch < end; p_ch++) {
    char ch = *p_ch;

    switch (parser_state) {
      case H_START:
        if (ch == CR) {
          // blank line: end of header section
          parser_state = H_LF_CR;
        } else if (ch == LF) {
          http_req->parser_state = H_START;
          buff->pos = p_ch + 1;
          return OK;
        } else if (http_is_name_char(ch)) {
          http_req->header_key_start = p_ch;
          parser_state = H_NAME;
        } else {
          return E_FAIL;
        }
        break;

      case H_NAME:
        if (http_is_name_char(ch))
          break;
        if (ch == ':') {
          http_req->header_key_end = p_ch;
          parser_state = H_COLON;
          break;
        }
        return E_FAIL;

      case H_COLON: // character right after ':'
        if (ch == ' ' || ch == '\t') {
          parser_state = H_SP_AFTER_COLON;
        } else if (ch == CR || ch == LF) {
          http_req->header_val_start = http_req->header_val_end = p_ch;
          parser_state = (ch == CR) ? H_CR : H_LF;
        } else {
          http_req->header_val_start = p_ch;
          parser_state = H_VAL;
        }
        break;

      case H_SP_AFTER_COLON:
        if (ch == ' ' || ch == '\t')
          break;
        if (ch == CR || ch == LF) {
          http_req->header_val_start = http_req->header_val_end = p_ch;
          parser_state = (ch == CR) ? H_CR : H_LF;
        } else {
          http_req->header_val_start = p_ch;
          parser_state = H_VAL;
        }
        break;

      case H_VAL:
        if (ch == CR || ch == LF) {
          http_req->header_val_end = p_ch;
          parser_state = (ch == CR) ? H_CR : H_LF;
        }
        break;

      case H_CR:
        if (ch != LF)
          return E_FAIL;
        parser_state = H_LF;
        break;

      case H_LF:
        if (ch == LF) {
          http_req->parser_state = H_START;
          buff->pos = p_ch + 1;
          return OK; // header section done
        } else if (ch == CR) {
          parser_state = H_LF_CR;
        } else {
          // start of next header: do not consume it
          http_req->parser_state = H_START;
          buff->pos = p_ch;
          return E_NEXT; // This header is done, repeat for the next
        }
        break;

      case H_LF_CR:
        if (ch == LF) {
          http_req->parser_state = H_START;
          buff->pos = p_ch + 1;
          return OK; // header section done
        }
        return E_FAIL;

      default:
        return E_FAIL;
    }
  }

  http_req->parser_state = parser_state;
  buff->pos = p_ch;
  return E_AGAIN;
}

const char *xps_http_get_header(vec_void_t *headers, const char *key) {
  assert(headers != NULL);
  assert(key != NULL);

  for (int i = 0; i < headers->length; i++) {
    xps_keyval_t *header = headers->data[i];
    // header names are case insensitive
    if (strcasecmp(header->key, key) == 0)
      return header->val;
  }
  return NULL;
}

xps_buffer_t *xps_http_serialize_headers(vec_void_t *headers) {
  assert(headers != NULL);

  // first pass: compute required size ("key: val\r\n" -> +4, plus terminator)
  size_t total = 0;
  for (int i = 0; i < headers->length; i++) {
    xps_keyval_t *header = headers->data[i];
    total += strlen(header->key) + strlen(header->val) + 4;
  }

  u_char *data = malloc(total + 1);
  if (data == NULL) {
    logger(LOG_ERROR, "xps_http_serialize_headers()", "malloc() failed for 'data'");
    return NULL;
  }
  data[0] = '\0';

  // second pass: write
  size_t off = 0;
  for (int i = 0; i < headers->length; i++) {
    xps_keyval_t *header = headers->data[i];
    off += sprintf((char *)data + off, "%s: %s\r\n", header->key, header->val);
  }

  xps_buffer_t *buff = xps_buffer_create(total + 1, off, data);
  if (buff == NULL) {
    free(data);
    return NULL;
  }
  return buff;
}