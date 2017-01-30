// Copyright (C) 2012 - Will Glozer.  All rights reserved.

#include "wrk.h"
#include "script.h"
#include "main.h"

static struct config {
    uint64_t connections;
    uint64_t duration;
    uint64_t threads;
    uint64_t timeout;
    uint64_t pipeline;
    bool     delay;
    bool     dynamic;
    bool     quiet;
    bool     latency;
    bool     tls_session_reuse;
    bool     no_keep_alive;
    char    *host;
    char    *script;
    SSL_CTX *ctx;
} cfg;

static struct {
    stats *latency;
    stats *requests;
} statistics;

static struct http_parser_settings parser_settings = {
    .on_message_complete = response_complete
};

static volatile sig_atomic_t stop = 0;

static void handler(int sig) {
    stop = 1;
}

static void usage() {
    printf("Usage: wrk <options> <url>                            \n"
           "  Options:                                            \n"
           "    -c, --connections <N>  Connections to keep open   \n"
           "    -d, --duration    <T>  Duration of test           \n"
           "    -q, --quiet            Quiet mode                 \n"
           "    -t, --threads     <N>  Number of threads to use   \n"
           "    -r, --reuse       <R>  Enable tls session reuse   \n"
           "    -k, --no_keepalive <K> Disable http keep-alive    \n"
           "                                                      \n"
           "    -s, --script      <S>  Load Lua script file       \n"
           "    -H, --header      <H>  Add header to request      \n"
           "        --latency          Print latency statistics   \n"
           "        --timeout     <T>  Socket/request timeout     \n"
           "    -v, --version          Print version details      \n"
           "                                                      \n"
           "  Numeric arguments may include a SI unit (1k, 1M, 1G)\n"
           "  Time arguments may include a time unit (2s, 2m, 2h)\n");
}

