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
/// \file http_source.c
/// libcurl-backed HTTP Range adapter (specs/mpak-http-range-design.md).
///
/// Session model: discovery fixes the total size (immutable) and a
/// strong ETag when offered; every fetch validates the response against
/// that session state; any violation fails the session (sticky). The
/// 256 KiB discovery prefix is retained and served from memory —
/// adapter read-ahead policy; the core container cache is separate.
/// One easy handle per source for connection reuse; no retries.

#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mpakhttp/mpakhttp.h>

#define MPAKHTTP_DISCOVERY_BYTES (256u * 1024u)
#define MPAKHTTP_DEFAULT_TIMEOUT_MS 10000L
#define MPAKHTTP_MAX_REDIRECTS 8

/* ------------------------------------------------------------------ */
/* response buffer + header capture                                    */
/* ------------------------------------------------------------------ */

typedef struct mpakhttp_resp {
    long status;
    char content_range[256];   /* raw header value, NUL-terminated */
    int have_content_range;
    char etag[256];            /* raw header value (quotes preserved) */
    int have_etag;
    int have_encoding;         /* Content-Encoding seen */
    int multipart;             /* Content-Type: multipart/... seen */
    int chunked;               /* Transfer-Encoding: chunked seen */
    unsigned char *body;       /* accumulated body */
    size_t len;
    size_t cap;                /* hard cap: oversized bodies abort */
    int oversized;             /* body exceeded cap */
} mpakhttp_resp;

static int
resp_init(mpakhttp_resp *r, size_t cap)
{
    memset(r, 0, sizeof *r);
    r->cap = cap;
    r->body = (unsigned char *) malloc(cap > 0 ? cap : 1);
    return r->body != 0;
}

static void
resp_free(mpakhttp_resp *r)
{
    free(r->body);
    r->body = 0;
    r->len = r->cap = 0;
}

static size_t
write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    mpakhttp_resp *r = (mpakhttp_resp *) userdata;
    size_t bytes = size * nmemb;

    if (r->oversized || bytes > r->cap - r->len) {
        r->oversized = 1;      /* abort the transfer via the return */
        return 0;              /* libcurl maps this to CURLE_WRITE_ERROR */
    }
    memcpy(r->body + r->len, ptr, bytes);
    r->len += bytes;
    return bytes;
}

static size_t
header_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    mpakhttp_resp *r = (mpakhttp_resp *) userdata;
    size_t bytes = size * nmemb;
    size_t trimmed = bytes;

    /* header lines are NUL-terminated by libcurl; strip trailing CRLF
       for parsing. The FULL length must be returned (a short return
       makes libcurl abort the transfer). */
    while (trimmed > 0 && (ptr[trimmed - 1] == '\n' ||
                           ptr[trimmed - 1] == '\r'))
        trimmed--;
    if (trimmed == 0)
        return bytes;          /* end-of-headers marker */

#define MPAKHTTP_HDR_MATCH(name)                                              \
    (bytes > (sizeof(name) - 1) + 1 &&                                        \
     strncasecmp(ptr, name, sizeof(name) - 1) == 0 &&                         \
     ptr[sizeof(name) - 1] == ':')

    if (MPAKHTTP_HDR_MATCH("Content-Range")) {
        const char *v = ptr + sizeof("Content-Range");   /* past ':' */
        size_t n = trimmed - (sizeof("Content-Range") - 1) - 1;
        while (n > 0 && (*v == ' ' || *v == '\t')) { v++; n--; }
        n = n < sizeof r->content_range - 1 ? n : sizeof r->content_range - 1;
        memcpy(r->content_range, v, n);
        r->content_range[n] = '\0';
        r->have_content_range = 1;
    } else if (MPAKHTTP_HDR_MATCH("ETag")) {
        const char *v = ptr + sizeof("ETag");            /* past ':' */
        size_t n = trimmed - (sizeof("ETag") - 1) - 1;
        while (n > 0 && (*v == ' ' || *v == '\t')) { v++; n--; }
        n = n < sizeof r->etag - 1 ? n : sizeof r->etag - 1;
        memcpy(r->etag, v, n);
        r->etag[n] = '\0';
        r->have_etag = 1;
    } else if (MPAKHTTP_HDR_MATCH("Content-Encoding")) {
        r->have_encoding = 1;
    } else if (MPAKHTTP_HDR_MATCH("Content-Type")) {
        const char *v = ptr + sizeof("Content-Type");    /* past ':' */
        size_t n = trimmed - (sizeof("Content-Type") - 1) - 1;
        while (n > 0 && (*v == ' ' || *v == '\t')) { v++; n--; }
        if (n >= sizeof("multipart/") - 1 &&
            strncasecmp(v, "multipart/", sizeof("multipart/") - 1) == 0)
            r->multipart = 1;
    } else if (MPAKHTTP_HDR_MATCH("Transfer-Encoding")) {
        const char *v = ptr + sizeof("Transfer-Encoding");   /* past ':' */
        size_t n = trimmed - (sizeof("Transfer-Encoding") - 1) - 1;
        while (n > 0 && (*v == ' ' || *v == '\t')) { v++; n--; }
        if (n >= sizeof("chunked") - 1 &&
            strncasecmp(v, "chunked", sizeof("chunked") - 1) == 0)
            r->chunked = 1;
    }
