#include "xps_session.h"

// Pipe handlers
void client_source_handler(void *ptr);
void client_source_close_handler(void *ptr);
void client_sink_handler(void *ptr);
void client_sink_close_handler(void *ptr);
void upstream_source_handler(void *ptr);
void upstream_source_close_handler(void *ptr);
void upstream_sink_handler(void *ptr);
void upstream_sink_close_handler(void *ptr);
void upstream_error_res(xps_session_t *session);
void file_sink_handler(void *ptr);
void file_sink_close_handler(void *ptr);

// Helpers
void set_to_client_buff(xps_session_t *session, xps_buffer_t *buff);
void set_from_client_buff(xps_session_t *session, xps_buffer_t *buff);
void session_check_destroy(xps_session_t *session);
void session_process_request(xps_session_t *session);
static void session_send_error(xps_session_t *session, u_int code);
static xps_buffer_t *session_read_sink(xps_pipe_sink_t *sink);

xps_session_t *xps_session_create(xps_core_t *core, xps_connection_t *client) {
  assert(core != NULL);
  assert(client != NULL);

  // Alloc memory for session instance
  xps_session_t *session = malloc(sizeof(xps_session_t));
  if (session == NULL) {
    logger(LOG_ERROR, "xps_session_create()", "malloc() failed for 'session'");
    return NULL;
  }

  // Init every field first so xps_session_destroy() is safe on all failure paths below
  session->core = core;
  session->client = client;
  session->upstream = NULL;
  session->upstream_connected = false;
  session->upstream_error_res_set = false;
  session->upstream_write_bytes = 0;
  session->file = NULL;
  session->http_req = NULL;
  session->request_handled = false;
  session->to_client_buff = NULL;
  session->from_client_buff = NULL;

  session->client_source =
      xps_pipe_source_create(session, client_source_handler, client_source_close_handler);
  session->client_sink =
      xps_pipe_sink_create(session, client_sink_handler, client_sink_close_handler);
  session->upstream_source =
      xps_pipe_source_create(session, upstream_source_handler, upstream_source_close_handler);
  session->upstream_sink =
      xps_pipe_sink_create(session, upstream_sink_handler, upstream_sink_close_handler);
  session->file_sink = xps_pipe_sink_create(session, file_sink_handler, file_sink_close_handler);

  if (!(session->client_source && session->client_sink && session->upstream_source &&
        session->upstream_sink && session->file_sink)) {
    logger(LOG_ERROR, "xps_session_create()", "failed to create some sources/sinks");
    xps_session_destroy(session);
    return NULL;
  }

  session->client_sink->ready = true;
  session->upstream_sink->ready = true;
  session->file_sink->ready = true;

  // Add current session to core->sessions
  vec_push(&core->sessions, session);

  // client.source -> session.client_sink (request), session.client_source -> client.sink (response)
  xps_pipe_t *req_pipe =
      xps_pipe_create(core, DEFAULT_PIPE_BUFF_THRESH, client->source, session->client_sink);
  xps_pipe_t *res_pipe =
      xps_pipe_create(core, DEFAULT_PIPE_BUFF_THRESH, session->client_source, client->sink);
  if (req_pipe == NULL || res_pipe == NULL) {
    logger(LOG_ERROR, "xps_session_create()", "failed to create client pipes");
    // Detaches our ends. The caller destroys the client connection, which leaves any pipe that
    // was created with no ends, and handle_pipes() then destroys it.
    xps_session_destroy(session);
    return NULL;
  }

  logger(LOG_DEBUG, "xps_session_create()", "created session");

  return session;
}

void xps_session_destroy(xps_session_t *session) {
  assert(session != NULL);

  // Sources/sinks detach themselves from their pipes. Connections and the file are NOT destroyed
  // here: they close themselves through their pipes' close handlers.
  if (session->client_source != NULL)
    xps_pipe_source_destroy(session->client_source);
  if (session->client_sink != NULL)
    xps_pipe_sink_destroy(session->client_sink);
  if (session->upstream_source != NULL)
    xps_pipe_source_destroy(session->upstream_source);
  if (session->upstream_sink != NULL)
    xps_pipe_sink_destroy(session->upstream_sink);
  if (session->file_sink != NULL)
    xps_pipe_sink_destroy(session->file_sink);

  if (session->to_client_buff != NULL)
    xps_buffer_destroy(session->to_client_buff);
  if (session->from_client_buff != NULL)
    xps_buffer_destroy(session->from_client_buff);

  if (session->http_req != NULL)
    xps_http_req_destroy(session->core, session->http_req);

  // Set NULL in core's list of sessions
  for (int i = 0; i < session->core->sessions.length; i++) {
    xps_session_t *curr = session->core->sessions.data[i];
    if (curr == session) {
      session->core->sessions.data[i] = NULL;
      session->core->n_null_sessions++;
      break;
    }
  }

  free(session);

  logger(LOG_DEBUG, "xps_session_destroy()", "destroyed session");
}