int main(int argc, char **argv) {
    char *url, **headers = zmalloc(argc * sizeof(char *));
    struct http_parser_url parts = {};
    bool ssl = false;

    if (parse_args(&cfg, &url, &parts, headers, argc, argv)) {
        usage();
        exit(1);
    }

    char *schema  = copy_url_part(url, &parts, UF_SCHEMA);
    char *host    = copy_url_part(url, &parts, UF_HOST);
    char *port    = copy_url_part(url, &parts, UF_PORT);
    char *service = port ? port : schema;

    if ((cfg.ctx = ssl_init()) == NULL) {
        fprintf(stderr, "unable to initialize SSL\n");
        ERR_print_errors_fp(stderr);
        exit(1);
    }
    if (!strncmp("https", schema, 5)) {
        ssl = true;
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT,  SIG_IGN);

    statistics.latency  = stats_alloc(cfg.timeout * 1000);
    statistics.requests = stats_alloc(MAX_THREAD_RATE_S);
    thread *threads     = zcalloc(cfg.threads * sizeof(thread));

    lua_State *L = script_create(cfg.script, url, headers);
    if (!script_resolve(L, host, service)) {
        char *msg = strerror(errno);
        fprintf(stderr, "unable to connect to %s:%s %s\n", host, service, msg);
        exit(1);
    }

    cfg.host = host;

    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t      = &threads[i];
        t->ssl         = ssl;
        t->loop        = aeCreateEventLoop(10 + cfg.connections * 3);
        t->connections = cfg.connections / cfg.threads;

        t->L = script_create(cfg.script, url, headers);

        script_init(L, t, argc - optind, &argv[optind]);

        if (i == 0) {
            cfg.pipeline = script_verify_request(t->L);
            cfg.dynamic  = !script_is_static(t->L);
            cfg.delay    = script_has_delay(t->L);
            if (script_want_response(t->L)) {
                parser_settings.on_header_field = header_field;
                parser_settings.on_header_value = header_value;
                parser_settings.on_body         = response_body;
            }
        }

        if (!t->loop || pthread_create(&t->thread, NULL, &thread_main, t)) {
            char *msg = strerror(errno);
            fprintf(stderr, "unable to create thread %"PRIu64": %s\n", i, msg);
            exit(2);
        }
    }

    struct sigaction sa = {
        .sa_handler = handler,
        .sa_flags   = 0,
    };
    sigfillset(&sa.sa_mask);
    sigaction(SIGINT, &sa, NULL);

    char *time = format_time_s(cfg.duration);
    if (!cfg.quiet) {
        printf("Running %s test @ %s\n", time, url);
        printf("  %"PRIu64" threads and %"PRIu64" connections. session reuse:%s, keep-alive:%s\n", cfg.threads, cfg.connections,
                cfg.tls_session_reuse?"enabled":"disabled", cfg.no_keep_alive?"disabled":"enabled");
    }

    uint64_t start    = time_us();
    uint64_t complete = 0;
    uint64_t bytes    = 0;
    errors errors     = { 0 };

    sleep(cfg.duration);	// wait for the threads to do their job
    stop = 1;

    // collect the threads
    for (uint64_t i = 0; i < cfg.threads; i++) {
        thread *t = &threads[i];
        pthread_join(t->thread, NULL);

        complete += t->complete;
        bytes    += t->bytes;

        errors.connect += t->errors.connect;
        errors.read    += t->errors.read;
        errors.write   += t->errors.write;
        errors.timeout += t->errors.timeout;
        errors.status  += t->errors.status;
    }

    uint64_t runtime_us = time_us() - start;
    long double runtime_s   = runtime_us / 1000000.0;
    long double req_per_s   = complete   / runtime_s;
    long double bytes_per_s = bytes      / runtime_s;

    if (complete / cfg.connections > 0) {
        int64_t interval = runtime_us / (complete / cfg.connections);
        stats_correct(statistics.latency, interval);
    }

    if (!cfg.quiet) {
        print_stats_header();
        print_stats("Latency", statistics.latency, format_time_us);
        print_stats("Req/Sec", statistics.requests, format_metric);
    }

    if (cfg.latency) print_stats_latency(statistics.latency);

    char *runtime_msg = format_time_us(runtime_us);

    if (!cfg.quiet) {
        printf("  %"PRIu64" requests in %s, %sB read\n", complete, runtime_msg, format_binary(bytes));
        if (errors.connect || errors.read || errors.write || errors.timeout) {
            printf("  Socket errors: connect %d, read %d, write %d, timeout %d\n",
                   errors.connect, errors.read, errors.write, errors.timeout);
        }

        if (errors.status) {
            printf("  Non-2xx or 3xx responses: %d\n", errors.status);
        }

        printf("Requests/sec: %9.2Lf\n", req_per_s);
        printf("Transfer/sec: %10sB\n", format_binary(bytes_per_s));
    }

    if(!cfg.quiet && cfg.ctx) {
        printf("TLS new conn %d reused %d miss %d finished conn %d sess_cb_hit %d renegotiation %d timeout %d full remove %d \n",
                cfg.ctx->stats.sess_connect,
                cfg.ctx->stats.sess_hit,
                cfg.ctx->stats.sess_miss,
                cfg.ctx->stats.sess_connect_good,
                cfg.ctx->stats.sess_cb_hit,
                cfg.ctx->stats.sess_connect_renegotiate,
                cfg.ctx->stats.sess_timeout,
                cfg.ctx->stats.sess_cache_full
              );
    }

    if (script_has_done(L)) {
        script_summary(L, runtime_us, complete, bytes);
        script_errors(L, &errors);
        script_done(L, statistics.latency, statistics.requests);
    }

    stats_free(statistics.latency);
    stats_free(statistics.requests);
    return 0;
}

