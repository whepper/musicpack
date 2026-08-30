/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  * Neither the name of the MusicPack Development Team nor the names of
  its contributors may be used to endorse or promote products derived
  from this software without specific prior written permission.

  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
 * Tests for the MPAK HTTP Range adapter (mpakhttp), run against a
 * deterministic loopback HTTP mock server. The server speaks real HTTP
 * over real sockets so adapter behavior (status/Content-Range/ETag
 * validation, no-Range fallback, encoding rejection, changed objects)
 * is exercised at the protocol level, with scripted raw responses for
 * malformed-server scenarios.
 *
 * Usage: mpakhttp_tests <album.mpack dir> <fixture.mpc>
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(_WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
# include <windows.h>
# define SOCK_CLOSE closesocket
# define SOCK_READ(s, b, n) recv(s, b, (int)(n), 0)
# define SOCK_WRITE(s, b, n) send(s, b, (int)(n), 0)
# define SOCK_INVALID INVALID_SOCKET
# define MTX CRITICAL_SECTION
typedef SOCKET sock_t;
#else
# include <arpa/inet.h>
# include <dirent.h>
# include <netinet/in.h>
# include <signal.h>
# include <sys/socket.h>
# include <sys/stat.h>
# include <unistd.h>
# define SOCK_CLOSE close
# define SOCK_READ(s, b, n) recv(s, b, n, 0)
# define SOCK_WRITE(s, b, n) send(s, b, n, 0)
# define SOCK_INVALID (-1)
# define MTX pthread_mutex_t
typedef int sock_t;
# include <pthread.h>
#endif

#include <musicpack/musicpack.h>
#include <musepack/musepack.h>
#include <mpakhttp/mpakhttp.h>

static int failures = 0;

#define CHECK(cond, msg)                                                     \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static int
make_temp_dir(char *buf, size_t cap)
{
#if defined(_WIN32)
    const char *base = getenv("TEMP");
    if (base == 0) base = ".";
    if (snprintf(buf, cap, "%s\\mpakhttp_test_%lu", base,
                 (unsigned long) GetCurrentProcessId()) >= (int) cap)
        return -1;
    if (CreateDirectoryA(buf, 0) == 0 && GetLastError() != ERROR_ALREADY_EXISTS)
        return -1;
    return 0;
#else
    if (snprintf(buf, cap, "/tmp/mpakhttp_test_XXXXXX") >= (int) cap)
        return -1;
    return mkdtemp(buf) != 0 ? 0 : -1;
#endif
}

static int
mkdir_p(const char *path)
{
    char tmp[4200];
    size_t len, i;

    len = strlen(path);
    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, path, len + 1);
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            char c = tmp[i];
            tmp[i] = '\0';
#ifdef _WIN32
            if (CreateDirectoryA(tmp, 0) == 0 && GetLastError() != ERROR_ALREADY_EXISTS)
                return -1;
#else
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
                return -1;
#endif
            tmp[i] = c;
        }
    }
#ifdef _WIN32
    if (CreateDirectoryA(tmp, 0) == 0 && GetLastError() != ERROR_ALREADY_EXISTS)
        return -1;
#else
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST)
        return -1;
#endif
    return 0;
}

static int
write_file(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == 0)
        return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    return fclose(f) == 0 ? 0 : -1;
}

static unsigned char *
read_file_all(const char *path, size_t *len_out)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;

    if (f == 0)
        return 0;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return 0;
    }
    buf = (unsigned char *) malloc(len > 0 ? (size_t) len : 1);
    if (buf == 0) {
        fclose(f);
        return 0;
    }
    if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    *len_out = (size_t) len;
    return buf;
}

static void
join_path(char *out, size_t cap, const char *a, const char *b)
{
    snprintf(out, cap, "%s/%s", a, b);
}

