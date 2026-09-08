#include "csapp.h"

#include <stdbool.h>
#include <strings.h>

/* 권장 캐시 크기와 객체 최대 크기 */
#define MAX_CACHE_SIZE 1049000
#define MAX_OBJECT_SIZE 102400
#define CACHE_ENTRY_COUNT 16

/* 이 긴 줄을 코드에 그대로 넣어도 스타일 점수는 깎이지 않습니다 */
static const char *user_agent_hdr =
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) Gecko/20120305 "
    "Firefox/10.0.3\r\n";

typedef struct {
  int valid;
  char uri[MAXLINE];
  char *object;
  size_t size;
  unsigned long stamp;
} cache_entry_t;

static cache_entry_t cache[CACHE_ENTRY_COUNT];
static size_t cache_bytes_used = 0;
static unsigned long cache_clock = 1;
static pthread_mutex_t cache_mutex = PTHREAD_MUTEX_INITIALIZER;

static void *thread(void *vargp);
static void handle_client(int connfd);
static int parse_uri(const char *uri, char *host, char *port, char *path);
static void parse_host_header(const char *header, char *host, char *port);
static int build_requesthdrs(rio_t *client_rio, const char *uri, char *host,
                             char *port, char *path, char *request_hdr,
                             size_t request_hdr_size, char *cache_key,
                             size_t cache_key_size);
static void clienterror(int fd, const char *cause, const char *errnum,
                        const char *shortmsg, const char *longmsg);
static ssize_t send_all(int fd, const void *buf, size_t n);
static void cache_init(void);
static bool cache_try_serve(int fd, const char *uri);
static void cache_store(const char *uri, const char *buf, size_t size);
static int cache_find_index_locked(const char *uri);
static int cache_select_victim_locked(void);

int main(int argc, char **argv) {
  int listenfd;
  pthread_t tid;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  Signal(SIGPIPE, SIG_IGN);
  cache_init();

  listenfd = Open_listenfd(argv[1]);
  while (1) {
    int *connfdp = Malloc(sizeof(int));
    *connfdp = Accept(listenfd, NULL, NULL);
    Pthread_create(&tid, NULL, thread, connfdp);
  }
}

static void *thread(void *vargp) {
  int connfd = *((int *)vargp);

  Free(vargp);
  Pthread_detach(Pthread_self());
  handle_client(connfd);
  Close(connfd);
  return NULL;
}

static void handle_client(int connfd) {
  char buf[MAXLINE];
  char method[MAXLINE];
  char uri[MAXLINE];
  char version[MAXLINE];
  char host[MAXLINE];
  char port[16];
  char path[MAXLINE];
  char request_hdr[MAXBUF];
  char cache_key[MAXLINE];
  char object_buf[MAX_OBJECT_SIZE];
  rio_t client_rio;
  rio_t server_rio;
  int serverfd;
  ssize_t n;
  size_t total_size = 0;
  bool cacheable = true;

  rio_readinitb(&client_rio, connfd);
  n = rio_readlineb(&client_rio, buf, MAXLINE);
  if (n <= 0) {
    return;
  }

  if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
    clienterror(connfd, buf, "400", "Bad Request",
                "Proxy could not parse the request line");
    return;
  }
  (void)version;

  if (strcasecmp(method, "GET")) {
    clienterror(connfd, method, "501", "Not Implemented",
                "Proxy only implements the GET method");
    return;
  }

  if (parse_uri(uri, host, port, path) < 0) {
    clienterror(connfd, uri, "400", "Bad Request",
                "Proxy could not parse the URI");
    return;
  }

  if (build_requesthdrs(&client_rio, uri, host, port, path, request_hdr,
                        sizeof(request_hdr), cache_key,
                        sizeof(cache_key)) < 0) {
    clienterror(connfd, uri, "400", "Bad Request",
                "Proxy could not build the request headers");
    return;
  }

  if (cache_try_serve(connfd, cache_key)) {
    return;
  }

  serverfd = open_clientfd(host, port);
  if (serverfd < 0) {
    clienterror(connfd, host, "502", "Bad Gateway",
                "Proxy could not connect to the end server");
    return;
  }

  if (send_all(serverfd, request_hdr, strlen(request_hdr)) < 0) {
    Close(serverfd);
    return;
  }

  rio_readinitb(&server_rio, serverfd);
  while ((n = rio_readnb(&server_rio, buf, MAXBUF)) > 0) {
    if (send_all(connfd, buf, (size_t)n) < 0) {
      cacheable = false;
      break;
    }

    if (cacheable && total_size + (size_t)n <= MAX_OBJECT_SIZE) {
      memcpy(object_buf + total_size, buf, (size_t)n);
    } else {
      cacheable = false;
    }
    total_size += (size_t)n;
  }

  if (n == 0 && cacheable && total_size <= MAX_OBJECT_SIZE) {
    cache_store(cache_key, object_buf, total_size);
  }

  Close(serverfd);
}

