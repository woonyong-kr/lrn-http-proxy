/* $begin tinymain */
/*
 * tiny.c - GET 메서드를 사용해 정적/동적 콘텐츠를 제공하는
 *     단순한 반복형 HTTP/1.0 웹 서버
 *
 * 2019/11 droh 수정
 *   - serve_static()와 clienterror()에서 sprintf() 별칭 문제 수정
 */
#include "csapp.h"

void doit(int fd);
void read_requesthdrs(rio_t* rp);
int parse_uri(char* uri, char* filename, char* cgiargs);
void serve_static(int fd, char* filename, int filesize);
void get_filetype(char* filename, char* filetype);
void serve_dynamic(int fd, char* filename, char* cgiargs);
void clienterror(int fd, char* cause, char* errnum, char* shortmsg,
                 char* longmsg);

int main(int argc, char** argv) {
  int listenfd, connfd;
  char hostname[MAXLINE], port[MAXLINE];
  socklen_t clientlen;
  struct sockaddr_storage clientaddr;

  if (argc != 2) {
    fprintf(stderr, "usage: %s <port>\n", argv[0]);
    exit(1);
  }

  listenfd = Open_listenfd(argv[1]);
  while (1) {
    clientlen = sizeof(clientaddr);
    connfd = Accept(listenfd, (SA*)&clientaddr, &clientlen);
    Getnameinfo((SA*)&clientaddr, clientlen, hostname, MAXLINE, port, MAXLINE,
                0);
    printf("Accepted connection from (%s, %s)\n", hostname, port);
    doit(connfd);
    Close(connfd);
  }
}

void doit(int fd) {
  int is_static;
  struct stat sbuf;
  char buf[MAXLINE], method[MAXLINE], uri[MAXLINE], version[MAXLINE];
  char filename[MAXLINE], cgiargs[MAXLINE];
  rio_t rio;
  ssize_t n;

  Rio_readinitb(&rio, fd);
  n = Rio_readlineb(&rio, buf, MAXLINE);
  if (n <= 0) {
    return;
  }

  printf("Request headers:\n");
  printf("%s", buf);
  if (sscanf(buf, "%s %s %s", method, uri, version) != 3) {
    clienterror(fd, buf, "400", "Bad Request",
                "Tiny could not parse the request line");
    return;
  }
  (void)version;

  if (strcasecmp(method, "GET")) {
    clienterror(fd, method, "501", "Not implemented",
                "Tiny does not implement this method");
    return;
  }
  read_requesthdrs(&rio);

  is_static = parse_uri(uri, filename, cgiargs);
  if (stat(filename, &sbuf) < 0) {
    clienterror(fd, filename, "404", "Not found",
                "Tiny couldn't find this file");
    return;
  }

  if (is_static) {
    if (!S_ISREG(sbuf.st_mode) || !(S_IRUSR & sbuf.st_mode)) {
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn't read the file");
      return;
    }
    serve_static(fd, filename, (int)sbuf.st_size);
  } else {
    if (!S_ISREG(sbuf.st_mode) || !(S_IXUSR & sbuf.st_mode)) {
      clienterror(fd, filename, "403", "Forbidden",
                  "Tiny couldn't run the CGI program");
      return;
    }
    serve_dynamic(fd, filename, cgiargs);
  }
}

void read_requesthdrs(rio_t* rp) {
  char buf[MAXLINE];

  do {
    Rio_readlineb(rp, buf, MAXLINE);
    printf("%s", buf);
  } while (strcmp(buf, "\r\n"));
}

int parse_uri(char* uri, char* filename, char* cgiargs) {
  char* ptr;

  if (!strstr(uri, "cgi-bin")) {
    strcpy(cgiargs, "");
    snprintf(filename, MAXLINE, ".%s", uri);
    if (uri[strlen(uri) - 1] == '/') {
      strncat(filename, "home.html", MAXLINE - strlen(filename) - 1);
    }
    return 1;
  }

  ptr = strchr(uri, '?');
  if (ptr != NULL) {
    strcpy(cgiargs, ptr + 1);
    *ptr = '\0';
  } else {
    strcpy(cgiargs, "");
  }
  snprintf(filename, MAXLINE, ".%s", uri);
  return 0;
}

void serve_static(int fd, char* filename, int filesize) {
  int srcfd;
  char* srcp;
  char filetype[MAXLINE];
  char buf[MAXBUF];
  int len;

  get_filetype(filename, filetype);
  len = snprintf(buf, sizeof(buf),
                 "HTTP/1.0 200 OK\r\n"
                 "Server: Tiny Web Server\r\n"
                 "Cache-Control: public, max-age=2\r\n"
                 "Content-length: %d\r\n"
                 "Content-type: %s\r\n"
                 "\r\n",
                 filesize, filetype);
  Rio_writen(fd, buf, (size_t)len);

  if (filesize == 0) {
    return;
  }

  srcfd = Open(filename, O_RDONLY, 0);
  srcp = Mmap(0, (size_t)filesize, PROT_READ, MAP_PRIVATE, srcfd, 0);
  Close(srcfd);
  Rio_writen(fd, srcp, (size_t)filesize);
  Munmap(srcp, (size_t)filesize);
}

void get_filetype(char* filename, char* filetype) {
  if (strstr(filename, ".html")) {
    strcpy(filetype, "text/html");
  } else if (strstr(filename, ".gif")) {
    strcpy(filetype, "image/gif");
  } else if (strstr(filename, ".png")) {
    strcpy(filetype, "image/png");
  } else if (strstr(filename, ".jpg") || strstr(filename, ".jpeg")) {
    strcpy(filetype, "image/jpeg");
  } else {
    strcpy(filetype, "text/plain");
  }
}

void serve_dynamic(int fd, char* filename, char* cgiargs) {
  char buf[MAXLINE];
  char* emptylist[] = {NULL};
  int len;

  len = snprintf(buf, sizeof(buf),
                 "HTTP/1.0 200 OK\r\n"
                 "Server: Tiny Web Server\r\n"
                 "Cache-Control: public, max-age=2\r\n");
  Rio_writen(fd, buf, (size_t)len);

  if (Fork() == 0) {
    if (setenv("QUERY_STRING", cgiargs, 1) < 0) {
      unix_error("setenv error");
    }
    Dup2(fd, STDOUT_FILENO);
    Execve(filename, emptylist, environ);
  }
  Wait(NULL);
}

void clienterror(int fd, char* cause, char* errnum, char* shortmsg,
                 char* longmsg) {
  char buf[MAXLINE], body[MAXLINE];
  int body_len, header_len;

  body_len = snprintf(body, sizeof(body),
                      "<html><title>Tiny Error</title>"
                      "<body bgcolor=\"ffffff\">\r\n"
                      "%s: %s\r\n"
                      "<p>%s: %s\r\n"
                      "<hr><em>The Tiny Web server</em>\r\n",
                      errnum, shortmsg, longmsg, cause);

  header_len = snprintf(buf, sizeof(buf),
                        "HTTP/1.0 %s %s\r\n"
                        "Content-type: text/html\r\n"
                        "Content-length: %d\r\n"
                        "\r\n",
                        errnum, shortmsg, body_len);
  Rio_writen(fd, buf, (size_t)header_len);
  Rio_writen(fd, body, (size_t)body_len);
}