/* ---------------------------------------------------------------------------------------------
 * client side
 * ------------------------------------------------------------------------------------------- */

// session -> client: writes session->to_client_buff into the pipe
void client_source_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_source_t *source = ptr;
  xps_session_t *session = source->ptr;

  if (session->to_client_buff == NULL) {
    source->ready = false; // nothing to write; do not make the loop spin
    return;
  }

  if (xps_pipe_source_write(source, session->to_client_buff) != OK) {
    logger(LOG_ERROR, "client_source_handler()", "xps_pipe_source_write() failed");
    xps_session_destroy(session);
    return;
  }

  xps_buffer_destroy(session->to_client_buff);
  set_to_client_buff(session, NULL);
  session_check_destroy(session);
}

void client_source_close_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_source_t *source = ptr;
  xps_session_t *session = source->ptr;

  session_check_destroy(session);
}

// client -> session: collects the request head, parses it and answers it
void client_sink_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  xps_buffer_t *buff = session_read_sink(sink);
  if (buff == NULL) {
    logger(LOG_ERROR, "client_sink_handler()", "failed to read from pipe, destroying session");
    xps_session_destroy(session); // otherwise the readable pipe would spin the loop forever
    return;
  }
  size_t read_len = buff->len;

  if (xps_pipe_sink_clear(sink, read_len) != OK)
    logger(LOG_ERROR, "client_sink_handler()", "failed to clear %lu bytes from sink",
           (unsigned long)read_len);

  // A response is already queued: there is no upstream in this stage, so drop anything else the
  // client sends (request body, pipelined requests) instead of parking it forever
  if (session->request_handled) {
    xps_buffer_destroy(buff);
    return;
  }

  // Append to the bytes of the request head received so far
  xps_buffer_t *acc = session->from_client_buff;
  if (acc == NULL) {
    session->from_client_buff = buff;
  } else {
    size_t total = acc->len + read_len;
    xps_buffer_t *merged = total <= DEFAULT_BUFFER_SIZE ? xps_buffer_create(total, total, NULL) : NULL;
    if (merged != NULL) {
      memcpy(merged->data, acc->data, acc->len);
      memcpy(merged->data + acc->len, buff->data, read_len);
    }
    xps_buffer_destroy(buff);
    xps_buffer_destroy(acc);
    session->from_client_buff = merged;

    if (merged == NULL) {
      logger(LOG_WARNING, "client_sink_handler()", "request head too large or out of memory");
      session->request_handled = true;
      session->http_req = NULL;
      session_process_request(session); // http_req == NULL -> 400
      return;
    }
  }

  // The parser keeps no state between calls (xps_http_req_create() starts from RL_START), so
  // always parse the accumulated head from its beginning
  xps_buffer_t *head = session->from_client_buff;
  head->pos = head->data;

  int error;
  xps_http_req_t *http_req = xps_http_req_create(session->core, head, &error);
  if (http_req == NULL && error == E_AGAIN) {
    logger(LOG_DEBUG, "client_sink_handler()", "incomplete request, waiting for more data");
    return;
  }

  // Complete (OK) or malformed (E_FAIL): the buffered head is not needed any more
  xps_buffer_destroy(head);
  session->from_client_buff = NULL;
  session->request_handled = true;
  session->http_req = http_req; // NULL for a malformed request

  // Must be the last thing done with 'session': it may destroy it
  session_process_request(session);
}

void client_sink_close_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  session_check_destroy(session);
}

/* ---------------------------------------------------------------------------------------------
 * upstream side (Stage 13 code, unused until upstream is wired back in)
 * ------------------------------------------------------------------------------------------- */

void upstream_source_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_source_t *source = ptr;
  xps_session_t *session = source->ptr;

  if (session->from_client_buff == NULL) {
    source->ready = false;
    return;
  }

  if (xps_pipe_source_write(source, session->from_client_buff) != OK) {
    logger(LOG_ERROR, "upstream_source_handler()", "xps_pipe_source_write() failed");
    return;
  }

  // Checking if upstream is connected
  if (session->upstream_connected == false) {
    session->upstream_write_bytes += session->from_client_buff->len;
    if (source->pipe != NULL && session->upstream_write_bytes > source->pipe->buff_list->len)
      session->upstream_connected = true;
  }

  xps_buffer_destroy(session->from_client_buff);
  set_from_client_buff(session, NULL);
  session_check_destroy(session);
}

