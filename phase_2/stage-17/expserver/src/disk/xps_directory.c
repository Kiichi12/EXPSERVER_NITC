#include "xps_directory.h"

/* ---------------------------------------------------------------------------------------------
 * small growable string used to build the page (no fixed-size buffer, so no overflow)
 * ------------------------------------------------------------------------------------------- */

typedef struct {
  char *data;
  size_t len;
  size_t cap;
} sbuf_t;

static bool sb_append(sbuf_t *sb, const char *str, size_t n) {
  if (sb->len + n + 1 > sb->cap) {
    size_t new_cap = sb->cap ? sb->cap : 4096;
    while (new_cap < sb->len + n + 1)
      new_cap *= 2;
    char *p = realloc(sb->data, new_cap);
    if (p == NULL)
      return false;
    sb->data = p;
    sb->cap = new_cap;
  }
  memcpy(sb->data + sb->len, str, n);
  sb->len += n;
  sb->data[sb->len] = '\0';
  return true;
}

static bool sb_puts(sbuf_t *sb, const char *str) { 
    return sb_append(sb, str, strlen(str)); 
}

static bool sb_printf(sbuf_t *sb, const char *fmt, ...) {
  char tmp[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n < 0 || (size_t)n >= sizeof(tmp))
    return false; // only used for short, bounded strings
  return sb_append(sb, tmp, (size_t)n);
}

// Names and paths can contain anything: never put them in the page unescaped
static bool sb_html_escape(sbuf_t *sb, const char *str) {
  for (; *str != '\0'; str++) {
    bool ok;
    switch (*str) {
      case '<': ok = sb_puts(sb, "&lt;"); break;
      case '>': ok = sb_puts(sb, "&gt;"); break;
      case '&': ok = sb_puts(sb, "&amp;"); break;
      case '"': ok = sb_puts(sb, "&quot;"); break;
      case '\'': ok = sb_puts(sb, "&#39;"); break;
      default: ok = sb_append(sb, str, 1); break;
    }
    if (!ok)
      return false;
  }
  return true;
}

// Percent-encodes everything except unreserved characters and '/'.
// keep_percent: leave '%' alone (the request pathname is already percent-encoded)
static bool sb_url_encode(sbuf_t *sb, const char *str, bool keep_percent) {
  for (; *str != '\0'; str++) {
    unsigned char c = (unsigned char)*str;
    bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '-' || c == '_' || c == '.' || c == '~' || c == '/' ||
                 (keep_percent && c == '%');
    if (plain ? !sb_append(sb, (const char *)&c, 1) : !sb_printf(sb, "%%%02X", c))
      return false;
  }
  return true;
}

static int name_cmp(const void *a, const void *b) {
  return strcmp(*(char *const *)a, *(char *const *)b);
}

/* ---------------------------------------------------------------------------------------------
 * directory listing
 * ------------------------------------------------------------------------------------------- */