void *thread_main(void *arg) {
    thread *thread = arg;

    char *request = NULL;
    size_t length = 0;

    if (!cfg.dynamic) {
        script_request(thread->L, &request, &length);
    }

    thread->cs = zcalloc(thread->connections * sizeof(connection));
    connection *c = thread->cs;

    for (uint64_t i = 0; i < thread->connections; i++, c++) {
        c->thread  = thread;
        c->request = request;
        c->length  = length;
        c->delayed = cfg.delay;
        connect_socket(thread, c);
    }

    aeEventLoop *loop = thread->loop;
    aeCreateTimeEvent(loop, RECORD_INTERVAL_MS, record_rate, thread, NULL);

    thread->start = time_us();
    aeMain(loop);

    SSL_SESSION_free(thread->cache.cached_session);
    aeDeleteEventLoop(loop);
    zfree(thread->cs);   thread->cs = NULL;
    zfree(thread->host); thread->host = NULL;

    return NULL;
}

static int connect_socket(thread *thread, connection *c) {
    struct addrinfo *addr = thread->addr;
    struct aeEventLoop *loop = thread->loop;
    int fd, flags;

    int rc;
    struct addrinfo *addrf;
    char *source_addr = thread->addrf;
    struct addrinfo hints = {
        .ai_family   = AF_UNSPEC,
        .ai_socktype = SOCK_STREAM
    };

    fd = socket(addr->ai_family, addr->ai_socktype, addr->ai_protocol);

    if (source_addr && *source_addr) {
        if ((rc = getaddrinfo(source_addr, NULL, &hints, &addrf)) != 0) {
            const char *msg = gai_strerror(rc);
            fprintf(stderr, "unable to resolve source %s: %s\n", source_addr, msg);
            exit(1);
        }
    }

    flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);

    if (source_addr && *source_addr) {
        if ((rc = bind(fd, addrf->ai_addr, addrf->ai_addrlen)) != 0) {
            const char *msg = gai_strerror(rc);
            fprintf(stderr, "unable to bind source %s: %s\n", source_addr, msg);
            exit(1);
        }
    }

    if (connect(fd, addr->ai_addr, addr->ai_addrlen) == -1) {
        if (errno != EINPROGRESS) goto error;
    }

    flags = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flags, sizeof(flags));
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flags, sizeof(flags));


    if(cfg.ctx){
        c->ssl = cfg.ctx ? SSL_new(cfg.ctx) : NULL;
        SSL_set_ex_data(c->ssl,ssl_data_index,c);
        c->cache = cfg.tls_session_reuse? &thread->cache:NULL;
    }

    flags = AE_READABLE | AE_WRITABLE;
    if (aeCreateFileEvent(loop, fd, flags, socket_connected, c) == AE_OK) {
        c->parser.data = c;
        c->fd = fd;
        return fd;
    }

  error:
    thread->errors.connect++;
    close(fd);
    return -1;
}

static int reconnect_socket(thread *thread, connection *c) {
    aeDeleteFileEvent(thread->loop, c->fd, AE_WRITABLE | AE_READABLE);
    SOCK_CLOSE(c);
    close(c->fd);
    return connect_socket(thread, c);
}

static int record_rate(aeEventLoop *loop, long long id, void *data) {
    thread *thread = data;

    if (thread->requests > 0) {
        uint64_t elapsed_ms = (time_us() - thread->start) / 1000;
        uint64_t requests = (thread->requests / (double) elapsed_ms) * 1000;

        stats_record(statistics.requests, requests);

        thread->requests = 0;
        thread->start    = time_us();
    }

    if (stop) aeStop(loop);

    return RECORD_INTERVAL_MS;
}

static int delay_request(aeEventLoop *loop, long long id, void *data) {
    connection *c = data;
    c->delayed = false;
    aeCreateFileEvent(loop, c->fd, AE_WRITABLE, socket_writeable, c);
    return AE_NOMORE;
}