void upstream_source_close_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_source_t *source = ptr;
  xps_session_t *session = source->ptr;

  if (!session->upstream_connected && !session->upstream_error_res_set)
    upstream_error_res(session);

  session_check_destroy(session);
}

void upstream_sink_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  session->upstream_connected = true;

  xps_buffer_t *buff = session_read_sink(sink);
  if (buff == NULL) {
    logger(LOG_ERROR, "upstream_sink_handler()", "xps_pipe_sink_read() failed");
    return;
  }

  size_t len = buff->len;
  set_to_client_buff(session, buff);
  xps_pipe_sink_clear(sink, len);
}

void upstream_sink_close_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  if (!session->upstream_connected && !session->upstream_error_res_set)
    upstream_error_res(session);

  session_check_destroy(session);
}

void upstream_error_res(xps_session_t *session) {
  assert(session != NULL);

  session->upstream_error_res_set = true;
}

/* ---------------------------------------------------------------------------------------------
 * file side
 * ------------------------------------------------------------------------------------------- */

// file -> session: moves file bytes (read from the pipe) into to_client_buff
void file_sink_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  xps_buffer_t *buff = session_read_sink(sink);
  if (buff == NULL) {
    logger(LOG_ERROR, "file_sink_handler()", "xps_pipe_sink_read() failed, destroying session");
    xps_session_destroy(session);
    return;
  }

  size_t len = buff->len;
  set_to_client_buff(session, buff); // takes ownership; file_sink->ready becomes false
  if (xps_pipe_sink_clear(sink, len) != OK)
    logger(LOG_ERROR, "file_sink_handler()", "failed to clear %lu bytes from sink",
           (unsigned long)len);
}

void file_sink_close_handler(void *ptr) {
  assert(ptr != NULL);

  xps_pipe_sink_t *sink = ptr;
  xps_session_t *session = sink->ptr;

  session_check_destroy(session);
}

/* ---------------------------------------------------------------------------------------------
 * helpers
 * ------------------------------------------------------------------------------------------- */

// Reads everything currently in the sink's pipe. Returns NULL if empty or on error.
static xps_buffer_t *session_read_sink(xps_pipe_sink_t *sink) {
  if (sink->pipe == NULL || sink->pipe->buff_list->len == 0)
    return NULL;
  return xps_pipe_sink_read(sink, sink->pipe->buff_list->len);
}

void set_to_client_buff(xps_session_t *session, xps_buffer_t *buff) {
  assert(session != NULL);

  if (buff != NULL && session->to_client_buff != NULL && session->to_client_buff != buff) {
    logger(LOG_ERROR, "set_to_client_buff()", "overwriting a pending buffer, dropping the old one");
    xps_buffer_destroy(session->to_client_buff);
  }

  session->to_client_buff = buff;

  if (buff == NULL) {
    // everything was written: accept more data from upstream / file
    session->client_source->ready = false;
    session->upstream_sink->ready = true;
    session->file_sink->ready = true;
  } else {
    // data waiting for the client: write it first, do not read more
    session->client_source->ready = true;
    session->upstream_sink->ready = false;
    session->file_sink->ready = false;
  }
}

void set_from_client_buff(xps_session_t *session, xps_buffer_t *buff) {
  assert(session != NULL);

  session->from_client_buff = buff;

  if (buff == NULL) {
    session->client_sink->ready = true;
    session->upstream_source->ready = false;
  } else {
    session->client_sink->ready = false;
    session->upstream_source->ready = true;
  }
}

void session_check_destroy(xps_session_t *session) {
  assert(session != NULL);

  bool c2u_flow = session->upstream_source->active &&
                  (session->client_sink->active || session->from_client_buff != NULL);

  bool u2c_flow = session->client_source->active &&
                  (session->upstream_sink->active || session->to_client_buff != NULL);

  bool f2c_flow = session->client_source->active &&
                  (session->file_sink->active || session->to_client_buff != NULL);

  bool flowing = c2u_flow || u2c_flow || f2c_flow;

  if (!flowing)
    xps_session_destroy(session);
}

// Builds an HTTP response with no body and queues it. May destroy the session if even that fails.
static void session_send_error(xps_session_t *session, u_int code) {
  xps_buffer_t *buff = NULL;

  xps_http_res_t *res = xps_http_res_create(session->core, code);
  if (res != NULL) {
    if (xps_http_set_header(&res->headers, "Content-Length", "0") == OK &&
        xps_http_set_header(&res->headers, "Connection", "close") == OK)
      buff = xps_http_res_serialize(res);
    xps_http_res_destroy(res);
  }

  if (buff == NULL) {
    logger(LOG_ERROR, "session_send_error()", "failed to build %u response, closing session", code);
    xps_session_destroy(session);
    return;
  }

  logger(LOG_DEBUG, "session_send_error()", "queued error response %u", code);
  set_to_client_buff(session, buff);
}

