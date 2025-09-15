#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <stdbool.h>
#include <semaphore.h>

#include "csapp.h"

#define MAXLINE 8192
#define MAX_OBJECT_SIZE 102400
#define MAX_CACHE_SIZE 1049000
#define MAX_CACHE 10
#define NTHREADS 4
#define SBUFSIZE 16

static const char *user_agent_hdr = 
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64; rv:10.0.3) "
    "Gecko/20120305 Firefox/10.0.3\r\n";

typedef struct URL {
    char host[MAXLINE];
    char port[MAXLINE];
    char path[MAXLINE];
} URL;

typedef struct {
    int *buf;
    int n;
    int front;
    int rear;
    sem_t mutex;
    sem_t slots;
    sem_t items;
} sbuf_t;

typedef struct Cache {
    bool empty;
    URL url;
    char data[MAX_OBJECT_SIZE];
    int lru;
    int read_cnt;
    sem_t mutex, w;
} Cache;

sbuf_t sbuf;
Cache ca[MAX_CACHE];

bool urlEqual(const URL *a, const URL *b);
void urlCopy(URL *a, const URL *b);
void sbuf_init(sbuf_t *sp, int n);
void sbuf_deinit(sbuf_t *sp);
void sbuf_insert(sbuf_t *sp, int item);
int sbuf_remove(sbuf_t *sp);
void doit(int connfd);
void parseUrl(char *s, URL *url);
void readClient(rio_t *rio, URL *url, char *data);
void thread(void *vargp);
void readBegin(Cache *c);
void readEnd(Cache *c);
void writeBegin(Cache *c);
void writeEnd(Cache *c);
void initCache();
Cache *getCache(URL *url);
void insCache(URL *url, char *data);
void updateLRU(Cache *c);
void fillCache(Cache *c, URL *url, char *data);

int main(int argc, char **argv) 
{
    int listenfd, connfd;
    char hostname[MAXLINE], port[MAXLINE];
    socklen_t clientlen;
    struct sockaddr_storage clientaddr;
    pthread_t tid;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <port>\n", argv[0]);
        exit(1);
    }

    sbuf_init(&sbuf, SBUFSIZE);
    for (int i = 0; i < NTHREADS; ++i)
        Pthread_create(&tid, NULL, thread, NULL);
    initCache();

    listenfd = Open_listenfd(argv[1]);
    while (1) {
        clientlen = sizeof(clientaddr);
        connfd = Accept(listenfd, (SA *)&clientaddr, &clientlen);
        Getnameinfo((SA *) &clientaddr, clientlen, hostname, MAXLINE, 
                    port, MAXLINE, 0);
        printf("Accepted connection from (%s, %s)\n", hostname, port);
        sbuf_insert(&sbuf, connfd);
    }
}

void thread(void *vargp) {
    Pthread_detach(pthread_self());
    while (1) {
        int connfd = sbuf_remove(&sbuf);
        doit(connfd);
        Close(connfd);
    }
}

void doit(int connfd) {
    rio_t rio;
    char line[MAXLINE];
    Rio_readinitb(&rio, connfd);
    
    URL url;
    char data[MAXLINE];
    readClient(&rio, &url, data);

    Cache *c;
    if ((c = getCache(&url)) != NULL) {
        Rio_writen(connfd, c->data, strlen(c->data));
        return;
    }

    int serverfd = Open_clientfd(url.host, url.port);
    if (serverfd < 0) {
        printf("Connection failed!\n");
        return;
    }

    rio_t server_rio;
    Rio_readinitb(&server_rio, serverfd);
    Rio_writen(serverfd, data, strlen(data));

    char buf[MAX_OBJECT_SIZE];
    int len, sumlen = 0;
    while ((len = Rio_readnb(&server_rio, line, MAXLINE)) > 0) {
        Rio_writen(connfd, line, len);
        if (sumlen + len < MAX_OBJECT_SIZE) {
            memcpy(buf + sumlen, line, len);
        }
        sumlen += len;
    }

    insCache(&url, buf);

    Close(serverfd);
}

void parseUrl(char *s, URL *url) {
    char *ptr = strstr(s, "//");
    if (ptr != NULL) s = ptr + 2;

    ptr = strchr(s, '/');
    if (ptr != NULL) {
        strcpy(url->path, ptr);
        *ptr = '\0';
    } else strcpy(url->path, "/");
    
    ptr = strchr(s, ':');
    if (ptr != NULL) {
        strcpy(url->port, ptr + 1);
        *ptr = '\0';
    } else strcpy(url->port, "80");

    strcpy(url->host, s);
}

