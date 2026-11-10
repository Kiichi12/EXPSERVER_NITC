#ifndef XPS_UTILS_H
#define XPS_UTILS_H

#include "../xps.h"
#include <fcntl.h>


// Sockets
bool is_valid_port(u_int port);
int make_socket_non_blocking(u_int sock_fd);

//address and ip 
struct addrinfo *xps_getaddrinfo(const char *host, u_int port);
char *get_remote_ip(u_int sock_fd);

// Other functions

const char *get_file_ext(const char *file_path);
void vec_filter_null(vec_void_t *v);
char *str_from_ptrs(const char *start, const char *end);

// Stage-16
bool is_abs_path(const char *path);
bool is_file(const char *path);
bool is_dir(const char *path);
char *str_create(const char *str);
char *path_join(const char *path_1, const char *path_2);
bool str_starts_with(const char *str, const char *prefix);


#endif