// Answers session->http_req (NULL means the request could not be parsed).
// The session may be destroyed on return: callers must not touch it afterwards.
void session_process_request(xps_session_t *session) {
  assert(session != NULL);

  xps_http_req_t *http_req = session->http_req;

  // BAD REQUEST: parse failure, bad URL, missing fields...
  if (http_req == NULL) {
    logger(LOG_DEBUG, "session_process_request()", "no valid http request -> 400");
    session_send_error(session, HTTP_BAD_REQUEST);
    return;
  }

  // HTTP/1.1 requires a Host header
  if (strcmp(http_req->http_version, "1.1") == 0 &&
      xps_http_get_header(&http_req->headers, "Host") == NULL) {
    logger(LOG_DEBUG, "session_process_request()", "missing Host header -> 400");
    session_send_error(session, HTTP_BAD_REQUEST);
    return;
  }

  // Only a file server for now
  if (http_req->method_n != HTTP_GET) {
    logger(LOG_DEBUG, "session_process_request()", "method %s not supported -> 501", http_req->method);
    session_send_error(session, HTTP_NOT_IMPLEMENTED);
    return;
  }

  // pathname has no query string. An empty one (absolute-form without a path) means "/"
  const char *pathname = http_req->pathname[0] == '\0' ? "/" : http_req->pathname;
  if (pathname[0] != '/') {
    session_send_error(session, HTTP_BAD_REQUEST);
    return;
  }

  size_t file_path_len = strlen(DEFAULT_PUBLIC_DIR) + strlen(pathname) + 1;
  char *file_path = malloc(file_path_len);
  if (file_path == NULL) {
    logger(LOG_ERROR, "session_process_request()", "malloc() failed for 'file_path'");
    session_send_error(session, HTTP_INTERNAL_SERVER_ERROR);
    return;
  }
  snprintf(file_path, file_path_len, "%s%s", DEFAULT_PUBLIC_DIR, pathname);

  int error;
  xps_file_t *file = xps_file_create(session->core, file_path, &error);
  if (file == NULL) {
    u_int code = HTTP_INTERNAL_SERVER_ERROR;
    if (error == E_PERMISSION)
      code = HTTP_FORBIDDEN;
    else if (error == E_NOTFOUND)
      code = HTTP_NOT_FOUND;
    logger(LOG_DEBUG, "session_process_request()", "xps_file_create(%s) failed -> %u", file_path, code);
    free(file_path);
    session_send_error(session, code);
    return;
  }
  free(file_path); // xps_file_create() keeps its own (resolved) copy of the path

  // Response head: status line + headers. The body is streamed from the file through the pipe.
  xps_buffer_t *head = NULL;
  xps_http_res_t *res = xps_http_res_create(session->core, HTTP_OK);
  if (res != NULL) {
    char len_str[32];
    snprintf(len_str, sizeof(len_str), "%lu", (unsigned long)file->size);

    int err = xps_http_set_header(&res->headers, "Content-Length", len_str);
    if (err == OK && file->mime_type != NULL)
      err = xps_http_set_header(&res->headers, "Content-Type", file->mime_type);
    if (err == OK)
      err = xps_http_set_header(&res->headers, "Connection", "close");
    if (err == OK)
      head = xps_http_res_serialize(res);
    xps_http_res_destroy(res);
  }
  if (head == NULL) {
    xps_file_destroy(file);
    session_send_error(session, HTTP_INTERNAL_SERVER_ERROR);
    return;
  }

  // file.source -> session.file_sink
  xps_pipe_t *file_pipe =
      xps_pipe_create(session->core, DEFAULT_PIPE_BUFF_THRESH, file->source, session->file_sink);
  if (file_pipe == NULL) {
    logger(LOG_ERROR, "session_process_request()", "xps_pipe_create() failed");
    xps_buffer_destroy(head);
    xps_file_destroy(file);
    session_send_error(session, HTTP_INTERNAL_SERVER_ERROR);
    return;
  }

  session->file = file; // non-owning, never dereferenced again

  logger(LOG_DEBUG, "session_process_request()", "serving %s (%lu bytes, %s)", http_req->pathname,
         (unsigned long)file->size, file->mime_type != NULL ? file->mime_type : "unknown type");

  // Queue the head. file_sink stays not-ready until it is written, so the head always goes first.
  set_to_client_buff(session, head);
}