/* removes a file or a directory tree (the tests' temp dir) */
static void
remove_tree(const char *path)
{
#ifdef _WIN32
    char glob[4200];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    snprintf(glob, sizeof glob, "%s\\*", path);
    h = FindFirstFileA(glob, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            char sub[4200];
            if (strcmp(fd.cFileName, ".") == 0 ||
                strcmp(fd.cFileName, "..") == 0)
                continue;
            snprintf(sub, sizeof sub, "%s\\%s", path, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
                remove_tree(sub);
            else
                DeleteFileA(sub);
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
#else
    DIR *d = opendir(path);
    struct dirent *e;

    if (d == 0) {
        unlink(path);
        return;
    }
    while ((e = readdir(d)) != 0) {
        char sub[4200];
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(sub, sizeof sub, "%s/%s", path, e->d_name);
        remove_tree(sub);
    }
    closedir(d);
    rmdir(path);
#endif
}

/* Minimal one-track directory package builder (audio = fixture, padded
   with `pad` zero bytes so member reads cross 64 KiB boundaries). */
static int
build_min_package(const char *root, const char *audio_src, size_t pad,
                  const char *audio_rel, const char *extra_rel)
{
    char path[4200], hash[MUSICPACK_SHA256_HEX_SIZE];
    char manifest[2048];
    unsigned char *mpc;
    size_t mpc_len;

    if (mkdir_p(root) != 0)
        return -1;
    join_path(path, sizeof path, root, "audio");
    if (mkdir_p(path) != 0)
        return -1;
    join_path(path, sizeof path, root, audio_rel);
    mpc = read_file_all(audio_src, &mpc_len);
    if (mpc == 0)
        return -1;
    if (pad > 0) {
        unsigned char *padded = (unsigned char *) calloc(1, mpc_len + pad);
        if (padded == 0) {
            free(mpc);
            return -1;
        }
        memcpy(padded, mpc, mpc_len);
        free(mpc);
        mpc = padded;
        mpc_len += pad;
    }
    if (write_file(path, mpc, mpc_len) != 0) {
        free(mpc);
        return -1;
    }
    free(mpc);
    if (musicpack_sha256_file(path, hash, sizeof hash) != MUSICPACK_OK)
        return -1;

    if (extra_rel != 0) {
        char ehash[MUSICPACK_SHA256_HEX_SIZE];
        char dir[4200];
        join_path(path, sizeof path, root, extra_rel);
        {
            char *slash = strrchr(path, '/');
            if (slash != 0) {
                size_t n = (size_t) (slash - path);
                memcpy(dir, path, n);
                dir[n] = '\0';
                if (mkdir_p(dir) != 0)
                    return -1;
            }
        }
        if (write_file(path, "note\n", 5) != 0)
            return -1;
        if (musicpack_sha256_file(path, ehash, sizeof ehash) != MUSICPACK_OK)
            return -1;
        snprintf(manifest, sizeof manifest,
                 "{\n"
                 "  \"album\": { \"artists\": [ { \"name\": \"T\" } ], \"title\": \"M\" },\n"
                 "  \"format\": \"musicpack\",\n"
                 "  \"media\": [ { \"disc\": 1, \"tracks\": [ { \"audio\": { \"path\": \"%s\", \"sha256\": \"%s\" }, \"title\": \"t\", \"track\": 1 } ] } ],\n"
                 "  \"extras\": [ { \"path\": \"%s\", \"sha256\": \"%s\" } ],\n"
                 "  \"version\": 1\n"
                 "}\n",
                 audio_rel, hash, extra_rel, ehash);
    } else {
        snprintf(manifest, sizeof manifest,
                 "{\n"
                 "  \"album\": { \"artists\": [ { \"name\": \"T\" } ], \"title\": \"M\" },\n"
                 "  \"format\": \"musicpack\",\n"
                 "  \"media\": [ { \"disc\": 1, \"tracks\": [ { \"audio\": { \"path\": \"%s\", \"sha256\": \"%s\" }, \"title\": \"t\", \"track\": 1 } ] } ],\n"
                 "  \"version\": 1\n"
                 "}\n",
                 audio_rel, hash);
    }
    join_path(path, sizeof path, root, "manifest.json");
    return write_file(path, manifest, strlen(manifest));
}

/* ------------------------------------------------------------------ */
/* loopback mock HTTP server                                           */
/* ------------------------------------------------------------------ */

#ifdef _WIN32
# define THREAD_HANDLE HANDLE
#else
# define THREAD_HANDLE pthread_t
#endif
#if !defined(_WIN32)
static void *
mock_thread(void *arg);
#else
static DWORD WINAPI
mock_thread(LPVOID arg);
#endif

typedef struct mock_raw_response {
    char *raw;
    size_t len;
} mock_raw_response;

struct mock_srv {
    /* container served in auto mode */
    unsigned char *data;
    size_t size;
    char etag[128];
    /* quirks (written only through the lock-protected setters) */
    long total_delta;          /* Content-Range total = size + delta */
    int short_body;            /* declare the range, truncate the body */
    int no_range_support;      /* answer ranged requests with 200 */
    int send_encoding;         /* attach Content-Encoding: gzip */
    long delay_ms;

    /* scripted raw responses (consumed FIFO, one per request) */
    mock_raw_response *raw;
    size_t raw_count, raw_next, raw_cap;

    /* request log */
    char ranges[256][96];
    size_t range_count;

    int port;
    sock_t listen_fd;
    volatile int stop;
    MTX lock;
    THREAD_HANDLE handle;
    int thread_valid;
};

static void
srv_lock(struct mock_srv *m)
{
#ifdef _WIN32
    EnterCriticalSection(&m->lock);
#else
    pthread_mutex_lock(&m->lock);
#endif
}

static void
srv_unlock(struct mock_srv *m)
{
#ifdef _WIN32
    LeaveCriticalSection(&m->lock);
#else
    pthread_mutex_unlock(&m->lock);
#endif
}

static void
srv_sleep_ms(long ms)
{
#ifdef _WIN32
    Sleep((DWORD) ms);
#else
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, 0);
#endif
}

/* extracts the value of a request header (case-insensitive) into
   `value`; returns `value` or NULL when absent */
static const char *
req_header(const char *req, const char *name, char *value, size_t cap)
{
    const char *p = req;
    size_t nlen = strlen(name);

    while ((p = strstr(p, name)) != 0) {
        /* must start a line */
        if (p == req || (p[-1] == '\n' && p[nlen] == ':')) {
            const char *v = p + nlen + 1;
            size_t n = 0;
            while (*v == ' ')
                v++;
            while (v[n] != '\0' && v[n] != '\r' && v[n] != '\n' &&
                   n < cap - 1) {
                value[n] = v[n];
                n++;
            }
            value[n] = '\0';
            return value;
        }
        p++;
    }
    return 0;
}

/* writes the complete buffer; a single send() may write partially */
static void
mock_send_all(sock_t conn, const char *buf, size_t n)
{
    size_t off = 0;

    while (off < n) {
        long w = SOCK_WRITE(conn, buf + off, n - off);
        if (w <= 0)
            return;
        off += (size_t) w;
    }
}

/* serves one connection in auto mode */
static void
mock_auto_respond(struct mock_srv *m, sock_t conn, const char *req)
{
    const char *range, *ifrange;
    char range_buf[96], ifrange_buf[300];
    uint64_t a = 0, b = 0;
    long status = 206;
    size_t body_len;

    range = req_header(req, "Range", range_buf, sizeof range_buf);
    ifrange = req_header(req, "If-Range", ifrange_buf, sizeof ifrange_buf);

    srv_lock(m);
    if (m->range_count < sizeof m->ranges / sizeof m->ranges[0]) {
        snprintf(m->ranges[m->range_count], sizeof m->ranges[0], "%s",
                 range != 0 ? range : "(none)");
        m->range_count++;
    }
    if (m->delay_ms > 0) {
        long d = m->delay_ms;
        srv_unlock(m);
        srv_sleep_ms(d);
        srv_lock(m);
    }
    if (m->raw_next < m->raw_count) {
        char *raw = m->raw[m->raw_next].raw;
        size_t n = m->raw[m->raw_next].len;

        m->raw_next++;
        srv_unlock(m);
        mock_send_all(conn, raw, n);
        return;
    }

    if (m->no_range_support || range == 0) {
        status = 200;
        a = 0;
        b = m->size > 0 ? m->size - 1 : 0;
    } else {
        {
            unsigned long long pa = 0, pb = 0;
            if (sscanf(range, "bytes=%llu-%llu", &pa, &pb) != 2) {
                static const char r416[] =
                    "HTTP/1.1 416 Range Not Satisfiable\r\n"
                    "Content-Length: 0\r\nConnection: close\r\n\r\n";

                srv_unlock(m);
                mock_send_all(conn, r416, sizeof r416 - 1);
                return;
            }
            a = pa;
            b = pb;
        }
        if (a >= m->size) {
            static const char r416[] =
                "HTTP/1.1 416 Range Not Satisfiable\r\n"
                "Content-Length: 0\r\nConnection: close\r\n\r\n";

            srv_unlock(m);
            mock_send_all(conn, r416, sizeof r416 - 1);
            return;
        }
        if (b >= m->size)
            b = m->size - 1;
        if (ifrange != 0 && strcmp(ifrange, m->etag) != 0) {
            /* validator mismatch: per RFC the whole object is returned */
            status = 200;
            a = 0;
            b = m->size - 1;
        }
    }
    body_len = (size_t) (b - a + 1);
    if (m->short_body && body_len > 8)
        body_len /= 2;
    srv_unlock(m);

    /* build and send the response (state captured under lock) */
    {
        char cr[128];
        char head[2048];
        if (status == 206) {
            long total = (long) m->size + m->total_delta;
            snprintf(cr, sizeof cr,
                     "Content-Range: bytes %llu-%llu/%ld\r\n",
                     (unsigned long long) a, (unsigned long long) b,
                     total);
            snprintf(head, sizeof head,
                     "HTTP/1.1 206 Partial Content\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "%s%s"
                     "ETag: %s\r\n"
                     "Accept-Ranges: bytes\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     cr,
                     m->send_encoding ? "Content-Encoding: gzip\r\n" : "",
                     m->etag, body_len);
        } else {
            snprintf(head, sizeof head,
                     "HTTP/1.1 %ld %s\r\n"
                     "Content-Type: application/octet-stream\r\n"
                     "Content-Length: %zu\r\n"
                     "Connection: close\r\n\r\n",
                     status, status == 200 ? "OK" : "Error", body_len);
        }
        mock_send_all(conn, head, strlen(head));
        /* body: from the container, honoring the actual byte range
           [a, b] but truncated for the short-body quirk */
        {
            size_t remaining = body_len;
            uint64_t pos = a;
            while (remaining > 0) {
                unsigned char buf[8192];
                size_t want = remaining > sizeof buf ? sizeof buf
                                                     : remaining;
                size_t i;
                for (i = 0; i < want; i++)
                    buf[i] = pos + i < m->size
                                 ? m->data[pos + i] : (unsigned char) 0xAB;
                if (SOCK_WRITE(conn, buf, want) != (long) want)
                    break;
                pos += want;
                remaining -= want;
            }
        }
    }
}

static void
mock_handle_conn(struct mock_srv *m, sock_t conn)
{
    char req[16384];
    size_t used = 0;
    int has_body_end = 0;

    /* read the request head */
    while (used < sizeof req - 1 && !has_body_end) {
        size_t n = SOCK_READ(conn, req + used, sizeof req - 1 - used);
        if (n == 0 || n == (size_t) -1)
            break;
        used += n;
        req[used] = '\0';
        if (strstr(req, "\r\n\r\n") != 0)
            has_body_end = 1;
    }
    if (used == 0)
        return;
    req[used] = '\0';
    mock_auto_respond(m, conn, req);
}

#if !defined(_WIN32)
static void *
mock_thread(void *arg)
{
    struct mock_srv *m = (struct mock_srv *) arg;

    while (!m->stop) {
        sock_t conn = accept(m->listen_fd, 0, 0);
        if (conn == SOCK_INVALID)
            continue;
        if (m->stop) {
            SOCK_CLOSE(conn);
            break;
        }
        mock_handle_conn(m, conn);
        SOCK_CLOSE(conn);
    }
    return 0;
}
#else
static DWORD WINAPI
mock_thread(LPVOID arg)
{
    struct mock_srv *m = (struct mock_srv *) arg;

    while (!m->stop) {
        sock_t conn = accept(m->listen_fd, 0, 0);
        if (conn == SOCK_INVALID)
            continue;
        if (m->stop) {
            SOCK_CLOSE(conn);
            break;
        }
        mock_handle_conn(m, conn);
        SOCK_CLOSE(conn);
    }
    return 0;
}
#endif

static struct mock_srv *
mock_start(const unsigned char *data, size_t len, const char *etag)
{
    struct mock_srv *m = (struct mock_srv *) calloc(1, sizeof *m);
    struct sockaddr_in addr;
    sock_t opt = 1;

    if (m == 0)
        return 0;
#ifdef _WIN32
    {
        WSADATA wsa;
        if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
            free(m);
            return 0;
        }
    }
#endif
    m->listen_fd = SOCK_INVALID;
    m->data = (unsigned char *) malloc(len > 0 ? len : 1);
    if (m->data == 0) {
        free(m);
        return 0;
    }
    memcpy(m->data, data, len);
    m->size = len;
    snprintf(m->etag, sizeof m->etag, "%s", etag != 0 ? etag : "\"mock\"");
    m->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (m->listen_fd == SOCK_INVALID) {
        free(m->data);
        free(m);
        return 0;
    }
    setsockopt(m->listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = 0;
    if (bind(m->listen_fd, (struct sockaddr *) &addr, sizeof addr) != 0 ||
        listen(m->listen_fd, 4) != 0) {
        SOCK_CLOSE(m->listen_fd);
        free(m->data);
        free(m);
        return 0;
    }
    {
        struct sockaddr_in bound;
        socklen_t blen = sizeof bound;
        getsockname(m->listen_fd, (struct sockaddr *) &bound, &blen);
        m->port = ntohs(bound.sin_port);
    }
#ifdef _WIN32
    InitializeCriticalSection(&m->lock);
    m->handle = CreateThread(0, 0, mock_thread, m, 0, 0);
    if (m->handle == 0) {
        SOCK_CLOSE(m->listen_fd);
        DeleteCriticalSection(&m->lock);
        free(m->data);
        free(m);
        return 0;
    }
    m->thread_valid = 1;
#else
    if (pthread_mutex_init(&m->lock, 0) != 0) {
        SOCK_CLOSE(m->listen_fd);
        free(m->data);
        free(m);
        return 0;
    }
    if (pthread_create(&m->handle, 0, mock_thread, m) != 0) {
        pthread_mutex_destroy(&m->lock);
        SOCK_CLOSE(m->listen_fd);
        free(m->data);
        free(m);
        return 0;
    }
    m->thread_valid = 1;
#endif
    return m;
}

/* enqueues a scripted response: the NUL-terminated `head` block (which
   may include pre-body framing such as a chunk-size line), then
   `body_len` bytes of `fill`, then the `tail` bytes (e.g. the chunked
   terminator) */
static void
mock_enqueue_raw_n(struct mock_srv *m, const char *head, size_t body_len,
                   int fill, const char *tail)
{
    size_t hlen = strlen(head);
    size_t tlen = tail != 0 ? strlen(tail) : 0;
    char *buf = (char *) malloc(hlen + body_len + tlen);

    if (buf == 0)
        return;
    memcpy(buf, head, hlen);
    memset(buf + hlen, fill, body_len);
    if (tlen != 0)
        memcpy(buf + hlen + body_len, tail, tlen);
    srv_lock(m);
    if (m->raw_count == m->raw_cap) {
        size_t ncap = m->raw_cap == 0 ? 8 : m->raw_cap * 2;
        mock_raw_response *nr = (mock_raw_response *)
            realloc(m->raw, ncap * sizeof *m->raw);
        if (nr != 0) {
            m->raw = nr;
            m->raw_cap = ncap;
        }
    }
    if (m->raw_count < m->raw_cap) {
        m->raw[m->raw_count].raw = buf;
        m->raw[m->raw_count].len = hlen + body_len + tlen;
        m->raw_count++;
    } else {
        free(buf);
    }
    srv_unlock(m);
}

static void
mock_enqueue_raw(struct mock_srv *m, const char *raw_response)
{
    mock_enqueue_raw_n(m, raw_response, 0, 0, 0);
}

static int
mock_range_requested(struct mock_srv *m, const char *range, size_t *count)
{
    size_t i;
    int found = 0;

    srv_lock(m);
    for (i = 0; i < m->range_count; i++)
        if (strcmp(m->ranges[i], range) == 0) {
            found = 1;
            break;
        }
    if (count != 0)
        *count = m->range_count;
    srv_unlock(m);
    return found;
}

static void
mock_stop(struct mock_srv *m)
{
    sock_t wake;
    struct sockaddr_in addr;
    int i;

    if (m == 0)
        return;
    srv_lock(m);
    m->stop = 1;
    i = m->port;
    srv_unlock(m);
    wake = socket(AF_INET, SOCK_STREAM, 0);
    if (wake != SOCK_INVALID) {
        memset(&addr, 0, sizeof addr);
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        addr.sin_port = htons((unsigned short) i);
        connect(wake, (struct sockaddr *) &addr, sizeof addr);
        SOCK_CLOSE(wake);
    }
    if (m->thread_valid) {
#ifdef _WIN32
        WaitForSingleObject(m->handle, 5000);
#else
        pthread_join(m->handle, 0);
#endif
    }
    SOCK_CLOSE(m->listen_fd);
#ifdef _WIN32
    DeleteCriticalSection(&m->lock);
    WSACleanup();
#else
    pthread_mutex_destroy(&m->lock);
#endif
    for (i = 0; (size_t) i < m->raw_count; i++)
        free(m->raw[i].raw);
    free(m->raw);
    free(m->data);
    free(m);
}

/* quirk setters: all shared mock state is written under the lock so
   the server thread never observes a torn value */
static void
mock_set_etag(struct mock_srv *m, const char *etag)
{
    srv_lock(m);
    snprintf(m->etag, sizeof m->etag, "%s", etag);
    srv_unlock(m);
}

static void
mock_set_no_range_support(struct mock_srv *m, int v)
{
    srv_lock(m);
    m->no_range_support = v;
    srv_unlock(m);
}

static void
mock_set_total_delta(struct mock_srv *m, long v)
{
    srv_lock(m);
    m->total_delta = v;
    srv_unlock(m);
}

static void
mock_set_short_body(struct mock_srv *m, int v)
{
    srv_lock(m);
    m->short_body = v;
    srv_unlock(m);
}

static void
mock_set_send_encoding(struct mock_srv *m, int v)
{
    srv_lock(m);
    m->send_encoding = v;
    srv_unlock(m);
}

static void
mock_set_delay_ms(struct mock_srv *m, long v)
{
    srv_lock(m);
    m->delay_ms = v;
    srv_unlock(m);
}

/* ------------------------------------------------------------------ */
/* tests                                                               */
/* ------------------------------------------------------------------ */

static const char *g_fixture;

/* walks the block stream of a packed container; returns the payload
   offset of the first block with the given type */
static int
find_block(const unsigned char *buf, size_t len, const char *type,
           uint64_t *block_off, uint64_t *payload_off, uint64_t *payload_len)
{
    uint64_t pos = 16;

    while (pos + 14 <= (uint64_t) len) {
        uint64_t plen = 0;
        size_t i;

        for (i = 0; i < 8; i++)
            plen = (plen << 8) | buf[pos + 4 + i];
        if (memcmp(buf + pos, type, 4) == 0) {
            *block_off = pos;
            *payload_off = pos + 14;
            *payload_len = plen;
            return 0;
        }
        pos += 14 + plen;
    }
    return -1;
}

static musicpack_status
pack_container(const char *tmp, const char *name, size_t pad,
               char *out, size_t cap)
{
    char root[4200];
    musicpack_status s;

    join_path(root, sizeof root, tmp, name);
    if (build_min_package(root, g_fixture, pad, "audio/01.mpc", 0) != 0)
        return MUSICPACK_ERR_IO;
    join_path(out, cap, tmp, name);
    strcat(out, ".mpak");
    s = musicpack_mpak_pack_dir(root, out, 0);
    return s;
}

static void
test_http_discovery_and_reads(const char *tmp)
{
    char mpak[4200];
    unsigned char *packed;
    size_t packed_len;
    struct mock_srv *srv;
    musicpack_range_source src;
    musicpack_package *pkg, *local;
    musicpack_status s;
    unsigned char *got = 0, *expect = 0;
    size_t got_len = 0, expect_len = 0;
    char url[256];

    join_path(mpak, sizeof mpak, tmp, "disc.mpak");
    s = pack_container(tmp, "disc", 300u * 1024u, mpak, sizeof mpak);
    CHECK(s == MUSICPACK_OK, "pack multi-block container");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read packed");
    if (packed == 0)
        return;
    CHECK(packed_len > 300u * 1024u,
          "container exceeds the discovery prefix");

    srv = mock_start(packed, packed_len, "\"test-etag-v1\"");
    CHECK(srv != 0, "mock server started");
    if (srv == 0) {
        free(packed);
        return;
    }
    /* the container exceeds the 256 KiB discovery prefix, so member
       fetches beyond the prefix are exercised */

    snprintf(url, sizeof url, "http://127.0.0.1:%d/file.mpak", srv->port);
    s = musicpack_http_range_source(url, 0, &src);
    if (s != MUSICPACK_OK) {
        CHECK(0, "http source created");
        /* creation failed: `out` was never written, nothing to destroy */
        free(packed);
        mock_stop(srv);
        return;
    }

    /* the discovery request asked for the design's 256 KiB prefix */
    CHECK(mock_range_requested(srv, "bytes=0-262143", 0),
          "discovery range requested");

    pkg = musicpack_package_open_range(&src, &s);
    CHECK(pkg != 0, "range open over HTTP");
    if (pkg != 0) {
        CHECK(musicpack_package_verify(pkg, 0, 0, 0) == MUSICPACK_OK,
              "HTTP-backed verify");

        /* byte-exact member reads, compared against the stdio source */
        CHECK(musicpack_package_read_member(pkg, "audio/01.mpc",
                                            2u << 20, &got, &got_len)
                  == MUSICPACK_OK, "http member read");
        CHECK(musicpack_range_source_stdio(mpak, &src) == MUSICPACK_OK,
              "stdio source");
        local = musicpack_package_open_range(&src, &s);
        CHECK(local != 0, "stdio-backed open");
        if (local != 0) {
            CHECK(musicpack_package_read_member(local, "audio/01.mpc",
                                                2u << 20, &expect,
                                                &expect_len)
                      == MUSICPACK_OK, "stdio member read");
            CHECK(got_len == expect_len &&
                  memcmp(got, expect, got_len) == 0,
                  "HTTP reads == stdio reads (byte-exact)");
            musicpack_package_close(local);
        }
        free(got);
        free(expect);
        got = expect = 0;
        musicpack_package_close(pkg);   /* adopts + destroys the source */
    }
    mock_stop(srv);
    free(packed);
}

static void
test_http_mpc_decode(const char *tmp)
{
    char mpak[4200], url[256];
    unsigned char *packed;
    size_t packed_len;
    struct mock_srv *srv;
    musicpack_range_source src;
    musicpack_package *pkg;
    musicpack_status s;
    mpc_reader reader;
    musepack_decoder *dec;
    float pcm[1152 * 2];
    uint64_t frames = 0, total = 0;
    unsigned char *orig, *got = 0;
    size_t orig_len = 0, got_len = 0;

    join_path(mpak, sizeof mpak, tmp, "dec.mpak");
    CHECK(pack_container(tmp, "dec", 0, mpak, sizeof mpak) == MUSICPACK_OK,
          "pack");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read");
    orig = read_file_all(g_fixture, &orig_len);
    CHECK(orig != 0, "fixture");
    if (packed == 0 || orig == 0) {
        free(packed);
        free(orig);
        return;
    }
    srv = mock_start(packed, packed_len, "\"etag-dec\"");
    CHECK(srv != 0, "server");
    if (srv == 0) {
        free(packed);
        free(orig);
        return;
    }
    snprintf(url, sizeof url, "http://127.0.0.1:%d/f.mpak", srv->port);
    if (musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK) {
        CHECK(0, "source");
        mock_stop(srv);
        free(packed);
        free(orig);
        return;
    }
    pkg = musicpack_package_open_range(&src, &s);
    CHECK(pkg != 0, "open");
    if (pkg != 0) {
        memset(&reader, 0, sizeof reader);
        CHECK(musicpack_package_track_open_reader(pkg, 0, 0, &reader)
                  == MUSICPACK_OK, "reader over HTTP package");
        /* complete byte stream identical to the original MPC */
        CHECK(musicpack_package_read_member(pkg, "audio/01.mpc",
                                            1u << 20, &got, &got_len)
                  == MUSICPACK_OK, "member read");
        CHECK(got_len == orig_len && memcmp(got, orig, orig_len) == 0,
              "bytes over HTTP == original MPC");
        free(got);
        got = 0;
        /* decoder consumes the HTTP-backed stream */
        dec = musepack_decoder_open(&reader, 0);
        CHECK(dec != 0, "decoder over HTTP-backed reader");
        if (dec != 0) {
            while (musepack_decoder_read(dec, pcm, 1152, &frames)
                       == MUSEPACK_OK)
                total += frames;
            CHECK(total > 0, "frames decoded over HTTP");
            /* in-track seek through the HTTP-backed reader */
            CHECK(musepack_decoder_seek_sample(dec, 22050) == MUSEPACK_OK,
                  "SV8 seek over HTTP");
            total = 0;
            while (musepack_decoder_read(dec, pcm, 1152, &frames)
                       == MUSEPACK_OK)
                total += frames;
            CHECK(total > 0, "decoded after seek");
            musepack_decoder_close(dec);
        }
        /* tell/get_size consistency through the HTTP-backed reader */
        CHECK(reader.seek(&reader, (mpc_seek_t) 1000) == MPC_TRUE,
              "raw seek");
        CHECK(reader.tell(&reader) == (mpc_seek_t) 1000, "raw tell");
        musicpack_package_track_close_reader(&reader);
        musicpack_package_close(pkg);   /* adopts + destroys the source */
    }
    mock_stop(srv);
    free(packed);
    free(orig);
}

static void
test_http_ownership(const char *tmp)
{
    char mpak[4200], url[256], url2[256];
    unsigned char *packed;
    size_t packed_len;
    struct mock_srv *srv;
    musicpack_range_source src;
    musicpack_package *pkg;
    musicpack_status s;

    join_path(mpak, sizeof mpak, tmp, "own.mpak");
    CHECK(pack_container(tmp, "own", 0, mpak, sizeof mpak) == MUSICPACK_OK,
          "pack");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read");
    if (packed == 0)
        return;
    srv = mock_start(packed, packed_len, "\"etag-own\"");
    CHECK(srv != 0, "server");
    if (srv == 0) {
        free(packed);
        return;
    }
    snprintf(url, sizeof url, "http://127.0.0.1:%d/o.mpak", srv->port);

    /* failure path A: server refuses Range -> creation fails -> `out`
       is never written and the adapter cleaned its own internals */
    mock_set_no_range_support(srv, 1);
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "no-range server fails creation");
    mock_set_no_range_support(srv, 0);

    /* failure path B: creation succeeds (discovery OK) but
       musicpack_package_open_range() fails on a corrupt MANF -> the
       caller owns the created source and destroys it (contract) */
    {
        unsigned char *broken;
        struct mock_srv *srv2 = 0;
        musicpack_range_source src2;
        musicpack_package *pkg2 = 0;
        uint64_t bo, po, pl;

        broken = (unsigned char *) malloc(packed_len);
        CHECK(broken != 0, "alloc broken");
        memcpy(broken, packed, packed_len);
        if (find_block(broken, packed_len, "MANF", &bo, &po, &pl) != 0) {
            free(broken);
            mock_stop(srv2);
            free(packed);
            CHECK(0, "find MANF");
            return;
        }
        broken[po] = '[';             /* corrupt the manifest JSON */
        srv2 = mock_start(broken, packed_len, "\"etag-own2\"");
        CHECK(srv2 != 0, "server 2");
        if (srv2 != 0) {
            snprintf(url2, sizeof url2, "http://127.0.0.1:%d/b.mpak",
                     srv2->port);
            if (musicpack_http_range_source(url2, 0, &src2)
                    != MUSICPACK_OK) {
                CHECK(0, "creation succeeds (corrupt MANF)");
                mock_stop(srv2);
                mock_stop(srv);
                free(broken);
                free(packed);
                return;
            }
            pkg2 = musicpack_package_open_range(&src2, &s);
            CHECK(pkg2 == 0 && s == MUSICPACK_ERR_JSON,
                  "open_range fails on corrupt MANF");
            /* the caller owns the created source on failure */
            src2.destroy(src2.ctx);
            mock_stop(srv2);
        }
        free(broken);
    }

    /* success path: package adopts and destroys the source at close */
    if (musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK) {
        CHECK(0, "creation succeeds");
        mock_stop(srv);
        free(packed);
        return;
    }
    pkg = musicpack_package_open_range(&src, &s);
    CHECK(pkg != 0, "open succeeds");
    if (pkg != 0)
        musicpack_package_close(pkg); /* destroys the adopted source */
    mock_stop(srv);
    free(packed);
}