static int header_field(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == VALUE) {
        *c->headers.cursor++ = '\0';
        c->state = FIELD;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int header_value(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    if (c->state == FIELD) {
        *c->headers.cursor++ = '\0';
        c->state = VALUE;
    }
    buffer_append(&c->headers, at, len);
    return 0;
}

static int response_body(http_parser *parser, const char *at, size_t len) {
    connection *c = parser->data;
    buffer_append(&c->body, at, len);
    return 0;
}

static int response_complete(http_parser *parser) {
    connection *c = parser->data;
    thread *thread = c->thread;
    uint64_t now = time_us();
    int status = parser->status_code;

    thread->complete++;
    thread->requests++;

    if (status > 399) {
        thread->errors.status++;
    }

    if (c->headers.buffer) {
        *c->headers.cursor++ = '\0';
        script_response(thread->L, status, &c->headers, &c->body);
        c->state = FIELD;
    }

    if (--c->pending == 0) {
        if (!stats_record(statistics.latency, now - c->start)) {
            thread->errors.timeout++;
        }
        c->delayed = cfg.delay;
        aeCreateFileEvent(thread->loop, c->fd, AE_WRITABLE, socket_writeable, c);
    }

    if ((cfg.no_keep_alive) || (!http_should_keep_alive(parser))) {
        reconnect_socket(thread, c);
        goto done;
    }

    http_parser_init(parser, HTTP_RESPONSE);

  done:
    return 0;
}

static void socket_connected(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    char    *host = c->thread->host? c->thread->host: cfg.host;

    switch (SOCK_CONNECT(c, host)) {
        case OK:
        break;

        case ERROR:
        goto error;

        case RETRY:
        return;
    }

    http_parser_init(&c->parser, HTTP_RESPONSE);
    c->written = 0;

    aeCreateFileEvent(c->thread->loop, fd, AE_READABLE, socket_readable, c);
    aeCreateFileEvent(c->thread->loop, fd, AE_WRITABLE, socket_writeable, c);

    return;

  error:
    c->thread->errors.connect++;
    reconnect_socket(c->thread, c);
}

static void socket_writeable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    thread *thread = c->thread;

    if (c->delayed) {
        uint64_t delay = script_delay(thread->L);
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
        aeCreateTimeEvent(loop, delay, delay_request, c, NULL);
        return;
    }

    if (!c->written) {
        if (cfg.dynamic) {
            script_request(thread->L, &c->request, &c->length);
        }
        c->start   = time_us();
        c->pending = cfg.pipeline;
    }

    char  *buf = c->request + c->written;
    size_t len = c->length  - c->written;
    size_t n;

    switch (SOCK_WRITE(c, buf, len, &n)) {
        case OK:    break;
        case ERROR: goto error;
        case RETRY: return;
    }

    c->written += n;
    if (c->written == c->length) {
        c->written = 0;
        aeDeleteFileEvent(loop, fd, AE_WRITABLE);
    }

    return;

  error:
    thread->errors.write++;
    reconnect_socket(thread, c);
}

static void socket_readable(aeEventLoop *loop, int fd, void *data, int mask) {
    connection *c = data;
    size_t n;

    do {
        switch (SOCK_READ(c, &n)) {
            case OK:    break;
            case ERROR: 
              fprintf(stderr, "sock read error\n");

              goto error;
            case RETRY: return;
        }

        if (http_parser_execute(&c->parser, &parser_settings, c->buf, n) != n) goto error;
        if (n == 0 && !http_body_is_final(&c->parser)) goto error;

        c->thread->bytes += n;
    } while (n == RECVBUF && SOCK_READABLE(c) > 0);

    return;

  error:
    c->thread->errors.read++;
    reconnect_socket(c->thread, c);
}

static uint64_t time_us() {
    struct timeval t;
    gettimeofday(&t, NULL);
    return (t.tv_sec * 1000000) + t.tv_usec;
}

static char *copy_url_part(char *url, struct http_parser_url *parts, enum http_parser_url_fields field) {
    char *part = NULL;

    if (parts->field_set & (1 << field)) {
        uint16_t off = parts->field_data[field].off;
        uint16_t len = parts->field_data[field].len;
        part = zcalloc(len + 1 * sizeof(char));
        memcpy(part, &url[off], len);
    }

    return part;
}