#undef MPAKHTTP_HDR_MATCH
    return bytes;
}

/* ------------------------------------------------------------------ */
/* Content-Range parsing (strict; overflow-checked)                    */
/* ------------------------------------------------------------------ */

static int
parse_u64(const char *s, const char **end, uint64_t *out)
{
    uint64_t v = 0;

    if (*s < '0' || *s > '9')
        return 0;
    while (*s >= '0' && *s <= '9') {
        unsigned d = (unsigned) (*s - '0');
        if (v > (UINT64_MAX - d) / 10)
            return 0;          /* overflow */
        v = v * 10 + d;
        s++;
    }
    *end = s;
    *out = v;
    return 1;
}

/* "bytes <start>-<end>/<total>" ; total may be '*' */
static int
parse_content_range(const char *s, uint64_t *start, uint64_t *end,
                    uint64_t *total, int *total_known)
{
    const char *p = s;

    if (strncasecmp(p, "bytes", 5) != 0)
        return 0;
    p += 5;
    while (*p == ' ' || *p == '\t')
        p++;
    if (!parse_u64(p, &p, start))
        return 0;
    if (*p != '-')
        return 0;
    p++;
    if (!parse_u64(p, &p, end))
        return 0;
    if (*p != '/')
        return 0;
    p++;
    if (*p == '*') {
        *total_known = 0;
        *total = 0;
        p++;
    } else {
        *total_known = 1;
        if (!parse_u64(p, &p, total))
            return 0;
    }
    if (*p != '\0' || *start > *end)
        return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* HTTP session                                                        */
/* ------------------------------------------------------------------ */

typedef struct mpakhttp_ctx {
    char *url;                 /* session URL (post-redirect) */
    char *etag;                /* strong validator, or NULL */
    uint64_t size;             /* total object size (immutable) */
    unsigned char *prefix;     /* discovery bytes */
    size_t prefix_len;
    CURL *curl;
    char errbuf[CURL_ERROR_SIZE];
    long timeout_ms;
    long last_status;
    int failed;                /* sticky: any transport/protocol failure */
    char last_error[160];      /* human-readable diagnostic */
} mpak_http_ctx;

static void
ctx_fail(mpak_http_ctx *c, const char *what)
{
    if (!c->failed) {
        c->failed = 1;
        snprintf(c->last_error, sizeof c->last_error, "%s", what);
    }
}

/* One ranged GET. Returns MUSICPACK_OK with resp filled, or an error;
   on protocol violations the session is marked failed. When
   `expect_206` is set, any non-206 status is a hard session error —
   a hostile or broken server must never be able to feed arbitrary
   non-object bytes (or an unwritten response buffer) into the
   container layer. */
static musicpack_status
http_fetch(mpak_http_ctx *c, const char *range, size_t body_cap,
           mpakhttp_resp *resp, const char *what, int expect_206)
{
    char range_hdr[96];
    char ifrange_hdr[300];
    struct curl_slist *headers = 0;
    CURLcode rc;

    /* `resp` must be zero-initialized (or hold a body from a previous
       http_fetch): any previous body is freed here, then an exact-cap
       buffer is allocated */
    resp_free(resp);
    if (!resp_init(resp, body_cap)) {
        ctx_fail(c, "out of memory for the response body");
        return MUSICPACK_ERR_NOMEM;
    }
    c->last_status = 0;

    snprintf(range_hdr, sizeof range_hdr, "Range: %s", range);
    headers = curl_slist_append(headers, range_hdr);
    headers = curl_slist_append(headers, "Accept-Encoding: identity");
    if (c->etag != 0) {
        snprintf(ifrange_hdr, sizeof ifrange_hdr, "If-Range: %s", c->etag);
        headers = curl_slist_append(headers, ifrange_hdr);
    }
    if (headers == 0) {
        ctx_fail(c, "out of memory building request headers");
        return MUSICPACK_ERR_NOMEM;
    }

    curl_easy_reset(c->curl);
    curl_easy_setopt(c->curl, CURLOPT_URL, c->url);
    curl_easy_setopt(c->curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(c->curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(c->curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(c->curl, CURLOPT_WRITEDATA, resp);
    curl_easy_setopt(c->curl, CURLOPT_HEADERFUNCTION, header_cb);
    curl_easy_setopt(c->curl, CURLOPT_HEADERDATA, resp);
    curl_easy_setopt(c->curl, CURLOPT_ERRORBUFFER, c->errbuf);
    curl_easy_setopt(c->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c->curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c->curl, CURLOPT_MAXREDIRS, (long) MPAKHTTP_MAX_REDIRECTS);
    curl_easy_setopt(c->curl, CURLOPT_REDIR_PROTOCOLS,
                     (long) CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(c->curl, CURLOPT_PROTOCOLS,
                     (long) CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(c->curl, CURLOPT_ACCEPT_ENCODING, "identity");
    curl_easy_setopt(c->curl, CURLOPT_USERAGENT, "musicpack-mpakhttp/1.0");
    curl_easy_setopt(c->curl, CURLOPT_TIMEOUT_MS, c->timeout_ms);
    curl_easy_setopt(c->curl, CURLOPT_CONNECTTIMEOUT_MS, c->timeout_ms);

    c->errbuf[0] = '\0';
    rc = curl_easy_perform(c->curl);
    curl_slist_free_all(headers);

    if (rc != CURLE_OK) {
        snprintf(c->last_error, sizeof c->last_error, "%s: %s%s", what,
                 c->errbuf[0] ? c->errbuf : curl_easy_strerror(rc),
                 resp->oversized ? " (oversized response)" : "");
        ctx_fail(c, c->last_error);
        resp_free(resp);
        return MUSICPACK_ERR_IO;
    }
    curl_easy_getinfo(c->curl, CURLINFO_RESPONSE_CODE, &c->last_status);
    resp->status = c->last_status;
    if (resp->oversized) {
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: response body exceeds the requested range", what);
        ctx_fail(c, c->last_error);
        resp_free(resp);
        return MUSICPACK_ERR_IO;
    }
    if (resp->have_encoding) {
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: content-encoded response would break byte offsets",
                 what);
        ctx_fail(c, c->last_error);
        resp_free(resp);
        return MUSICPACK_ERR_IO;
    }
    if (resp->multipart) {
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: multipart/byteranges responses are not supported",
                 what);
        ctx_fail(c, c->last_error);
        resp_free(resp);
        return MUSICPACK_ERR_IO;
    }
    if (resp->chunked && resp->status != 200) {
        /* design §4 rule 6: chunked is only acceptable on 200 full-body
           responses; on a ranged response the framing must be exact */
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: Transfer-Encoding: chunked is only permitted on 200 "
                 "responses", what);
        ctx_fail(c, c->last_error);
        resp_free(resp);
        return MUSICPACK_ERR_IO;
    }
    if (expect_206 && resp->status != 206) {
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: expected 206, got %ld", what, resp->status);
        ctx_fail(c, c->last_error);
        resp_free(resp);
        return MUSICPACK_ERR_IO;
    }
    return MUSICPACK_OK;
}

/* Validates a 206 against the session; returns OK when the response is
   byte-exact for the requested range. `what` names the request. */
static musicpack_status
validate_206(mpak_http_ctx *c, const mpakhttp_resp *r, uint64_t offset,
             size_t len, const char *what)
{
    uint64_t start, end, total;
    int total_known;
    uint64_t expect_end = offset + (len > 0 ? len - 1 : 0);

    if (r->status != 206) {
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: expected 206, got %ld", what, r->status);
        ctx_fail(c, c->last_error);
        return MUSICPACK_ERR_IO;
    }
    if (!r->have_content_range ||
        !parse_content_range(r->content_range, &start, &end, &total,
                             &total_known)) {
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: malformed Content-Range \"%s\"", what,
                 r->have_content_range ? r->content_range : "");
        ctx_fail(c, c->last_error);
        return MUSICPACK_ERR_IO;
    }
    if (start != offset || end != expect_end || r->len != len) {
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: Content-Range/body disagree with the request "
                 "(got bytes %llu-%llu, %zu bytes)", what,
                 (unsigned long long) start, (unsigned long long) end,
                 r->len);
        ctx_fail(c, c->last_error);
        return MUSICPACK_ERR_IO;
    }
    if (total_known && total != c->size) {
        snprintf(c->last_error, sizeof c->last_error,
                 "%s: remote object changed (total %llu, session %llu)",
                 what, (unsigned long long) total,
                 (unsigned long long) c->size);
        ctx_fail(c, c->last_error);
        return MUSICPACK_ERR_IO;
    }
    return MUSICPACK_OK;
}

/* ---- range_source callbacks ---------------------------------------- */

static musicpack_status
http_source_size(void *ctx, uint64_t *out)
{
    mpak_http_ctx *c = (mpak_http_ctx *) ctx;

    if (c->failed)
        return MUSICPACK_ERR_IO;
    *out = c->size;
    return MUSICPACK_OK;
}

static musicpack_status
http_source_read(void *ctx, uint64_t offset, unsigned char *buf, size_t len)
{
    mpak_http_ctx *c = (mpak_http_ctx *) ctx;
    mpakhttp_resp resp;
    musicpack_status s;
    char range[64];

    memset(&resp, 0, sizeof resp);
    if (len == 0)
        return MUSICPACK_OK;
    if (c->failed)
        return MUSICPACK_ERR_IO;
    if (offset > c->size || len > c->size - offset)
        return MUSICPACK_ERR_IO;   /* defensive: callers stay in bounds */

    /* discovery prefix: served from memory (adapter read-ahead) */
    if (offset < c->prefix_len &&
        len <= c->prefix_len - offset) {
        memcpy(buf, c->prefix + offset, len);
        return MUSICPACK_OK;
    }

    snprintf(range, sizeof range, "bytes=%llu-%llu",
             (unsigned long long) offset,
             (unsigned long long) (offset + len - 1));
    s = http_fetch(c, range, len, &resp, "member read", 1);
    if (s != MUSICPACK_OK)
        return s;
    if (resp.status == 200 && c->etag != 0) {
        /* If-Range validator mismatch: the remote object changed */
        ctx_fail(c, "member read: remote object changed (If-Range "
                    "validator mismatch)");
        s = MUSICPACK_ERR_IO;
    } else {
        s = validate_206(c, &resp, offset, len, "member read");
        if (s == MUSICPACK_OK)
            memcpy(buf, resp.body, len);
    }
    resp_free(&resp);
    return s;
}

static void
http_source_destroy(void *ctx)
{
    mpak_http_ctx *c = (mpak_http_ctx *) ctx;

    if (c == 0)
        return;
    if (c->curl != 0)
        curl_easy_cleanup(c->curl);
    free(c->prefix);
    free(c->url);
    free(c->etag);
    free(c);
}

/* ------------------------------------------------------------------ */
/* creation + discovery                                                */
/* ------------------------------------------------------------------ */

musicpack_status
musicpack_http_range_source(const char *url,
                            const musicpack_http_source_opts *opts,
                            musicpack_range_source *out)
{
    mpak_http_ctx *c;
    musicpack_range_source src;
    musicpack_status s;
    mpakhttp_resp resp;
    static int curl_global_done;

    if (url == 0 || out == 0)
        return MUSICPACK_ERR_INVALID;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0)
        return MUSICPACK_ERR_INVALID;

    if (!curl_global_done) {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK)
            return MUSICPACK_ERR_IO;
        curl_global_done = 1;
    }

    c = (mpak_http_ctx *) calloc(1, sizeof *c);
    if (c == 0)
        return MUSICPACK_ERR_NOMEM;
    c->timeout_ms = (opts != 0 && opts->timeout_ms > 0)
                        ? opts->timeout_ms
                        : MUSICPACK_HTTP_TIMEOUT_DEFAULT_MS;
    c->url = strdup(url);
    if (c->url == 0) {
        free(c);
        return MUSICPACK_ERR_NOMEM;
    }
    c->curl = curl_easy_init();
    if (c->curl == 0) {
        http_source_destroy(c);
        return MUSICPACK_ERR_NOMEM;
    }

    /* ---- discovery: bytes=0-262143, design §3 R0 ---- */
    {
        uint64_t start = 0, end = 0, total = 0;
        int total_known = 0;
        uint64_t expect_end = MPAKHTTP_DISCOVERY_BYTES - 1;
        const char *effective = 0;

        if (!resp_init(&resp, 0)) {
            http_source_destroy(c);
            return MUSICPACK_ERR_NOMEM;
        }
        s = http_fetch(c, "bytes=0-262143", MPAKHTTP_DISCOVERY_BYTES, &resp,
                       "discovery", 0);
        if (s != MUSICPACK_OK)
            goto fail;
        if (resp.status == 404) {
            ctx_fail(c, "discovery: remote object not found (404)");
            s = MUSICPACK_ERR_MISSING;
            goto fail;
        }
        if (resp.status == 200) {
            /* Range ignored: design Tier-B — fail fast; the embedder may
               download the object and open it from disk instead. */
            ctx_fail(c, "discovery: server ignored Range (200 for a "
                        "ranged request)");
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        if (resp.status != 206) {
            snprintf(c->last_error, sizeof c->last_error,
                     "discovery: expected 206, got %ld", resp.status);
            ctx_fail(c, c->last_error);
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        if (resp.have_encoding || resp.multipart) {
            /* already rejected inside http_fetch */
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        if (!resp.have_content_range ||
            !parse_content_range(resp.content_range, &start, &end, &total,
                                 &total_known) ||
            start != 0) {
            snprintf(c->last_error, sizeof c->last_error,
                     "discovery: malformed Content-Range \"%s\"",
                     resp.have_content_range ? resp.content_range : "");
            ctx_fail(c, c->last_error);
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        if (total_known) {
            if (total == 0) {
                ctx_fail(c, "discovery: remote object is empty");
                s = MUSICPACK_ERR_IO;
                goto fail;
            }
            c->size = total;
            expect_end = total - 1 < expect_end ? total - 1 : expect_end;
        } else if (resp.len < MPAKHTTP_DISCOVERY_BYTES) {
            /* total '*': a short body is the whole object */
            c->size = resp.len;
            expect_end = c->size - 1;
        } else {
            /* total '*' with a full-length body: ambiguous — probe */
            mpakhttp_resp probe;
            uint64_t pstart = 0, pend = 0, ptotal = 0;
            int pknown = 0;

            memset(&probe, 0, sizeof probe);
            resp_free(&resp);
            s = http_fetch(c, "bytes=0-0", 8, &probe, "size probe", 1);
            if (s != MUSICPACK_OK)
                goto fail;
            if (probe.status != 206 ||
                !probe.have_content_range ||
                !parse_content_range(probe.content_range, &pstart, &pend,
                                     &ptotal, &pknown) ||
                pstart != 0 || pend != 0 || !pknown || ptotal == 0) {
                ctx_fail(c, "discovery: object size could not be "
                            "determined");
                s = MUSICPACK_ERR_IO;
                resp_free(&probe);
                goto fail;
            }
            c->size = ptotal;
            /* re-fetch the discovery prefix now that the size is known */
            s = http_fetch(c, "bytes=0-262143", MPAKHTTP_DISCOVERY_BYTES,
                           &resp, "discovery", 0);
            if (s != MUSICPACK_OK)
                goto fail;
            expect_end = c->size - 1 < expect_end ? c->size - 1 : expect_end;
            resp_free(&probe);
        }
        if (end != expect_end || resp.len != (size_t) (end + 1)) {
            snprintf(c->last_error, sizeof c->last_error,
                     "discovery: Content-Range/body disagree with the "
                     "request (bytes 0-%llu, %zu bytes)",
                     (unsigned long long) end, resp.len);
            ctx_fail(c, c->last_error);
            s = MUSICPACK_ERR_IO;
            goto fail;
        }
        if (c->size == 0 || c->size > UINT64_MAX - end) {
            ctx_fail(c, "discovery: nonsensical object size");
            s = MUSICPACK_ERR_IO;
            goto fail;
        }

        /* session URL: post-redirect location for all later requests */
        if (curl_easy_getinfo(c->curl, CURLINFO_EFFECTIVE_URL,
                              &effective) == CURLE_OK &&
            effective != 0) {
            free(c->url);
            c->url = strdup(effective);
            if (c->url == 0) {
                s = MUSICPACK_ERR_NOMEM;
                goto fail;
            }
        }

        /* strong validator only (weak ETags carry no byte guarantees) */
        if (resp.have_etag && strncmp(resp.etag, "W/", 2) != 0) {
            c->etag = strdup(resp.etag);
            if (c->etag == 0) {
                s = MUSICPACK_ERR_NOMEM;
                goto fail;
            }
        }

        /* retain the discovery bytes (adapter read-ahead policy) */
        c->prefix = resp.body;      /* stolen */
        c->prefix_len = resp.len;
        resp.body = 0;
        resp.len = 0;
    }

    src.ctx = c;
    src.size = http_source_size;
    src.read = http_source_read;
    src.destroy = http_source_destroy;
    *out = src;
    return MUSICPACK_OK;

fail:
    resp_free(&resp);
    http_source_destroy(c);
    return s;
}