static void
test_http_hostile_responses(const char *tmp)
{
    char mpak[4200], url[256];
    unsigned char *packed;
    size_t packed_len;
    struct mock_srv *srv;
    musicpack_range_source src;
    musicpack_status s;

    join_path(mpak, sizeof mpak, tmp, "hos.mpak");
    CHECK(pack_container(tmp, "hos", 0, mpak, sizeof mpak) == MUSICPACK_OK,
          "pack");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read");
    if (packed == 0)
        return;
    srv = mock_start(packed, packed_len, "\"etag-h\"");
    CHECK(srv != 0, "server");
    if (srv == 0) {
        free(packed);
        return;
    }
    snprintf(url, sizeof url, "http://127.0.0.1:%d/h.mpak", srv->port);

    /* malformed Content-Range: header missing entirely, body complete —
       the rejection is the Content-Range validation, not a transport
       error (the body must be complete or curl fails first) */
    mock_enqueue_raw_n(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Length: 262144\r\n"
        "Connection: close\r\n\r\n",
        262144, 'N', 0);
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "missing Content-Range rejected (complete body)");

    /* wrong Content-Range offset: internally consistent except the
       start, so the offset check is what rejects it */
    mock_enqueue_raw_n(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 5-262143/262144\r\n"
        "Content-Length: 262144\r\n"
        "Connection: close\r\n\r\n",
        262144, 'O', 0);
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "wrong offset rejected (complete body)");

    /* wrong Content-Range total: inconsistent with the served range
       (end 262143 >= total 143750) */
    mock_enqueue_raw_n(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-262143/143750\r\n"
        "Content-Length: 262144\r\n"
        "Connection: close\r\n\r\n",
        262144, 'T', 0);
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "wrong total rejected (complete body)");

    /* truncated discovery body (Content-Length larger than the body) */
    mock_enqueue_raw(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-262143/143750\r\n"
        "Content-Length: 262144\r\n"
        "Connection: close\r\n\r\nXXXX");
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "short body rejected");

    /* 200 to a ranged request: never interpreted as the range */
    mock_enqueue_raw(srv,
        "HTTP/1.1 200 OK\r\n"
        "Content-Length: 143750\r\n"
        "Connection: close\r\n\r\n");
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "no-Range server rejected");

    /* 206 with garbage Content-Range unit */
    mock_enqueue_raw_n(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: items 0-262143/262144\r\n"
        "Content-Length: 262144\r\n"
        "Connection: close\r\n\r\n",
        262144, 'U', 0);
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "wrong unit rejected (complete body)");

    /* multipart/byteranges: forbidden on range responses (design §4) —
       the response is otherwise completely valid, so the rejection is
       the adapter's Content-Type check, not transport or MPAK-layer */
    mock_enqueue_raw_n(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-262143/262144\r\n"
        "Content-Type: multipart/byteranges; boundary=x\r\n"
        "Content-Length: 262144\r\n"
        "Connection: close\r\n\r\n",
        262144, 'M', 0);
    CHECK(musicpack_http_range_source(url, 0, &src) == MUSICPACK_ERR_IO,
          "multipart/byteranges rejected (adapter layer)");

    /* Transfer-Encoding: chunked on a 206: design §4 rule 6 permits
       chunked only on 200 full-body responses; the body is properly
       chunked-framed (one 262144-byte chunk) so curl itself succeeds
       and the rejection is the adapter's header validation */
    mock_enqueue_raw_n(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-262143/262144\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: close\r\n\r\n"
        "40000\r\n",
        262144, 'C', "\r\n0\r\n\r\n");
    CHECK(musicpack_http_range_source(url, 0, &src) == MUSICPACK_ERR_IO,
          "chunked 206 rejected (adapter layer)");

    /* 5xx transport failure */
    mock_enqueue_raw(srv,
        "HTTP/1.1 500 Internal Server Error\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n");
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "5xx rejected");

    /* 404 maps to MISSING */
    mock_enqueue_raw(srv,
        "HTTP/1.1 404 Not Found\r\n"
        "Content-Length: 0\r\n"
        "Connection: close\r\n\r\n");
    CHECK(musicpack_http_range_source(url, 0, &src) == MUSICPACK_ERR_MISSING,
          "404 maps to MISSING");

    /* content-encoding rejection (complete body: the header check is
       what rejects, not a short transfer) */
    mock_enqueue_raw_n(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-262143/262144\r\n"
        "Content-Encoding: gzip\r\n"
        "Content-Length: 262144\r\n"
        "Connection: close\r\n\r\n",
        262144, 'E', 0);
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "content-encoding rejected (complete body)");

    /* weak ETag must not be used as a strong validator: the session is
       created (no If-Range is ever replayed) but nothing stronger is
       promised; the open itself still works */
    mock_enqueue_raw_n(srv,
        "HTTP/1.1 206 Partial Content\r\n"
        "Content-Range: bytes 0-262143/262144\r\n"
        "ETag: W/\"weak\"\r\n"
        "Content-Length: 262144\r\n"
        "Connection: close\r\n\r\n",
        262144, 'W', 0);
    s = musicpack_http_range_source(url, 0, &src);
    CHECK(s == MUSICPACK_OK, "weak ETag: session usable without replay");
    if (s == MUSICPACK_OK) {
        src.destroy(src.ctx);   /* caller-owned: open_range never called */
    }

    /* oversized discovery body (more bytes than requested) */
    {
        size_t i;
        char *big = (char *) malloc(300000 + 512);
        strcpy(big,
               "HTTP/1.1 206 Partial Content\r\n"
               "Content-Range: bytes 0-262143/143750\r\n"
               "Content-Length: 262144\r\n"
               "Connection: close\r\n\r\n");
        for (i = 0; i < 300000; i++)
            strcat(big + strlen(big), "x");
        mock_enqueue_raw(srv, big);
        free(big);
        CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
              "oversized body rejected");
    }

    mock_stop(srv);
    free(packed);
}