static struct option longopts[] = {
    { "connections", required_argument, NULL, 'c' },
    { "duration",    required_argument, NULL, 'd' },
    { "threads",     required_argument, NULL, 't' },
    { "script",      required_argument, NULL, 's' },
    { "header",      required_argument, NULL, 'H' },
    { "quiet",       no_argument,       NULL, 'q' },
    { "latency",     no_argument,       NULL, 'L' },
    { "timeout",     required_argument, NULL, 'T' },
    { "reuse",       no_argument,       NULL, 'r' },
    { "no_keepalive",no_argument,       NULL, 'k' },
    { "help",        no_argument,       NULL, 'h' },
    { "version",     no_argument,       NULL, 'v' },
    { NULL,          0,                 NULL,  0  }
};

static int parse_args(struct config *cfg, char **url, struct http_parser_url *parts, char **headers, int argc, char **argv) {
    char **header = headers;
    int c;

    memset(cfg, 0, sizeof(struct config));
    cfg->threads     = 2;
    cfg->connections = 10;
    cfg->duration    = 10;
    cfg->timeout     = SOCKET_TIMEOUT_MS;

    while ((c = getopt_long(argc, argv, "t:c:d:s:H:T:qkLrv?", longopts, NULL)) != -1) {
        switch (c) {
            case 't':
                if (scan_metric(optarg, &cfg->threads)) return -1;
                break;
            case 'c':
                if (scan_metric(optarg, &cfg->connections)) return -1;
                break;
            case 'd':
                if (scan_time(optarg, &cfg->duration)) return -1;
                break;
            case 's':
                cfg->script = optarg;
                break;
            case 'H':
                *header++ = optarg;
                break;
            case 'q':
                cfg->quiet = true;
                break;
            case 'L':
                cfg->latency = true;
                break;
            case 'T':
                if (scan_time(optarg, &cfg->timeout)) return -1;
                cfg->timeout *= 1000;
                break;
            case 'v':
                printf("wrk %s [%s] ", VERSION, aeGetApiName());
                printf("Copyright (C) 2012 Will Glozer\n");
                break;
            case 'r':
                cfg->tls_session_reuse=true;
                break;
            case 'k':
                cfg->no_keep_alive=true;
                break;
            case 'h':
            case '?':
            case ':':
            default:
                return -1;
        }
    }

    if (optind == argc || !cfg->threads || !cfg->duration) return -1;

    if (!script_parse_url(argv[optind], parts)) {
        fprintf(stderr, "invalid URL: %s\n", argv[optind]);
        return -1;
    }

    if (!cfg->connections || cfg->connections < cfg->threads) {
        fprintf(stderr, "number of connections must be >= threads\n");
        return -1;
    }

    *url    = argv[optind];
    *header = NULL;

    return 0;
}

static void print_stats_header() {
    printf("  Thread Stats%6s%11s%8s%12s\n", "Avg", "Stdev", "Max", "+/- Stdev");
}

static void print_units(long double n, char *(*fmt)(long double), int width) {
    char *msg = fmt(n);
    int len = strlen(msg), pad = 2;

    if (isalpha(msg[len-1])) pad--;
    if (isalpha(msg[len-2])) pad--;
    width -= pad;

    printf("%*.*s%.*s", width, width, msg, pad, "  ");

    free(msg);
}

static void print_stats(char *name, stats *stats, char *(*fmt)(long double)) {
    uint64_t max = stats->max;
    long double mean  = stats_mean(stats);
    long double stdev = stats_stdev(stats, mean);

    printf("    %-10s", name);
    print_units(mean,  fmt, 8);
    print_units(stdev, fmt, 10);
    print_units(max,   fmt, 9);
    printf("%8.2Lf%%\n", stats_within_stdev(stats, mean, stdev, 1));
}

static void print_stats_latency(stats *stats) {
    long double percentiles[] = { 50.0, 75.0, 90.0, 99.0 };
    printf("  Latency Distribution\n");
    for (size_t i = 0; i < sizeof(percentiles) / sizeof(long double); i++) {
        long double p = percentiles[i];
        uint64_t n = stats_percentile(stats, p);
        printf("%7.0Lf%%", p);
        print_units(n, format_time_us, 10);
        printf("\n");
    }
}