xps_buffer_t *xps_directory_browsing(const char *dir_path, const char *pathname) {
  assert(dir_path != NULL);
  assert(pathname != NULL);

  sbuf_t sb = {NULL, 0, 0};
  char **names = NULL;
  size_t n_names = 0, names_cap = 0;
  char *base = NULL;
  xps_buffer_t *result = NULL;

  // Open a directory stream and collect the entries
  DIR *dir = opendir(dir_path);
  if (dir == NULL) {
    logger(LOG_ERROR, "xps_directory_browsing()", "opendir() failed for %s", dir_path);
    return NULL;
  }

  struct dirent *dir_entry;
  while ((dir_entry = readdir(dir)) != NULL) {
    // skip ".", ".." and hidden files (.git, .env, ...)
    if (dir_entry->d_name[0] == '.')
      continue;

    if (n_names == names_cap) {
      size_t new_cap = names_cap ? names_cap * 2 : 32;
      char **p = realloc(names, new_cap * sizeof(char *));
      if (p == NULL) {
        closedir(dir);
        goto cleanup;
      }
      names = p;
      names_cap = new_cap;
    }
    names[n_names] = str_create(dir_entry->d_name);
    if (names[n_names] == NULL) {
      closedir(dir);
      goto cleanup;
    }
    n_names++;
  }
  closedir(dir);

  qsort(names, n_names, sizeof(char *), name_cmp);

  // Base for the links: pathname without trailing slashes ("/" -> "")
  base = str_create(pathname);
  if (base == NULL)
    goto cleanup;
  size_t base_len = strlen(base);
  while (base_len > 0 && base[base_len - 1] == '/')
    base[--base_len] = '\0';

  // Heading and basic styling
  if (!sb_puts(&sb, "<html><head lang='en'><meta http-equiv='Content-Type' "
                    "content='text/html; charset=UTF-8'><meta name='viewport' "
                    "content='width=device-width, initial-scale=1.0'><title>Directory: ") ||
      !sb_html_escape(&sb, pathname) ||
      !sb_puts(&sb, "</title><style>body{font-family: monospace; font-size: 15px;}"
                    "td {padding: 1.5px 6px; padding-right: 20px;} "
                    "h1{font-family: serif; margin: 0;} "
                    "h3{font-family: serif;margin: 12px 0px; "
                    "background-color: rgba(0,0,0,0.1); padding: 4px 0px;}"
                    "</style></head><body><h1>eXpServer</h1><h3>Index of&nbsp;") ||
      !sb_html_escape(&sb, pathname) || !sb_puts(&sb, "</h3><hr><table>"))
    goto cleanup;

  // One row per file / directory
  for (size_t i = 0; i < n_names; i++) {
    char *full_path = path_join(dir_path, names[i]);
    if (full_path == NULL)
      goto cleanup;

    struct stat file_stat;
    int stat_ret = stat(full_path, &file_stat);
    free(full_path);
    if (stat_ret != 0) {
      logger(LOG_DEBUG, "xps_directory_browsing()", "stat() failed for %s, skipping", names[i]);
      continue;
    }

    bool is_directory = S_ISDIR(file_stat.st_mode);
    if (!is_directory && !S_ISREG(file_stat.st_mode))
      continue;

    char time_buff[32] = "";
    struct tm tm_buf;
    if (localtime_r(&file_stat.st_mtime, &tm_buf) != NULL)
      strftime(time_buff, sizeof(time_buff), "%Y-%m-%d %H:%M:%S", &tm_buf);

    if (!sb_puts(&sb, "<tr><td><a href=\"") || !sb_url_encode(&sb, base, true) ||
        !sb_puts(&sb, "/") || !sb_url_encode(&sb, names[i], false) ||
        !sb_puts(&sb, is_directory ? "/\">" : "\">") || !sb_html_escape(&sb, names[i]) ||
        !sb_puts(&sb, is_directory ? "/</a></td>" : "</a></td>") ||
        !sb_printf(&sb, "<td>%s</td>", time_buff))
      goto cleanup;

    if (is_directory) {
      if (!sb_puts(&sb, "<td>-</td></tr>\n"))
        goto cleanup;
    } else if (!sb_printf(&sb, "<td>%.1f KB</td></tr>\n", file_stat.st_size / 1024.0)) {
      goto cleanup;
    }
  }

  if (!sb_puts(&sb, "</table></body></html>"))
    goto cleanup;

  result = xps_buffer_create(sb.len, sb.len, NULL);
  if (result == NULL) {
    logger(LOG_ERROR, "xps_directory_browsing()", "xps_buffer_create() returned NULL");
    goto cleanup;
  }
  memcpy(result->data, sb.data, sb.len);

cleanup:
  if (result == NULL)
    logger(LOG_ERROR, "xps_directory_browsing()", "failed to build listing for %s", dir_path);
  for (size_t i = 0; i < n_names; i++)
    free(names[i]);
  free(names);
  free(base);
  free(sb.data);
  return result;
}