static void
test_http_quirk_responses(const char *tmp)
{
    char mpak[4200], url[256];
    unsigned char *packed;
    size_t packed_len;
    struct mock_srv *srv;
    musicpack_range_source src;
    musicpack_package *pkg;
    musicpack_status s;

    join_path(mpak, sizeof mpak, tmp, "qrk.mpak");
    /* pad past the 256 KiB discovery prefix so the changed-object test
       exercises a real post-discovery fetch (If-Range mismatch) */
    CHECK(pack_container(tmp, "qrk", 300u * 1024u, mpak, sizeof mpak)
              == MUSICPACK_OK, "pack");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read");
    if (packed == 0)
        return;
    srv = mock_start(packed, packed_len, "\"etag-q\"");
    CHECK(srv != 0, "server");
    if (srv == 0) {
        free(packed);
        return;
    }
    snprintf(url, sizeof url, "http://127.0.0.1:%d/q.mpak", srv->port);

    /* wrong total size in Content-Range (quirk): at discovery the total
       DEFINES the session, so creation succeeds — the lie is caught as
       soon as the scanner reads past the real data (a conformant
       adapter refuses ranges beyond the object it actually has) */
    mock_set_total_delta(srv, 999);
    if (musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK) {
        CHECK(0, "wrong total: creation succeeds (total defines session)");
        mock_stop(srv);
        free(packed);
        return;
    }
    pkg = musicpack_package_open_range(&src, &s);
    CHECK(pkg == 0 && s == MUSICPACK_ERR_IO,
          "size lie fails open when the scanner reads past real data");
    src.destroy(src.ctx);   /* failed open: caller owns the source */
    mock_set_total_delta(srv, 0);

    /* declared range with truncated body (quirk) */
    mock_set_short_body(srv, 1);
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "short body (quirk) rejected");
    mock_set_short_body(srv, 0);

    /* Content-Encoding (quirk) */
    mock_set_send_encoding(srv, 1);
    CHECK(musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK,
          "content-encoding (quirk) rejected");
    mock_set_send_encoding(srv, 0);

    /* changed remote object: the server's ETag changes between the
       discovery and a later fetch; If-Range mismatch yields 200 which
       the adapter must reject instead of mixing versions */
    if (musicpack_http_range_source(url, 0, &src) != MUSICPACK_OK) {
        CHECK(0, "open with etag v1");
        mock_stop(srv);
        free(packed);
        return;
    }
    mock_set_etag(srv, "\"etag-changed\"");
    pkg = musicpack_package_open_range(&src, &s);
    /* the package may open (bytes already validated at discovery) but
       any member fetch must fail: changed object */
    if (pkg != 0) {
        unsigned char *got = 0;
        size_t got_len = 0;
        CHECK(musicpack_package_read_member(pkg, "audio/01.mpc", 1u << 20,
                                            &got, &got_len)
                  != MUSICPACK_OK, "changed object fails member read");
        free(got);
        musicpack_package_close(pkg);   /* adopted: destroyed at close */
    }
    mock_stop(srv);
    free(packed);
}

