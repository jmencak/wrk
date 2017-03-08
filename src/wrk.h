#ifndef WRK_H
#define WRK_H

#include "config.h"
#include <pthread.h>
#include <inttypes.h>
#include <sys/types.h>
#include <netdb.h>
#include <sys/socket.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <luajit-2.0/lua.h>

#include "stats.h"
#include "ae.h"
#include "http_parser.h"

#define RECVBUF  8192

#define MAX_THREAD_RATE_S   10000000
#define SOCKET_TIMEOUT_MS   2000
#define RECORD_INTERVAL_MS  100

#define SOCK_CONNECT(c, host) (c->thread->ssl? ssl_connect(c, host): sock_connect(c, host))
#define SOCK_CLOSE(c) (c->thread->ssl? ssl_close(c): sock_close(c))
#define SOCK_READ(c, n) (c->thread->ssl? ssl_read(c, n): sock_read(c, n))
#define SOCK_WRITE(c, buf, len, n) (c->thread->ssl? ssl_write(c, buf, len, n): sock_write(c, buf, len, n))
#define SOCK_READABLE(c) (c->thread->ssl? ssl_readable(c): sock_readable(c))

extern const char *VERSION;


typedef struct {
    SSL_SESSION * cached_session; /* only cache 1 SSL_SESSION*/
} tls_session_cache;

typedef struct {
    pthread_t thread;
    aeEventLoop *loop;
    struct addrinfo *addr;
    bool ssl;
    char *host;
    char addrf[16];	// 127.127.127.127
    uint64_t connections;
    uint64_t complete;
    uint64_t requests;
    uint64_t bytes;
    uint64_t start;
    lua_State *L;
    errors errors;
    tls_session_cache cache;
    struct connection *cs;
} thread;

typedef struct {
    char  *buffer;
    size_t length;
    char  *cursor;
} buffer;

typedef struct connection {
    thread *thread;
    http_parser parser;
    enum {
        FIELD, VALUE
    } state;
    int fd;
    SSL *ssl;
    tls_session_cache *cache;
    bool delayed;
    uint64_t start;		// time [us] since the Epoch a request was sent
    struct {
        uint64_t start;		// time [us] since the Epoch we first tried to establish this connection
        uint64_t delay_est;	// time [us] it took to establish this connection (connection establishment delay)
        uint64_t delay_req;	// time [us] since the Epoch the socket was writeable but we were instructed to delay this request
        uint64_t reqs;		// number of requests sent over this connection
    } cstats;
    char *request;
    size_t length;
    size_t written;
    uint64_t pending;
    buffer headers;
    buffer body;
    char buf[RECVBUF];
} connection;

#endif /* WRK_H */