void readClient(rio_t *rio, URL *url, char *data) {
    char host[MAXLINE] = "", other[MAXLINE] = "";
    char line[MAXLINE], method[MAXLINE], urlstr[MAXLINE], version[MAXLINE];

    Rio_readlineb(rio, line, MAXLINE);
    sscanf(line, "%s %s %s", method, urlstr, version);
    parseUrl(urlstr, url);

    sprintf(host, "Host: %s\r\n", url->host);
    while (Rio_readlineb(rio, line, MAXLINE) > 0) {
        if (strcmp(line, "\r\n") == 0) break;
        if (strncmp(line, "Host", 4) == 0) strcpy(host, line);
        if (strncmp(line, "User-Agent", 10) &&
            strncmp(line, "Connection", 10) &&
            strncmp(line, "Proxy-Connection", 16)) strcat(other, line);
    }

    sprintf(data, "%s %s HTTP/1.0\r\n%s%sConnection: close\r\nProxy-Connection: close\r\n%s\r\n",
        method, url->path, host, user_agent_hdr, other);
}

/* --------------------- Cache Implementation --------------------- */
void initCache() {
    for (int i = 0; i < MAX_CACHE; ++i) {
        ca[i].empty = true;
        Sem_init(&ca[i].mutex, 0, 1);
        Sem_init(&ca[i].w, 0, 1);
        ca[i].read_cnt = 0;
        ca[i].lru = 0;
    }
}

Cache *getCache(URL *url) {
    Cache *ans = NULL;
    for (int i = 0; i < MAX_CACHE && ans == NULL; ++i) {
        readBegin(&ca[i]);
        if (!ca[i].empty && urlEqual(&ca[i].url, url)) ans = &ca[i];
        readEnd(&ca[i]);
    }
    if (ans != NULL) updateLRU(ans);
    return ans;
}

void insCache(URL *url, char *data) {
    Cache *pos = NULL;
    for (int i = 0; i < MAX_CACHE && pos == NULL; ++i) {
        readBegin(&ca[i]);
        if (ca[i].empty) pos = &ca[i];
        readEnd(&ca[i]);
    }
    if (pos != NULL) {
        fillCache(pos, url, data);
        return;
    }

    int minLRU = __INT_MAX__;
    for (int i = 0; i < MAX_CACHE; ++i) {
        readBegin(&ca[i]);
        if (!ca[i].empty && ca[i].lru < minLRU) {
            minLRU = ca[i].lru;
            pos = &ca[i];
        }
        readEnd(&ca[i]);
    }
    fillCache(pos, url, data);
}

void updateLRU(Cache *c) {
    static int clock = 0;
    writeBegin(c);
    c->lru = ++clock;
    writeEnd(c);
}

void fillCache(Cache *c, URL *url, char *data) {
    writeBegin(c);
    c->empty = false;
    urlCopy(&c->url, url);
    strcpy(c->data, data);
    writeEnd(c);
    updateLRU(c);
}

bool urlEqual(const URL *a, const URL *b) {
    return strcmp(a->host, b->host) == 0 &&
           strcmp(a->port, b->port) == 0 &&
           strcmp(a->path, b->path) == 0;
}
void urlCopy(URL *a, const URL *b) {
    strcpy(a->host, b->host);
    strcpy(a->port, b->port);
    strcpy(a->path, b->path);
}

void sbuf_init(sbuf_t *sp, int n) {
    sp->buf = Calloc(n, sizeof(int));
    sp->n = n;
    sp->front = sp->rear = 0;
    Sem_init(&sp->mutex, 0, 1);
    Sem_init(&sp->slots, 0, n);
    Sem_init(&sp->items, 0, 0);
}

void sbuf_deinit(sbuf_t *sp) { Free(sp->buf); }
void sbuf_insert(sbuf_t *sp, int item) {
    P(&sp->slots);
    P(&sp->mutex);
    sp->buf[(++sp->rear) % sp->n] = item;
    V(&sp->mutex);
    V(&sp->items);
}
int sbuf_remove(sbuf_t *sp) {
    int item;
    P(&sp->items);
    P(&sp->mutex);
    item = sp->buf[(++sp->front) % sp->n];
    V(&sp->mutex);
    V(&sp->slots);
    return item;
}

void readBegin(Cache *c) {
    P(&c->mutex);
    if (++c->read_cnt == 1) P(&c->w);
    V(&c->mutex);
}
void readEnd(Cache *c) {
    P(&c->mutex);
    if (--c->read_cnt == 0) V(&c->w);
    V(&c->mutex);
}
void writeBegin(Cache *c) { P(&c->w); }
void writeEnd(Cache *c) { V(&c->w); }