static void
test_http_timeout(const char *tmp)
{
    char mpak[4200], url[256];
    unsigned char *packed;
    size_t packed_len;
    struct mock_srv *srv;
    musicpack_range_source src;
    musicpack_http_source_opts opts;
    musicpack_status s;
    time_t start, end;

    join_path(mpak, sizeof mpak, tmp, "to.mpak");
    CHECK(pack_container(tmp, "to", 0, mpak, sizeof mpak) == MUSICPACK_OK,
          "pack");
    packed = read_file_all(mpak, &packed_len);
    CHECK(packed != 0, "read");
    if (packed == 0)
        return;
    srv = mock_start(packed, packed_len, "\"etag-to\"");
    CHECK(srv != 0, "server");
    if (srv == 0) {
        free(packed);
        return;
    }
    mock_set_delay_ms(srv, 2000);
    snprintf(url, sizeof url, "http://127.0.0.1:%d/t.mpak", srv->port);
    memset(&opts, 0, sizeof opts);
    opts.timeout_ms = 300;
    start = time(0);
    s = musicpack_http_range_source(url, &opts, &src);
    end = time(0);
    CHECK(s == MUSICPACK_ERR_IO, "timeout maps to IO");
    CHECK(end - start <= 1, "timeout honored promptly");
    if (s == MUSICPACK_OK)
        src.destroy(src.ctx);
    mock_set_delay_ms(srv, 0);
    mock_stop(srv);
    free(packed);
}

int
main(int argc, char **argv)
{
    char tmp[4200];

    if (argc < 2) {
        fprintf(stderr, "usage: mpakhttp_tests <fixture.mpc>\n");
        return 2;
    }
    g_fixture = argv[1];

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN);   /* the mock must survive client timeouts */
#endif
    if (make_temp_dir(tmp, sizeof tmp) != 0) {
        fprintf(stderr, "cannot create temp dir\n");
        return 1;
    }

    test_http_discovery_and_reads(tmp);
    test_http_mpc_decode(tmp);
    test_http_ownership(tmp);
    test_http_hostile_responses(tmp);
    test_http_quirk_responses(tmp);
    test_http_timeout(tmp);

    remove_tree(tmp);

    if (failures) {
        fprintf(stderr, "%d mpakhttp test(s) failed\n", failures);
        return 1;
    }
    printf("all mpakhttp tests passed\n");
    return 0;
}