static int parse_uri(const char *uri, char *host, char *port, char *path) {
  const char *hostbegin = uri;
  const char *hostend;
  const char *pathbegin;
  const char *portbegin;
  size_t hostlen;
  size_t portlen;

  host[0] = '\0';
  strcpy(port, "80");
  strcpy(path, "/");

  if (!strncasecmp(uri, "http://", 7)) {
    hostbegin = uri + 7;
  } else if (!strncasecmp(uri, "https://", 8)) {
    return -1;
  } else if (uri[0] == '/') {
    snprintf(path, MAXLINE, "%s", uri);
    return 0;
  }

  pathbegin = strchr(hostbegin, '/');
  if (pathbegin != NULL) {
    snprintf(path, MAXLINE, "%s", pathbegin);
    hostend = pathbegin;
  } else {
    hostend = hostbegin + strlen(hostbegin);
  }

  portbegin = memchr(hostbegin, ':', (size_t)(hostend - hostbegin));
  if (portbegin != NULL) {
    hostlen = (size_t)(portbegin - hostbegin);
    portlen = (size_t)(hostend - portbegin - 1);
    if (hostlen == 0 || portlen == 0 || hostlen >= MAXLINE || portlen >= 16) {
      return -1;
    }
    memcpy(host, hostbegin, hostlen);
    host[hostlen] = '\0';
    memcpy(port, portbegin + 1, portlen);
    port[portlen] = '\0';
  } else {
    hostlen = (size_t)(hostend - hostbegin);
    if (hostlen == 0 || hostlen >= MAXLINE) {
      return -1;
    }
    memcpy(host, hostbegin, hostlen);
    host[hostlen] = '\0';
  }

  return 0;
}

static void parse_host_header(const char *header, char *host, char *port) {
  char value[MAXLINE];
  char *start;
  char *end;
  char *colon;

  snprintf(value, sizeof(value), "%s", header);
  start = value + 5;
  while (*start != '\0' && isspace((unsigned char)*start)) {
    start++;
  }

  end = start + strlen(start);
  while (end > start &&
         (end[-1] == '\r' || end[-1] == '\n' || isspace((unsigned char)end[-1]))) {
    end--;
  }
  *end = '\0';

  colon = strchr(start, ':');
  if (colon != NULL) {
    *colon = '\0';
    snprintf(host, MAXLINE, "%s", start);
    snprintf(port, 16, "%s", colon + 1);
  } else {
    snprintf(host, MAXLINE, "%s", start);
    strcpy(port, "80");
  }
}

static int build_requesthdrs(rio_t *client_rio, const char *uri, char *host,
                             char *port, char *path, char *request_hdr,
                             size_t request_hdr_size, char *cache_key,
                             size_t cache_key_size) {
  char buf[MAXLINE];
  char host_hdr[MAXLINE];
  char other_hdrs[MAXBUF];
  ssize_t n;
  int len;

  other_hdrs[0] = '\0';
  while ((n = rio_readlineb(client_rio, buf, MAXLINE)) > 0) {
    if (!strcmp(buf, "\r\n")) {
      break;
    }

    if (!strncasecmp(buf, "Host:", 5)) {
      if (host[0] == '\0') {
        parse_host_header(buf, host, port);
      }
      continue;
    }
    if (!strncasecmp(buf, "User-Agent:", 11) ||
        !strncasecmp(buf, "Connection:", 11) ||
        !strncasecmp(buf, "Proxy-Connection:", 17)) {
      continue;
    }

    if (strlen(other_hdrs) + strlen(buf) < sizeof(other_hdrs)) {
      strcat(other_hdrs, buf);
    }
  }

  if (n < 0 || host[0] == '\0') {
    return -1;
  }
  if (path[0] == '\0') {
    strcpy(path, "/");
  }

  if (!strcmp(port, "80")) {
    len = snprintf(host_hdr, sizeof(host_hdr), "Host: %s\r\n", host);
  } else {
    len = snprintf(host_hdr, sizeof(host_hdr), "Host: %s:%s\r\n", host, port);
  }
  if (len < 0 || (size_t)len >= sizeof(host_hdr)) {
    return -1;
  }

  len = snprintf(request_hdr, request_hdr_size,
                 "GET %s HTTP/1.0\r\n"
                 "%s"
                 "%s"
                 "Connection: close\r\n"
                 "Proxy-Connection: close\r\n"
                 "%s"
                 "\r\n",
                 path, host_hdr, user_agent_hdr, other_hdrs);
  if (len < 0 || (size_t)len >= request_hdr_size) {
    return -1;
  }

  len = snprintf(cache_key, cache_key_size, "http://%s:%s%s", host, port, path);
  if (len < 0 || (size_t)len >= cache_key_size) {
    return -1;
  }

  (void)uri;
  return 0;
}

