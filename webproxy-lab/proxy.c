#define _GNU_SOURCE
/* HTTP/1 GET forwarding with a bounded worker pool and conservative shared
 * cache. The CS:APP RIO, socket helpers and the inherited LRU design remain the
 * base. One request per connection; chunked messages are explicitly
 * unsupported.
 */
#include "csapp.h"
#include <limits.h>
#include <math.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <strings.h>
#include <time.h>

#define CACHE_BYTES 1049000
#define OBJECT_BYTES 102400
#define ENTRIES 16
#define HEADER_BYTES 32768
#define QUEUE_SIZE 32
#define MAX_WORKERS 32

typedef struct {
  char key[MAXLINE];
  char *data;
  size_t size;
  double expires;
  double stored;
  unsigned long stamp;
  long age;
} cache_entry;
static cache_entry cache[ENTRIES];
static size_t cache_bytes;
static unsigned long cache_clock;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;
static int queue[QUEUE_SIZE], queue_head, queue_count;
static pthread_mutex_t queue_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t queue_ready = PTHREAD_COND_INITIALIZER;
static int timeout_ms = 2000;

static double now(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec + t.tv_nsec / 1e9;
}
static int send_all(int fd, const void *data, size_t n) {
  const char *p = data;
  while (n) {
    ssize_t k = write(fd, p, n);
    if (k < 0 && errno == EINTR)
      continue;
    if (k <= 0)
      return -1;
    p += k;
    n -= (size_t)k;
  }
  return 0;
}
static void error_reply(int fd, int status, const char *message) {
  char reply[512];
  int n = snprintf(reply, sizeof reply,
                   "HTTP/1.0 %d %s\r\nConnection: close\r\nContent-Type: "
                   "text/plain\r\nContent-Length: %zu\r\n\r\n%s",
                   status, message, strlen(message), message);
  if (n > 0 && (size_t)n < sizeof reply)
    send_all(fd, reply, (size_t)n);
}
static void set_timeouts(int fd) {
  struct timeval t = {timeout_ms / 1000, (timeout_ms % 1000) * 1000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &t, sizeof t);
}
static int connect_origin(const char *host, const char *port) {
  struct addrinfo hints = {0}, *list, *p;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo(host, port, &hints, &list))
    return -1;
  double deadline = now() + timeout_ms / 1000.0;
  int fd = -1;
  for (p = list; p && now() < deadline; p = p->ai_next) {
    fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
    if (fd < 0)
      continue;
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int result = connect(fd, p->ai_addr, p->ai_addrlen);
    if (result < 0 && errno == EINPROGRESS) {
      struct pollfd pollfd = {fd, POLLOUT, 0};
      do {
        int remaining = (int)((deadline - now()) * 1000);
        result = poll(&pollfd, 1, remaining > 0 ? remaining : 0);
      } while (result < 0 && errno == EINTR && now() < deadline);
      int err = 0;
      socklen_t length = sizeof err;
      if (result > 0 &&
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &length) == 0 && !err)
        result = 0;
      else {
        if (result == 0)
          errno = ETIMEDOUT;
        result = -1;
      }
    }
    if (result == 0) {
      fcntl(fd, F_SETFL, flags);
      set_timeouts(fd);
      break;
    }
    close(fd);
    fd = -1;
  }
  freeaddrinfo(list);
  return fd;
}
static bool decimal(const char *s, long *value) {
  if (!*s)
    return false;
  for (const char *p = s; *p; p++)
    if (*p < '0' || *p > '9')
      return false;
  errno = 0;
  *value = strtol(s, NULL, 10);
  return !errno && *value >= 0;
}
static char *trim(char *s) {
  while (*s == ' ' || *s == '\t')
    s++;
  size_t n = strlen(s);
  while (n && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' ||
               s[n - 1] == '\n'))
    s[--n] = 0;
  return s;
}
static int header_line(rio_t *rio, char *line, size_t size) {
  ssize_t n = rio_readlineb(rio, line, size);
  if (n <= 0)
    return -1;
  if (n < 2 || line[n - 2] != '\r' || line[n - 1] != '\n' ||
      (size_t)n != strlen(line))
    return -1;
  return (int)n;
}
static bool split_header(char *line, char **name, char **value) {
  char *colon = strchr(line, ':');
  if (!colon || colon == line)
    return false;
  for (char *p = line; p < colon; p++) {
    if (!(isalnum((unsigned char)*p) || strchr("!#$%&'*+-.^_`|~", *p)))
      return false;
    *p = (char)tolower((unsigned char)*p);
  }
  *colon = 0;
  *name = line;
  *value = trim(colon + 1);
  for (char *p = *value; *p; p++)
    if ((unsigned char)*p < 32 && *p != '\t')
      return false;
  return true;
}
static bool append_header(char *out, size_t *used, const char *name,
                          const char *value) {
  int n =
      snprintf(out + *used, HEADER_BYTES - *used, "%s: %s\r\n", name, value);
  if (n < 0 || (size_t)n >= HEADER_BYTES - *used)
    return false;
  *used += (size_t)n;
  return true;
}
static bool parse_uri(const char *uri, char *host, char *port, char *path) {
  if (strncmp(uri, "http://", 7))
    return false;
  const char *start = uri + 7, *end = strpbrk(start, "/?#");
  if (!end)
    end = start + strlen(start);
  size_t n = (size_t)(end - start);
  if (!n || n >= 512 || memchr(start, '@', n) || memchr(start, '[', n) ||
      strchr(uri, '#'))
    return false;
  memcpy(host, start, n);
  host[n] = 0;
  char *colon = strchr(host, ':');
  strcpy(port, "80");
  if (colon) {
    *colon++ = 0;
    long number;
    if (!decimal(colon, &number) || number < 1 || number > 65535)
      return false;
    snprintf(port, 16, "%ld", number);
  }
  if (!*host)
    return false;
  for (char *p = host; *p; p++) {
    if (!isalnum((unsigned char)*p) && *p != '-' && *p != '.')
      return false;
    *p = (char)tolower((unsigned char)*p);
  }
  int result = snprintf(path, MAXLINE, "%s%s", *end == '/' ? "" : "/", end);
  return result >= 0 && result < MAXLINE;
}
static void discard_entry(int i) {
  cache_bytes -= cache[i].size;
  free(cache[i].data);
  memset(&cache[i], 0, sizeof cache[i]);
}
static int send_cached(int fd, const char *data, size_t size, long age) {
  const char *end = strstr(data, "\r\n\r\n");
  if (!end)
    return -1;
  size_t head = (size_t)(end - data) + 2;
  char header[80];
  int n = snprintf(header, sizeof header, "Age: %ld\r\n\r\n", age);
  if (send_all(fd, data, head) || send_all(fd, header, (size_t)n))
    return -1;
  return send_all(fd, data + head + 2, size - head - 2);
}
static bool cache_serve(int fd, const char *key) {
  char *copy = NULL;
  size_t size = 0;
  long age = 0;
  pthread_mutex_lock(&cache_lock);
  for (int i = 0; i < ENTRIES; i++) {
    if (cache[i].data && cache[i].expires <= now())
      discard_entry(i);
    if (cache[i].data && !strcmp(cache[i].key, key)) {
      size = cache[i].size;
      copy = malloc(size + 1);
      if (copy) {
        memcpy(copy, cache[i].data, size + 1);
        age = cache[i].age + (long)(now() - cache[i].stored);
        cache[i].stamp = ++cache_clock;
      }
      break;
    }
  }
  pthread_mutex_unlock(&cache_lock);
  if (!copy)
    return false;
  send_cached(fd, copy, size, age);
  free(copy);
  fprintf(stderr, "cache HIT %s\n", key);
  return true;
}
static void cache_store(const char *key, const char *data, size_t size,
                        double expires, long age) {
  if (size > OBJECT_BYTES || expires <= now())
    return;
  char *copy = malloc(size + 1);
  if (!copy)
    return;
  memcpy(copy, data, size);
  copy[size] = 0;
  pthread_mutex_lock(&cache_lock);
  int slot = -1;
  for (int i = 0; i < ENTRIES; i++)
    if (cache[i].data && !strcmp(cache[i].key, key))
      discard_entry(i);
  for (;;) {
    int victim = -1;
    for (int i = 0; i < ENTRIES; i++) {
      if (!cache[i].data)
        slot = i;
      else if (victim < 0 || cache[i].stamp < cache[victim].stamp)
        victim = i;
    }
    if (slot >= 0 && cache_bytes + size <= CACHE_BYTES)
      break;
    if (victim < 0)
      break;
    discard_entry(victim);
  }
  if (slot >= 0) {
    cache[slot] = (cache_entry){.data = copy,
                                .size = size,
                                .expires = expires,
                                .stored = now(),
                                .stamp = ++cache_clock,
                                .age = age};
    snprintf(cache[slot].key, MAXLINE, "%s", key);
    cache_bytes += size;
  } else
    free(copy);
  pthread_mutex_unlock(&cache_lock);
}
static void handle_client(int fd) {
  rio_t client;
  rio_readinitb(&client, fd);
  char line[MAXLINE], method[32], uri[MAXLINE], version[32], extra;
  char host[512], port[16], path[MAXLINE], key[MAXLINE];
  char headers[HEADER_BYTES] = "";
  size_t used = 0, read_total = 0;
  bool cacheable = true, host_seen = false;
  if (header_line(&client, line, sizeof line) < 0)
    return;
  if (sscanf(line, "%31s %8191s %31s %c", method, uri, version, &extra) != 3) {
    error_reply(fd, 400, "Bad Request");
    return;
  }
  if (strcmp(method, "GET")) {
    error_reply(fd, 501, "GET Only");
    return;
  }
  if ((strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1")) ||
      !parse_uri(uri, host, port, path)) {
    error_reply(fd, 400, "Unsupported Request Target");
    return;
  }
  for (;;) {
    int n = header_line(&client, line, sizeof line);
    if (n < 0 || (read_total += (size_t)n) > HEADER_BYTES) {
      error_reply(fd, 400, "Invalid Headers");
      return;
    }
    if (!strcmp(line, "\r\n"))
      break;
    char *name, *value;
    if (!split_header(line, &name, &value)) {
      error_reply(fd, 400, "Invalid Header");
      return;
    }
    if (!strcmp(name, "host")) {
      if (host_seen) {
        error_reply(fd, 400, "Duplicate Host");
        return;
      }
      host_seen = true;
      continue;
    }
    if (!strcmp(name, "transfer-encoding") || !strcmp(name, "expect") ||
        !strcmp(name, "upgrade") || !strcmp(name, "trailer")) {
      error_reply(fd, 400, "Unsupported Framing");
      return;
    }
    if (!strcmp(name, "content-length")) {
      long size;
      if (!decimal(value, &size) || size) {
        error_reply(fd, 400, "GET Body Unsupported");
        return;
      }
      continue;
    }
    if (!strcmp(name, "connection") || !strcmp(name, "proxy-connection")) {
      if (strcasecmp(value, "close") && strcasecmp(value, "keep-alive")) {
        error_reply(fd, 400, "Unsupported Connection Options");
        return;
      }
      continue;
    }
    if (!strcmp(name, "keep-alive") || !strcmp(name, "te") ||
        !strcmp(name, "proxy-authorization"))
      continue;
    if (!strcmp(name, "authorization") || !strcmp(name, "cookie") ||
        !strcmp(name, "range") || !strncmp(name, "if-", 3) ||
        !strcmp(name, "cache-control") || !strcmp(name, "pragma"))
      cacheable = false;
    if (!append_header(headers, &used, name, value)) {
      error_reply(fd, 431, "Headers Too Large");
      return;
    }
  }
  if (!strcmp(version, "HTTP/1.1") && !host_seen) {
    error_reply(fd, 400, "Host Required");
    return;
  }
  int k = snprintf(key, sizeof key, "http://%s:%s%s", host, port, path);
  if (k < 0 || (size_t)k >= sizeof key) {
    error_reply(fd, 414, "URI Too Long");
    return;
  }
  if (cacheable && cache_serve(fd, key))
    return;
  fprintf(stderr, "cache MISS %s\n", key);
  int origin = connect_origin(host, port);
  if (origin < 0) {
    error_reply(fd, errno == ETIMEDOUT ? 504 : 502, "Origin Unavailable");
    return;
  }
  char first[MAXLINE + 1024];
  k = snprintf(first, sizeof first,
               "GET %s HTTP/1.0\r\nHost: %s:%s\r\nConnection: close\r\n", path,
               host, port);
  if (send_all(origin, first, (size_t)k) || send_all(origin, headers, used) ||
      send_all(origin, "\r\n", 2)) {
    close(origin);
    error_reply(fd, 502, "Origin Write Failed");
    return;
  }
  double response_start = now();
  rio_t upstream;
  rio_readinitb(&upstream, origin);
  if (header_line(&upstream, line, sizeof line) < 0) {
    close(origin);
    error_reply(fd, 504, "Origin Timeout");
    return;
  }
  int status;
  char response_version[32];
  if (sscanf(line, "%31s %d", response_version, &status) != 2 ||
      (strcmp(response_version, "HTTP/1.0") &&
       strcmp(response_version, "HTTP/1.1")) ||
      status < 200 || status > 599) {
    close(origin);
    error_reply(fd, 502, "Invalid Origin Status");
    return;
  }
  used =
      (size_t)snprintf(headers, sizeof headers,
                       "HTTP/1.0 %d Response\r\nConnection: close\r\n", status);
  long length = -1, max_age = -1, age = 0;
  bool public_response = false, control_seen = false, valid = true,
       age_seen = false;
  double date_age = 0;
  bool date_seen = false;
  read_total = 0;
  while (valid) {
    int n = header_line(&upstream, line, sizeof line);
    if (n < 0 || (read_total += (size_t)n) > HEADER_BYTES) {
      valid = false;
      break;
    }
    if (!strcmp(line, "\r\n"))
      break;
    char *name, *value;
    if (!split_header(line, &name, &value)) {
      valid = false;
      break;
    }
    if (!strcmp(name, "transfer-encoding")) {
      valid = false;
      break;
    }
    if (!strcmp(name, "content-length")) {
      if (length >= 0 || !decimal(value, &length)) {
        valid = false;
        break;
      }
    }
    if (!strcmp(name, "date")) {
      struct tm tm = {0};
      char *end = strptime(value, "%a, %d %b %Y %H:%M:%S GMT", &tm);
      if (date_seen || !end || *end)
        cacheable = false;
      else {
        double apparent = difftime(time(NULL), timegm(&tm));
        if (apparent > 0)
          date_age = apparent;
      }
      date_seen = true;
    }
    if (!strcmp(name, "age")) {
      if (age_seen || !decimal(value, &age)) {
        valid = false;
        break;
      }
      age_seen = true;
      continue;
    }
    if (!strcmp(name, "connection")) {
      if (strcasecmp(value, "close") && strcasecmp(value, "keep-alive")) {
        valid = false;
        break;
      }
      continue;
    }
    if (!strcmp(name, "keep-alive") || !strcmp(name, "proxy-connection"))
      continue;
    if (!strcmp(name, "set-cookie") || !strcmp(name, "vary") ||
        !strcmp(name, "www-authenticate") || !strcmp(name, "pragma"))
      cacheable = false;
    if (!strcmp(name, "cache-control")) {
      if (control_seen)
        cacheable = false;
      control_seen = true;
      char copy[MAXLINE];
      snprintf(copy, sizeof copy, "%s", value);
      char *save, *token = strtok_r(copy, ",", &save);
      while (token) {
        token = trim(token);
        if (!strcasecmp(token, "public"))
          public_response = true;
        else if (!strncasecmp(token, "max-age=", 8)) {
          if (max_age >= 0 || !decimal(token + 8, &max_age))
            cacheable = false;
        } else
          cacheable =
              false; /* unknown/revalidation/private directives bypass */
        token = strtok_r(NULL, ",", &save);
      }
    }
    if (!append_header(headers, &used, name, value))
      valid = false;
  }
  if (!valid) {
    close(origin);
    error_reply(fd, 502, "Unsupported Origin Message");
    return;
  }
  if (used + 2 >= sizeof headers) {
    close(origin);
    error_reply(fd, 502, "Origin Headers Too Large");
    return;
  }
  memcpy(headers + used, "\r\n", 3);
  used += 2;
  cacheable = cacheable && status == 200 && public_response && max_age > age &&
              max_age <= 86400 && length >= 0;
  double corrected_age = fmax(date_age, (double)age) + (now() - response_start);
  double expires = now() + (double)max_age - corrected_age;
  age = (long)fmin(corrected_age, (double)LONG_MAX - 1024);
  char object[OBJECT_BYTES + 1];
  size_t total = used;
  if (used > OBJECT_BYTES)
    cacheable = false;
  if (cacheable)
    memcpy(object, headers, used);
  /* Forward the original age; cache hits add their residence time. */
  char age_header[80];
  k = snprintf(age_header, sizeof age_header, "Age: %ld\r\n\r\n", age);
  if (send_all(fd, headers, used - 2) || send_all(fd, age_header, (size_t)k)) {
    close(origin);
    return;
  }
  long remaining = length;
  ssize_t n = 0;
  while (remaining != 0) {
    size_t want =
        remaining < 0 || remaining > MAXBUF ? MAXBUF : (size_t)remaining;
    n = rio_readnb(&upstream, line, want);
    if (n <= 0)
      break;
    if (send_all(fd, line, (size_t)n)) {
      cacheable = false;
      break;
    }
    if (cacheable && total + (size_t)n <= OBJECT_BYTES)
      memcpy(object + total, line, (size_t)n);
    else
      cacheable = false;
    total += (size_t)n;
    if (remaining > 0)
      remaining -= n;
  }
  close(origin);
  if (cacheable && remaining == 0)
    cache_store(key, object, total, expires, age);
}
static void *worker(void *unused) {
  (void)unused;
  for (;;) {
    pthread_mutex_lock(&queue_lock);
    while (!queue_count)
      pthread_cond_wait(&queue_ready, &queue_lock);
    int fd = queue[queue_head];
    queue_head = (queue_head + 1) % QUEUE_SIZE;
    queue_count--;
    pthread_mutex_unlock(&queue_lock);
    set_timeouts(fd);
    handle_client(fd);
    close(fd);
  }
  return NULL;
}
int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    return 1;
  }
  int workers = 8;
  long parsed;
  const char *value = getenv("PROXY_TIMEOUT_MS");
  if (value) {
    if (!decimal(value, &parsed) || parsed < 50 || parsed > 60000)
      return 1;
    timeout_ms = (int)parsed;
  }
  value = getenv("PROXY_WORKERS");
  if (value) {
    if (!decimal(value, &parsed) || parsed < 1 || parsed > MAX_WORKERS)
      return 1;
    workers = (int)parsed;
  }
  signal(SIGPIPE, SIG_IGN);
  long port_number;
  if (!decimal(argv[1], &port_number) || port_number < 1 || port_number > 65535)
    return 1;
  int listenfd = socket(AF_INET, SOCK_STREAM, 0);
  if (listenfd < 0)
    return 1;
  int reuse = 1;
  setsockopt(listenfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof reuse);
  struct sockaddr_in address = {0};
  address.sin_family = AF_INET;
  address.sin_port = htons((uint16_t)port_number);
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  if (bind(listenfd, (struct sockaddr *)&address, sizeof address) < 0 ||
      listen(listenfd, QUEUE_SIZE) < 0) {
    close(listenfd);
    return 1;
  }
  for (int i = 0; i < workers; i++) {
    pthread_t tid;
    if (pthread_create(&tid, NULL, worker, NULL))
      return 1;
    pthread_detach(tid);
  }
  fprintf(stderr,
          "GET proxy workers=%d queue=%d timeout_ms=%d cache_bytes=%d "
          "object_bytes=%d\n",
          workers, QUEUE_SIZE, timeout_ms, CACHE_BYTES, OBJECT_BYTES);
  for (;;) {
    int fd = accept(listenfd, NULL, NULL);
    if (fd < 0) {
      if (errno == EINTR)
        continue;
      break;
    }
    pthread_mutex_lock(&queue_lock);
    if (queue_count == QUEUE_SIZE) {
      pthread_mutex_unlock(&queue_lock);
      close(fd);
      continue;
    }
    queue[(queue_head + queue_count) % QUEUE_SIZE] = fd;
    queue_count++;
    pthread_cond_signal(&queue_ready);
    pthread_mutex_unlock(&queue_lock);
  }
  close(listenfd);
  return 0;
}