static void clienterror(int fd, const char *cause, const char *errnum,
                        const char *shortmsg, const char *longmsg) {
  char buf[MAXBUF];
  char body[MAXBUF];
  int body_len;
  int header_len;

  body_len = snprintf(body, sizeof(body),
                      "<html><title>Proxy Error</title>"
                      "<body bgcolor=\"ffffff\">\r\n"
                      "%s: %s\r\n"
                      "<p>%s: %s\r\n"
                      "<hr><em>CS:APP Proxy</em>\r\n",
                      errnum, shortmsg, longmsg, cause);
  if (body_len < 0) {
    return;
  }

  header_len = snprintf(buf, sizeof(buf),
                        "HTTP/1.0 %s %s\r\n"
                        "Content-type: text/html\r\n"
                        "Content-length: %d\r\n"
                        "\r\n",
                        errnum, shortmsg, body_len);
  if (header_len < 0) {
    return;
  }

  send_all(fd, buf, (size_t)header_len);
  send_all(fd, body, (size_t)body_len);
}

static ssize_t send_all(int fd, const void *buf, size_t n) {
  size_t nleft = n;
  ssize_t nwritten;
  const char *bufp = (const char *)buf;

  while (nleft > 0) {
    nwritten = write(fd, bufp, nleft);
    if (nwritten < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (nwritten == 0) {
      return -1;
    }
    nleft -= (size_t)nwritten;
    bufp += nwritten;
  }

  return (ssize_t)n;
}

static void cache_init(void) {
  int i;

  for (i = 0; i < CACHE_ENTRY_COUNT; i++) {
    cache[i].valid = 0;
    cache[i].uri[0] = '\0';
    cache[i].object = NULL;
    cache[i].size = 0;
    cache[i].stamp = 0;
  }
}

static bool cache_try_serve(int fd, const char *uri) {
  int index;
  char *copy = NULL;
  size_t size = 0;

  pthread_mutex_lock(&cache_mutex);
  index = cache_find_index_locked(uri);
  if (index >= 0) {
    size = cache[index].size;
    if (size > 0) {
      copy = Malloc(size);
      memcpy(copy, cache[index].object, size);
    }
    cache[index].stamp = cache_clock++;
  }
  pthread_mutex_unlock(&cache_mutex);

  if (index < 0) {
    return false;
  }

  if (size > 0) {
    send_all(fd, copy, size);
    Free(copy);
  }
  return true;
}

static void cache_store(const char *uri, const char *buf, size_t size) {
  int i;
  int victim;

  if (size == 0 || size > MAX_OBJECT_SIZE) {
    return;
  }

  pthread_mutex_lock(&cache_mutex);

  i = cache_find_index_locked(uri);
  if (i >= 0) {
    cache_bytes_used -= cache[i].size;
    Free(cache[i].object);
    cache[i].object = NULL;
    cache[i].size = 0;
    cache[i].valid = 0;
  }

  while (cache_bytes_used + size > MAX_CACHE_SIZE) {
    victim = cache_select_victim_locked();
    if (victim < 0) {
      break;
    }
    cache_bytes_used -= cache[victim].size;
    Free(cache[victim].object);
    cache[victim].object = NULL;
    cache[victim].size = 0;
    cache[victim].valid = 0;
    cache[victim].uri[0] = '\0';
    cache[victim].stamp = 0;
  }

  victim = -1;
  for (i = 0; i < CACHE_ENTRY_COUNT; i++) {
    if (!cache[i].valid) {
      victim = i;
      break;
    }
  }
  if (victim < 0) {
    victim = cache_select_victim_locked();
    if (victim >= 0) {
      cache_bytes_used -= cache[victim].size;
      Free(cache[victim].object);
      cache[victim].object = NULL;
      cache[victim].size = 0;
      cache[victim].valid = 0;
      cache[victim].uri[0] = '\0';
      cache[victim].stamp = 0;
    }
  }

  if (victim >= 0) {
    cache[victim].object = Malloc(size);
    memcpy(cache[victim].object, buf, size);
    snprintf(cache[victim].uri, sizeof(cache[victim].uri), "%s", uri);
    cache[victim].size = size;
    cache[victim].stamp = cache_clock++;
    cache[victim].valid = 1;
    cache_bytes_used += size;
  }

  pthread_mutex_unlock(&cache_mutex);
}

static int cache_find_index_locked(const char *uri) {
  int i;

  for (i = 0; i < CACHE_ENTRY_COUNT; i++) {
    if (cache[i].valid && !strcmp(cache[i].uri, uri)) {
      return i;
    }
  }
  return -1;
}

static int cache_select_victim_locked(void) {
  int i;
  int victim = -1;
  unsigned long oldest = 0;

  for (i = 0; i < CACHE_ENTRY_COUNT; i++) {
    if (!cache[i].valid) {
      continue;
    }
    if (victim < 0 || cache[i].stamp < oldest) {
      victim = i;
      oldest = cache[i].stamp;
    }
  }
  return victim;
}
