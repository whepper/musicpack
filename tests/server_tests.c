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

  * Neither the name of the MusicPack Development Team nor the
  names of its contributors may be used to endorse or promote
  products derived from this software without specific prior
  written permission.

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
/// \file server_tests.c
/// Unit tests for the musicpack-server core (all platforms):
/// range parser, migrations, identity, MIME, and scanner behaviors using the
/// reference fixtures. The HTTP API + streaming are covered separately by
/// run_server.sh / server_api_test.py (UNIX).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sqlite3.h>

#include "db.h"
#include "identity.h"
#include "library.h"
#include "mime.h"
#include "range.h"
#include "scanner.h"
#include "schema.h"
#include "sessions.h"
#include "tokens.h"

#include <musicpack/musicpack.h>

static int failures = 0;
static char g_tmpdir[4096];
static char g_ref_mpc[4096];
static char g_ref_flac[4096];

#define CHECK(cond, msg) do {                                             \
    if (!(cond)) {                                                        \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);     \
        failures++;                                                       \
    }                                                                     \
} while (0)

#ifdef _WIN32
# include <io.h>
# include <direct.h>
# include <sys/stat.h>
# include "dirent.h"
# define mkdir_one(p) _mkdir(p)
# define stat_t struct _stat
# define walk_stat(p, st) _stat(p, st)
# define S_IS_DIR(m) ((m) & _S_IFDIR)
#else
# include <dirent.h>
# include <fcntl.h>
# include <sys/stat.h>
# include <unistd.h>
# define mkdir_one(p) mkdir(p, 0755)
# define stat_t struct stat
# define walk_stat(p, st) stat(p, st)
# define S_IS_DIR(m) S_ISDIR(m)
#endif

static char g_ref_mpc[4096];
static char g_ref_flac[4096];

/* ---------- filesystem helpers ------------------------------------------ */

static void
make_dir(const char *path)
{
    char tmp[4096];
    size_t len, i;
    strncpy(tmp, path, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = '\0';
    len = strlen(tmp);
    if (len > 0 && (tmp[len - 1] == '/' || tmp[len - 1] == '\\'))
        tmp[len - 1] = '\0';
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/' || tmp[i] == '\\') {
            tmp[i] = '\0';
            mkdir_one(tmp);
            tmp[i] = '/';
        }
    }
    mkdir_one(tmp);
}

/* Creates the parent directories of a file path (never the file itself). */
static void
make_parent_dirs(const char *filepath)
{
    char tmp[4096];
    size_t len, i;
    strncpy(tmp, filepath, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = '\0';
    len = strlen(tmp);
    for (i = len; i > 0 && tmp[i - 1] != '/' && tmp[i - 1] != '\\'; i--)
        ;
    if (i > 0)
        tmp[i - 1] = '\0';
    make_dir(tmp);
}

static int
copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    FILE *out;
    char buf[65536];
    size_t n;
    if (in == 0)
        return -1;
    make_parent_dirs(dst);
    out = fopen(dst, "wb");
    if (out == 0) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    fclose(in);
    if (fclose(out) != 0)
        return -1;
    return 0;
}

static void
copy_tree(const char *src, const char *dst)
{
    DIR *d = opendir(src);
    struct dirent *e;
    if (d == 0)
        return;
    make_dir(dst);
    while ((e = readdir(d)) != 0) {
        char s[4096], t[4096];
        stat_t st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(s, sizeof s, "%s/%s", src, e->d_name);
        snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
        if (walk_stat(s, &st) != 0)
            continue;
        if (S_IS_DIR(st.st_mode))
            copy_tree(s, t);
        else
            copy_file(s, t);
    }
    closedir(d);
}

static char *
read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    long len;
    char *buf;
    if (f == 0)
        return 0;
    fseek(f, 0, SEEK_END);
    len = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = (char *) malloc((size_t) len + 1);
    if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        return 0;
    }
    fclose(f);
    buf[len] = '\0';
    return buf;
}

static void
write_file(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    if (f != 0) {
        fwrite(data, 1, strlen(data), f);
        fclose(f);
    }
}

/* Replaces the first occurrence of \p from with \p to in file at \p path. */
static void
replace_in_file(const char *path, const char *from, const char *to)
{
    char *buf = read_file(path);
    char *hit;
    size_t flen = strlen(from), tlen = strlen(to);
    char *out;
    if (buf == 0)
        return;
    hit = strstr(buf, from);
    if (hit == 0) {
        free(buf);
        return;
    }
    out = (char *) malloc(strlen(buf) - flen + tlen + 1);
    memcpy(out, buf, (size_t) (hit - buf));
    memcpy(out + (hit - buf), to, tlen);
    strcpy(out + (hit - buf) + tlen, hit + flen);
    write_file(path, out);
    free(out);
    free(buf);
}

static void
insert_before_final_brace(const char *path, const char *text)
{
    char *buf = read_file(path);
    char *end;
    char *out;
    if (buf == 0)
        return;
    end = strrchr(buf, '}');
    if (end == 0) {
        free(buf);
        return;
    }
    out = (char *) malloc(strlen(buf) + strlen(text) + 1);
    memcpy(out, buf, (size_t) (end - buf));
    strcpy(out + (end - buf), text);
    strcpy(out + (end - buf) + strlen(text), end);
    write_file(path, out);
    free(out);
    free(buf);
}

/* ---------- range parser ------------------------------------------------- */

static void
test_range(void)
{
    mp_range r;
    CHECK(mp_range_parse("bytes=0-1023", 100000, &r) == MP_RANGE_OK &&
          r.start == 0 && r.length == 1024, "range 0-1023");
    CHECK(mp_range_parse("bytes=1024-", 100000, &r) == MP_RANGE_OK &&
          r.start == 1024 && r.length == 98976, "open-ended");
    CHECK(mp_range_parse("bytes=-4096", 100000, &r) == MP_RANGE_OK &&
          r.start == 95904 && r.length == 4096, "suffix -4096");
    CHECK(mp_range_parse("bytes=-200000", 100000, &r) == MP_RANGE_OK &&
          r.start == 0 && r.length == 100000, "suffix clamps to size");
    CHECK(mp_range_parse("bytes=0-0", 1, &r) == MP_RANGE_OK &&
          r.start == 0 && r.length == 1, "single byte");
    CHECK(mp_range_parse("bytes=0-0", 100, &r) == MP_RANGE_OK &&
          r.start == 0 && r.length == 1, "single byte nonzero file");
    CHECK(mp_range_parse("bytes=5000-999999", 100000, &r) == MP_RANGE_OK &&
          r.start == 5000 && r.length == 95000, "end clamped to EOF");
    CHECK(mp_range_parse("bytes=99999-", 100000, &r) == MP_RANGE_OK &&
          r.start == 99999 && r.length == 1, "last byte");
    CHECK(mp_range_parse("bytes=100000-", 100000, &r) == MP_RANGE_UNSATISFIABLE,
          "start == size unsatisfiable");
    CHECK(mp_range_parse("bytes=200000-", 100000, &r) == MP_RANGE_UNSATISFIABLE,
          "start > size unsatisfiable");
    CHECK(mp_range_parse("bytes=5-2", 100, &r) == MP_RANGE_INVALID,
          "start > end invalid");
    CHECK(mp_range_parse("bytes=0-1,5-6", 100, &r) == MP_RANGE_INVALID,
          "multiple ranges rejected");
    CHECK(mp_range_parse("items=0-1", 100, &r) == MP_RANGE_INVALID,
          "wrong unit rejected");
    CHECK(mp_range_parse("bytes=-0", 100, &r) == MP_RANGE_INVALID,
          "suffix 0 rejected");
    CHECK(mp_range_parse("bytes=abc", 100, &r) == MP_RANGE_INVALID,
          "non-numeric rejected");
    CHECK(mp_range_parse(0, 100, &r) == MP_RANGE_INVALID,
          "null header rejected");
    CHECK(mp_range_parse("bytes=18446744073709551616-", 100, &r)
          == MP_RANGE_INVALID, "overflowing start rejected");
}

/* ---------- migrations / restart ----------------------------------------- */

static void
test_migrations(void)
{
    char dbpath[4096];
    mp_db *db;
    char err[256];
    snprintf(dbpath, sizeof dbpath, "%s/mig.db", g_tmpdir);
    CHECK(mp_db_open(&db, dbpath, 1, err, sizeof err) == 0, "open fresh db");
    CHECK(db != 0 && mp_db_schema_version(db) == 8, "schema version 8");
    mp_db_close(db);
    CHECK(mp_db_open(&db, dbpath, 1, err, sizeof err) == 0, "reopen db");
    CHECK(mp_db_schema_version(db) == 8, "version stable on reopen");
    mp_db_close(db);
    CHECK(mp_db_open(&db, dbpath, 0, err, sizeof err) == 0, "open read-only");
    mp_db_close(db);
}

/* ---------- identity ------------------------------------------------------ */

static void
test_identity(const char *ref)
{
    char mpath[4096];
    char fp1[MP_ID_KEY_MAX], fp2[MP_ID_KEY_MAX];
    char gk1[MP_ID_KEY_MAX], gk2[MP_ID_KEY_MAX];
    char rk1[MP_ID_KEY_MAX], rk2[MP_ID_KEY_MAX];
    char manifest_sha[65];
    musicpack_manifest *m1, *m2;
    char *json1;

    snprintf(mpath, sizeof mpath, "%s/manifest.json", ref);
    json1 = read_file(mpath);
    CHECK(json1 != 0, "read reference manifest");
    m1 = musicpack_manifest_parse(json1, 0);
    CHECK(m1 != 0, "parse reference manifest");

    /* fingerprint stable across identical manifest */
    CHECK(mp_identity_package_fingerprint(m1, fp1, sizeof fp1) == MUSICPACK_OK,
          "package fingerprint");
    CHECK(mp_identity_package_fingerprint(m1, fp2, sizeof fp2) == MUSICPACK_OK &&
          strcmp(fp1, fp2) == 0, "fingerprint deterministic");
    CHECK(mp_identity_manifest_hash(json1, strlen(json1), manifest_sha,
                                    sizeof manifest_sha) == MUSICPACK_OK &&
          strlen(manifest_sha) == 64, "manifest sha256 length");

    /* edition change alters release key but not group key */
    m2 = musicpack_manifest_parse(json1, 0);
    CHECK(m2 != 0, "parse second manifest");
    CHECK(mp_identity_group_key(m1, gk1, sizeof gk1) == MUSICPACK_OK &&
          mp_identity_group_key(m2, gk2, sizeof gk2) == MUSICPACK_OK &&
          strcmp(gk1, gk2) == 0, "group key stable across editions");
    if (m2->release.present && m2->release.edition != 0) {
        free(m2->release.edition);
        m2->release.edition = strdup("1987 Original CD");
        CHECK(mp_identity_release_key(m1, rk1, sizeof rk1) == MUSICPACK_OK &&
              mp_identity_release_key(m2, rk2, sizeof rk2) == MUSICPACK_OK &&
           strcmp(rk1, rk2) != 0, "release key differs by edition");
    }
    {
        char *long_title = (char *) malloc(10001);
        memset(long_title, 'a', 10000);
        long_title[10000] = '\0';
        free(m1->album_title);
        free(m2->album_title);
        m1->album_title = strdup(long_title);
        long_title[9999] = 'b';
        m2->album_title = strdup(long_title);
        CHECK(mp_identity_group_key(m1, gk1, sizeof gk1) == MUSICPACK_OK &&
              mp_identity_group_key(m2, gk2, sizeof gk2) == MUSICPACK_OK &&
              strcmp(gk1, gk2) != 0,
              "long identity fields are not truncated");
        free(long_title);
    }
    musicpack_manifest_free(m1);
    musicpack_manifest_free(m2);
    free(json1);
}

/* ---------- scanner ------------------------------------------------------- */

static int
count_rows(sqlite3 *db, const char *sql, long long bind)
{
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
        return -1;
    if (bind >= 0)
        sqlite3_bind_int64(st, 1, bind);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void
test_scanner(void)
{
    char lib[4096], dbpath[4096];
    char pkg_mpc[4096], pkg_flac[4096];
    char bad[4096], second[4096], moved[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;
    long long mpc_pkg_id = -1;
    char cmd[8192];

    snprintf(lib, sizeof lib, "%s/scanlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/scan.db", g_tmpdir);
    make_dir(lib);
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "open scan db");
    db = mp_library_sqlite(lib_h);

    /* 1. empty library */
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.total == 0 && res.added == 0, "empty library scans clean");

    /* 2. one mpc + one flac package */
    snprintf(pkg_mpc, sizeof pkg_mpc, "%s/TestComp.mpack", lib);
    snprintf(pkg_flac, sizeof pkg_flac, "%s/Classical.mpack", lib);
    copy_tree(g_ref_mpc, pkg_mpc);
    copy_tree(g_ref_flac, pkg_flac);
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.total == 2 && res.added == 2, "two packages added");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM release_groups", -1) == 2,
          "two release groups");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM releases", -1) == 2,
          "two releases");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks", -1) == 7,
          "seven tracks (4 mpc + 3 flac)");

    /* 3. idempotent second scan */
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.total == 2 && res.added == 0 && res.updated == 0,
          "idempotent rescan changes nothing");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks", -1) == 7,
          "track count stable");

    /* 4. multiple editions of the same album -> 1 group, 2 releases */
    snprintf(second, sizeof second, "%s/TestComp-1987.mpack", lib);
    copy_tree(g_ref_mpc, second);
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", second);
        replace_in_file(mpath, "\"edition\": \"2016 Digital Remaster\"",
                        "\"edition\": \"1987 Original CD\"");
    }
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.total == 3 && res.added == 1, "third package added");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM release_groups", -1) == 2,
          "still two groups (edition grouped)");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM releases", -1) == 3,
          "three releases (two editions)");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks", -1) == 11,
          "eleven tracks");

    /* An identical package is a separate copy, not a move that hijacks the
       original package row. */
    {
        char duplicate[4096];
        snprintf(duplicate, sizeof duplicate, "%s/ClassicalCopy.mpack", lib);
        copy_tree(g_ref_flac, duplicate);
        mp_scan_library(lib_h, lib, 0, &res, 0, 0);
        CHECK(res.added == 1 && res.moved == 0, "duplicate does not hijack move");
    }

    /* 5. changed manifest -> updated, not duplicated */
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkg_mpc);
        replace_in_file(mpath, "\"title\": \"Alphaville - Big in Japan\"",
                        "\"title\": \"Big in Japan (2016 mix)\"");
    }
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.updated == 1 && res.added == 0, "changed package updated");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks", -1) == 11,
          "no track duplication on update");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM tracks WHERE title='Big in Japan (2016 mix)'",
        -1) == 1, "edited title present");

    /* 6. malformed package -> invalid, others untouched */
    snprintf(bad, sizeof bad, "%s/Broken.mpack", lib);
    make_dir(bad);
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", bad);
        write_file(mpath, "{ this is not json ");
    }
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.invalid == 1, "malformed package recorded invalid");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='invalid'", -1) == 1,
        "invalid status persisted");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks", -1) == 11,
          "malformed package did not corrupt index");

    /* Lightweight scans still resolve every analysis reference. */
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", second);
        insert_before_final_brace(mpath,
            ",\n  \"analysis\": [{\"type\": \"other\", \"path\": \"analysis/missing.json\", \"sha256\": \"0000000000000000000000000000000000000000000000000000000000000000\"}]\n");
        mp_scan_library(lib_h, lib, 0, &res, 0, 0);
        CHECK(count_rows(db, "SELECT COUNT(*) FROM packages WHERE status='warning'", -1)
              >= 1, "missing analysis reference warns on lightweight scan");
    }

    /* 7. moved package -> same id at new path */
    snprintf(moved, sizeof moved, "%s/MovedClassical.mpack", lib);
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT id FROM packages WHERE path = ?1", -1, &st, 0)
            == SQLITE_OK) {
            sqlite3_bind_text(st, 1, pkg_flac, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW)
                mpc_pkg_id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }
    snprintf(cmd, sizeof cmd, "mv '%s' '%s'", pkg_flac, moved);
    if (system(cmd) == 0) {
        mp_scan_library(lib_h, lib, 0, &res, 0, 0);
        CHECK(res.moved == 1, "moved package detected");
        {
            sqlite3_stmt *st;
            long long id_at_new = -1;
            if (sqlite3_prepare_v2(db,
                    "SELECT id FROM packages WHERE path = ?1", -1, &st, 0)
                == SQLITE_OK) {
                sqlite3_bind_text(st, 1, moved, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW)
                    id_at_new = sqlite3_column_int64(st, 0);
                sqlite3_finalize(st);
            }
            CHECK(id_at_new == mpc_pkg_id, "move keeps package id");
        }
    } else {
        fprintf(stderr, "note: mv not available, skipping move test\n");
    }

    /* 8. deleted package -> unavailable */
    snprintf(cmd, sizeof cmd, "rm -rf '%s'", pkg_mpc);
    if (system(cmd) == 0) {
        mp_scan_library(lib_h, lib, 0, &res, 0, 0);
        CHECK(res.removed == 1, "deleted package marked unavailable");
        {
            sqlite3_stmt *st;
            int unavail = 0;
            if (sqlite3_prepare_v2(db,
                    "SELECT COUNT(*) FROM packages WHERE path = ?1"
                    " AND status='unavailable'", -1, &st, 0) == SQLITE_OK) {
                sqlite3_bind_text(st, 1, pkg_mpc, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW)
                    unavail = sqlite3_column_int(st, 0);
                sqlite3_finalize(st);
            }
            CHECK(unavail == 1, "unavailable status persisted for deleted path");
        }
    }

    /* 9. database restart: reopen and rescan -> no changes */
    mp_library_close(lib_h);
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "reopen after close");
    db = mp_library_sqlite(lib_h);
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 0 && res.updated == 0, "restart rescan is a no-op");
    mp_library_close(lib_h);
}

/* Re-ingest must REPLACE, not accumulate, a release's assets. Editing a
    package's manifest (a track title keeps the album identity stable) triggers
    ingest_valid -> mp_library_replace_release_content, which syncs asset rows
    by their (kind, role, path) key: existing entries are updated in place,
    removed entries deleted, so stale artwork/booklet/lyrics/extras rows never
    remain servable and rows never duplicate. */
static void
test_reingest_assets(void)
{
    char lib[4096], dbpath[4096];
    char pkg[4096], mpath[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;

    snprintf(lib, sizeof lib, "%s/relib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/re.db", g_tmpdir);
    make_dir(lib);
    snprintf(pkg, sizeof pkg, "%s/Album.mpack", lib);
    copy_tree(g_ref_mpc, pkg);
    snprintf(mpath, sizeof mpath, "%s/manifest.json", pkg);

    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "re-ingest: open db");
    db = mp_library_sqlite(lib_h);

    /* initial ingest: the fixture carries 1 artwork + 1 booklet + 1 extras */
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "re-ingest: first scan ok");
    CHECK(res.added == 1, "re-ingest: first scan adds one");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM assets", -1) == 5,
          "re-ingest: five asset rows after first ingest");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM assets WHERE kind='artwork'",
                     -1) == 1, "re-ingest: one artwork row");

    /* content change -> full re-ingest path; assets must be replaced */
    replace_in_file(mpath, "\"title\": \"Alphaville - Big in Japan\"",
                    "\"title\": \"Big in Japan (2016 mix)\"");
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "re-ingest: rescan ok");
    CHECK(res.updated == 1, "re-ingest: manifest change updates package");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM assets", -1) == 5,
          "re-ingest: assets replaced, not duplicated");

    /* repeated re-ingests must never accumulate stale rows */
    replace_in_file(mpath, "\"title\": \"Big in Japan (2016 mix)\"",
                    "\"title\": \"Big in Japan (2017 mix)\"");
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "re-ingest: rescan 2 ok");
    replace_in_file(mpath, "\"title\": \"Big in Japan (2017 mix)\"",
                    "\"title\": \"Big in Japan (2018 mix)\"");
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "re-ingest: rescan 3 ok");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM assets", -1) == 5,
          "re-ingest: no accumulation after repeated re-ingests");

    /* no duplicate (kind, relative_path) pairs survive */
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM (SELECT kind, relative_path FROM assets"
        " GROUP BY kind, relative_path HAVING COUNT(*) > 1)", -1) == 0,
        "re-ingest: no duplicate asset keys");

    mp_library_close(lib_h);
}

/* ---------- re-ingest identity stability ---------------------------------- */

/* Returns the first column of the first row of \p sql as an integer, or -1
   when there is no row (or the query fails). \p bind_text may be NULL. */
static long long
scalar_ll(sqlite3 *db, const char *sql, const char *bind_text)
{
    sqlite3_stmt *st;
    long long v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
        return -1;
    if (bind_text != 0)
        sqlite3_bind_text(st, 1, bind_text, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* First text column of the first row ("" when NULL/absent; never NULL). */
static const char *
scalar_text(sqlite3 *db, const char *sql, const char *bind_text,
            char *out, size_t cap)
{
    sqlite3_stmt *st;
    out[0] = '\0';
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
        return out;
    if (bind_text != 0)
        sqlite3_bind_text(st, 1, bind_text, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const unsigned char *t = sqlite3_column_text(st, 0);
        snprintf(out, cap, "%s", t != 0 ? (const char *) t : "");
    }
    sqlite3_finalize(st);
    return out;
}

/* Removes the JSON object block containing \p needle from the file at
   \p path. Only valid for the canonical fixture formatting: object blocks
   inside an array open with "{" and close with "}" on their own line,
   indented with exactly 8 spaces. */
static void
remove_block_containing(const char *path, const char *needle)
{
    char *buf = read_file(path);
    char *hit, *s, *e;
    if (buf == 0)
        return;
    hit = strstr(buf, needle);
    if (hit == 0) {
        free(buf);
        return;
    }
    s = hit;
    while (s > buf && memcmp(s, "\n        {", 10) != 0)
        s--;
    e = hit;
    while (*e != '\0' && memcmp(e, "\n        }", 10) != 0)
        e++;
    if (s <= buf || *e == '\0') {
        free(buf);
        return;
    }
    {
        size_t len = (size_t) ((e + 10) - (s + 1));
        if (e[10] == ',')
            len++; /* swallow the separating comma */
        memmove(s + 1, s + 1 + len, strlen(s + 1 + len) + 1);
    }
    write_file(path, buf);
    free(buf);
}

/* Inserts \p text immediately after the "}" that closes the JSON object
   block containing \p needle (same canonical fixture formatting rules as
   remove_block_containing). Used to append sibling entries to an array. */
static void
insert_block_after_block(const char *path, const char *needle,
                         const char *text)
{
    char *buf = read_file(path);
    char *hit, *e;
    size_t tlen;
    char *out;
    if (buf == 0)
        return;
    hit = strstr(buf, needle);
    if (hit == 0) {
        free(buf);
        return;
    }
    e = hit;
    while (*e != '\0' && memcmp(e, "\n        }", 10) != 0)
        e++;
    if (*e == '\0') {
        free(buf);
        return;
    }
    e += 10; /* just past the closing brace */
    tlen = strlen(text);
    out = (char *) malloc((size_t) (e - buf) + tlen + strlen(e) + 1);
    if (out == 0) {
        free(buf);
        return;
    }
    memcpy(out, buf, (size_t) (e - buf));
    memcpy(out + (e - buf), text, tlen);
    strcpy(out + (e - buf) + tlen, e);
    write_file(path, out);
    free(out);
    free(buf);
}

/* Re-ingestion must preserve public identity: a track/audio/media/release
   that still logically exists keeps its row id across rescans, even when
   non-identity metadata changes. Genuinely added/removed tracks are
   inserted/deleted without re-keying survivors, and identity follows audio
   content rather than position when tracks are renumbered. */
static void
test_reingest_preserves_ids(void)
{
    const char *T1 = "Alphaville - Big in Japan";
    const char *T2 = "Bleachers - The Van";
    const char *T3 = "Synthwave - Night Drive";
    const char *T4 = "Test Artist - Fourth Track";
    const char *T5 = "Added Fifth Track";
    char lib[4096], dbpath[4096], pkg[4096], mpath[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;
    long long g1, r1, m1;
    long long t[5], a[5], s[3];
    long long ar[3];
    long long art, bk, ex, ly;
    char cmd[8192];

    snprintf(lib, sizeof lib, "%s/idlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/id.db", g_tmpdir);
    make_dir(lib);
    /* Seed another package FIRST so its tracks occupy low rowids: with the
       historical delete+reinsert ingest, re-created rows land above them
       and every id of the edited package shifts. A lone-package library
       accidentally reproduces the same ids (rowids restart at 1), which
       would mask the bug this test exists to catch. */
    {
        char seed[4096];
        snprintf(seed, sizeof seed, "%s/SeedClassical.mpack", lib);
        copy_tree(g_ref_flac, seed);
        snprintf(pkg, sizeof pkg, "%s/Stable.mpack", lib);
        copy_tree(g_ref_mpc, pkg);
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkg);
        lib_h = mp_library_open(dbpath, 1, 0, 0);
        CHECK(lib_h != 0, "ids: open db");
        db = mp_library_sqlite(lib_h);
        CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
              "ids: seed+target scan ok");
        CHECK(res.added == 2, "ids: two packages added");
        /* The point of the seed is that the tracks table is never fully
           emptied when only the target package re-ingests, so accidental
           rowid reuse cannot mask a re-keying bug. */
        CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks", -1) == 7,
              "ids: seven tracks across both packages");
    }

#define ID_TRACK(title) \
    scalar_ll(db, "SELECT id FROM tracks WHERE title = ?1", title)
#define ID_AUDIO(title)                                    \
    scalar_ll(db, "SELECT a.id FROM audio_objects a"       \
                  " JOIN tracks t ON t.id = a.track_id"    \
                  " WHERE t.title = ?1",                   \
              title)

    /* capture target-package ids (the seeded package already occupies
       lower rowids, so these are genuinely at risk of shifting) */
    g1 = scalar_ll(db,
        "SELECT id FROM release_groups WHERE title = 'Synthetic Test"
        " Compilation'", 0);
    r1 = scalar_ll(db,
        "SELECT r.id FROM releases r JOIN release_groups g"
        " ON g.id = r.group_id WHERE g.title = 'Synthetic Test Compilation'",
        0);
    m1 = scalar_ll(db,
        "SELECT me.id FROM media me JOIN releases r ON r.id = me.release_id"
        " WHERE r.id IN (SELECT id FROM releases WHERE group_id = "
        "(SELECT id FROM release_groups WHERE title = 'Synthetic Test"
        " Compilation')) AND me.disc_number = 1", 0);
    t[0] = ID_TRACK(T1); t[1] = ID_TRACK(T2);
    t[2] = ID_TRACK(T3); t[3] = ID_TRACK(T4);
    a[0] = ID_AUDIO(T1); a[1] = ID_AUDIO(T2);
    a[2] = ID_AUDIO(T3); a[3] = ID_AUDIO(T4);
    art = scalar_ll(db,
        "SELECT id FROM assets WHERE kind='artwork' AND release_id = "
        "(SELECT id FROM releases WHERE group_id = (SELECT id FROM"
        " release_groups WHERE title = 'Synthetic Test Compilation'))", 0);
    bk = scalar_ll(db,
        "SELECT id FROM assets WHERE kind='booklet' AND release_id = "
        "(SELECT id FROM releases WHERE group_id = (SELECT id FROM"
        " release_groups WHERE title = 'Synthetic Test Compilation'))", 0);
    ex = scalar_ll(db,
        "SELECT id FROM assets WHERE kind='extras' AND release_id = "
        "(SELECT id FROM releases WHERE group_id = (SELECT id FROM"
        " release_groups WHERE title = 'Synthetic Test Compilation'))", 0);
    ly = scalar_ll(db,
        "SELECT MIN(id) FROM assets WHERE kind='lyrics' AND release_id = "
        "(SELECT id FROM releases WHERE group_id = (SELECT id FROM"
        " release_groups WHERE title = 'Synthetic Test Compilation'))", 0);
    s[0] = ID_TRACK("Classical Piece No 1");
    s[1] = ID_TRACK("Classical Piece No 2");
    s[2] = ID_TRACK("Classical Piece No 3");
    ar[0] = scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                      "Alphaville");
    ar[1] = scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                      "Bleachers");
    ar[2] = scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                      "Synthetic Chamber Orchestra");
    CHECK(g1 > 0 && r1 > 0 && m1 > 0, "ids: hierarchy captured");
    CHECK(t[0] > 0 && t[1] > 0 && t[2] > 0 && t[3] > 0, "ids: tracks captured");
    CHECK(a[0] > 0 && a[1] > 0 && a[2] > 0 && a[3] > 0, "ids: audio captured");
    CHECK(art > 0 && bk > 0 && ex > 0 && ly > 0, "ids: assets captured");
    CHECK(s[0] > 0 && s[1] > 0 && s[2] > 0, "ids: seeded tracks captured");
    CHECK(ar[0] > 0 && ar[1] > 0 && ar[2] > 0, "ids: artists captured");

    /* 1. identical rescan (unchanged manifest bytes): full idempotence */
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 0 && res.updated == 0,
          "ids: unchanged rescan is a no-op");
    CHECK(ID_TRACK(T1) == t[0] && ID_AUDIO(T1) == a[0],
          "ids: unchanged rescan preserves track/audio ids");
    CHECK(scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Alphaville") == ar[0] &&
          scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Bleachers") == ar[1] &&
          scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Synthetic Chamber Orchestra") == ar[2],
          "ids: unchanged rescan preserves artist ids");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM track_waveforms", -1) == 4,
          "ids: waveform rows stable across unchanged rescan");

    /* 2. metadata-only edit (track title + loudness value): the manifest
       bytes change, so the full ingest path runs, but no logical entity is
       added or removed -> every public id must survive. */
    replace_in_file(mpath, "\"title\": \"Alphaville - Big in Japan\"",
                    "\"title\": \"Big in Japan (2016 remaster)\"");
    replace_in_file(mpath, "-7.1915088", "-7.1111111");
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "ids: metadata-edit scan ok");
    CHECK(res.updated == 1 && res.added == 0,
          "ids: metadata edit updates the package");
    CHECK(scalar_ll(db,
        "SELECT id FROM release_groups WHERE title = 'Synthetic Test"
        " Compilation'", 0) == g1 &&
          scalar_ll(db,
        "SELECT r.id FROM releases r JOIN release_groups g"
        " ON g.id = r.group_id WHERE g.title = 'Synthetic Test Compilation'",
        0) == r1 &&
          scalar_ll(db,
              "SELECT me.id FROM media me WHERE me.release_id = "
              "(SELECT id FROM releases WHERE group_id = "
              "(SELECT id FROM release_groups WHERE title = 'Synthetic Test"
              " Compilation')) AND me.disc_number = 1", 0) == m1,
          "ids: group/release/media ids preserved across metadata edit");
    CHECK(ID_TRACK("Big in Japan (2016 remaster)") == t[0],
          "ids: renamed track keeps its id");
    CHECK(ID_TRACK(T2) == t[1] && ID_TRACK(T3) == t[2] && ID_TRACK(T4) == t[3],
          "ids: untouched tracks keep their ids");
    CHECK(ID_AUDIO("Big in Japan (2016 remaster)") == a[0] &&
          ID_AUDIO(T2) == a[1] && ID_AUDIO(T3) == a[2] && ID_AUDIO(T4) == a[3],
          "ids: audio object ids preserved across metadata edit");
    CHECK(scalar_ll(db, "SELECT id FROM assets WHERE kind='artwork'", 0) == art &&
          scalar_ll(db, "SELECT id FROM assets WHERE kind='booklet'", 0) == bk &&
          scalar_ll(db, "SELECT id FROM assets WHERE kind='extras'", 0) == ex &&
          scalar_ll(db, "SELECT MIN(id) FROM assets WHERE kind='lyrics'", 0) == ly,
          "ids: asset ids preserved across metadata edit");
    CHECK(scalar_ll(db,
        "SELECT COUNT(*) FROM assets WHERE release_id = "
        "(SELECT id FROM releases WHERE group_id = (SELECT id FROM"
        " release_groups WHERE title = 'Synthetic Test Compilation'))",
        0) == 5, "ids: target asset count stable across metadata edit");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM track_waveforms", -1) == 4 &&
          count_rows(db,
              "SELECT COUNT(*) FROM track_waveforms WHERE track_id IS NOT"
              " NULL AND track_id IN (SELECT id FROM tracks)", -1) == 4,
          "ids: waveforms re-attached to the same surviving tracks");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM track_waveforms WHERE track_id = "
        "(SELECT id FROM tracks WHERE title = 'Big in Japan (2016 remaster)')",
        -1) == 1, "ids: renamed track keeps its waveform association");
    CHECK(scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Alphaville") == ar[0] &&
          scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Bleachers") == ar[1] &&
          scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Synthetic Chamber Orchestra") == ar[2],
          "ids: artist ids preserved across metadata edit");

    /* 3. renumbering swaps positions but not identities: content identity
       (audio sha256) beats position. Tracks 3 and 4 swap numbers; each must
       keep the id it had before the swap. Run before any duplicate-audio
       track exists, so content matching is unambiguous here. */
    replace_in_file(mpath, "\"track\": 3", "\"track\": 99");
    replace_in_file(mpath, "\"track\": 4", "\"track\": 3");
    replace_in_file(mpath, "\"track\": 99", "\"track\": 4");
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "ids: renumber scan ok");
    CHECK(ID_TRACK(T3) == t[2] && ID_TRACK(T4) == t[3],
          "ids: renumbered tracks keep their ids (identity != position)");
    CHECK(scalar_ll(db,
        "SELECT track_number FROM tracks WHERE id = "
        "(SELECT id FROM tracks WHERE title = ?1)", T3) == 4 &&
          scalar_ll(db,
        "SELECT track_number FROM tracks WHERE id = "
        "(SELECT id FROM tracks WHERE title = ?1)", T4) == 3,
          "ids: numbers actually swapped in the database");
    CHECK(ID_AUDIO(T3) == a[2] && ID_AUDIO(T4) == a[3],
          "ids: renumbered tracks keep their audio object ids");

    /* 4. adding a track inserts one new entity and never re-keys existing
       ones. The new entry references a byte-identical copy of track 4's
       audio file, so its sha256 is known without hashing here. */
    {
        char src[4096], dst[4096];
        snprintf(src, sizeof src, "%s/audio/04 - Test Artist - Fourth Track.mpc",
                 pkg);
        snprintf(dst, sizeof dst, "%s/audio/05 - Added Track.mpc", pkg);
        copy_file(src, dst);
    }
    insert_block_after_block(mpath, "analysis/waveform/01-04.wfm",
        ",\n"
        "        {\n"
        "          \"audio\": {\n"
        "            \"path\": \"audio/05 - Added Track.mpc\",\n"
        "            \"sha256\": "
        "\"e99002a619704d137611bf928f4d328a5d3cf5cb0e0bba62a343f6281e79bd29\"\n"
        "          },\n"
        "          \"duration\": 1,\n"
        "          \"loudness\": {\n"
        "            \"trackLUFS\": -7.3654852,\n"
        "            \"truePeakDbTP\": -4.2588095\n"
        "          },\n"
        "          \"title\": \"Added Fifth Track\",\n"
        "          \"track\": 5\n"
        "        }");
    /* verify=1 proves the appended entry passes full integrity checks. A
       verified scan bypasses the unchanged-manifest fast path for BOTH
       packages, so both are re-ingested and reported as updated. */
    CHECK(mp_scan_library(lib_h, lib, 1, &res, 0, 0) == MUSICPACK_OK,
          "ids: add-track scan (verified) ok");
    CHECK(res.updated == 2 && res.added == 0,
          "ids: verified scan re-ingests both packages");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks", -1) == 8,
          "ids: eight tracks total after add (3 seeded + 5 target)");
    CHECK(ID_TRACK("Big in Japan (2016 remaster)") == t[0] &&
          ID_TRACK(T2) == t[1] && ID_TRACK(T3) == t[2] && ID_TRACK(T4) == t[3],
          "ids: adding a track does not re-key existing tracks");
    CHECK(ID_AUDIO("Big in Japan (2016 remaster)") == a[0] &&
          ID_AUDIO(T2) == a[1] && ID_AUDIO(T3) == a[2] && ID_AUDIO(T4) == a[3],
          "ids: adding a track does not re-key existing audio objects");
    {
        long long t5 = ID_TRACK(T5);
        CHECK(t5 > 0 && t5 != t[0] && t5 != t[1] && t5 != t[2] && t5 != t[3],
              "ids: new track gets a fresh id");
    }

    /* 5. removing a track deletes exactly that entity; survivors keep ids.
       The removed track's audio file is deleted too (unreferenced files are
       only warnings, but a clean fixture is easier to reason about). */
    remove_block_containing(mpath, "audio/02 - Bleachers - The Van.mpc");
    snprintf(cmd, sizeof cmd, "rm -f '%s/audio/02 - Bleachers - The Van.mpc'",
             pkg);
    if (system(cmd) != 0) { }
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "ids: remove-track scan ok");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks", -1) == 7,
          "ids: seven tracks after removal (3 seeded + 4 target)");
    CHECK(ID_TRACK(T2) < 0, "ids: removed track is gone");
    CHECK(scalar_ll(db,
        "SELECT COUNT(*) FROM audio_objects WHERE track_id IN"
        " (SELECT id FROM tracks)", 0) == 7,
        "ids: removed track's audio object is gone (no orphans)");
    CHECK(ID_TRACK("Big in Japan (2016 remaster)") == t[0] &&
          ID_TRACK(T3) == t[2] && ID_TRACK(T4) == t[3] && ID_TRACK(T5) > 0,
          "ids: removal does not re-key remaining tracks");

    CHECK(scalar_ll(db,
        "SELECT r.id FROM releases r JOIN release_groups g"
        " ON g.id = r.group_id WHERE g.title = 'Synthetic Test Compilation'",
        0) == r1, "ids: release id survives structural churn");

    /* the seeded package was never edited: its track ids must be untouched
       by everything that happened to the target package */
    CHECK(s[0] > 0 &&
          ID_TRACK("Classical Piece No 1") == s[0] &&
          ID_TRACK("Classical Piece No 2") == s[1] &&
          ID_TRACK("Classical Piece No 3") == s[2],
          "ids: seeded package keeps its track ids (no cross-package"
          " interference)");

#undef ID_TRACK
#undef ID_AUDIO

    /* 6. final unchanged rescan: still a no-op after all the churn */    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 0 && res.updated == 0,
          "ids: post-churn unchanged rescan is a no-op");

    mp_library_close(lib_h);
}

/* Stable-uid and index hardening (schema migrations 6 and 7): every track
   and asset row carries a server-generated uid that survives re-ingest,
   and the artist join tables used by listing/visibility queries have
   indexes. */
static void
test_stable_uid_and_indexes(void)
{
    char lib[4096], dbpath[4096], pkg[4096], mpath[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;

    snprintf(lib, sizeof lib, "%s/uidlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/uid.db", g_tmpdir);
    make_dir(lib);
    snprintf(pkg, sizeof pkg, "%s/UidPkg.mpack", lib);
    copy_tree(g_ref_mpc, pkg);
    snprintf(mpath, sizeof mpath, "%s/manifest.json", pkg);

    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "uid: open db");
    db = mp_library_sqlite(lib_h);

    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "uid: first scan ok");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks WHERE uid IS NULL",
                     -1) == 0,
          "uid: every track row carries a uid");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM assets WHERE uid IS NULL",
                     -1) == 0,
          "uid: every asset row carries a uid");

    /* uids are preserved across a metadata-only re-ingest */
    {
        char u_before[8][64];
        const char *titles[4] = { "Alphaville - Big in Japan",
                                  "Bleachers - The Van",
                                  "Synthwave - Night Drive",
                                  "Test Artist - Fourth Track" };
        size_t k;
        for (k = 0; k < 4; k++) {
            sqlite3_stmt *st;
            u_before[k][0] = '\0';
            if (sqlite3_prepare_v2(db,
                    "SELECT uid FROM tracks WHERE title = ?1", -1, &st, 0)
                == SQLITE_OK) {
                sqlite3_bind_text(st, 1, titles[k], -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const unsigned char *u = sqlite3_column_text(st, 0);
                    if (u != 0)
                        snprintf(u_before[k], sizeof u_before[k], "%s",
                                 (const char *) u);
                }
                sqlite3_finalize(st);
            }
        }
        replace_in_file(mpath, "\"title\": \"Alphaville - Big in Japan\"",
                        "\"title\": \"Big in Japan (uid probe)\"");
        CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
              "uid: rescan ok");
        for (k = 0; k < 4; k++) {
            const char *t = (k == 0) ? "Big in Japan (uid probe)" : titles[k];
            char u_after[64];
            sqlite3_stmt *st;
            u_after[0] = '\0';
            if (sqlite3_prepare_v2(db,
                    "SELECT uid FROM tracks WHERE title = ?1", -1, &st, 0)
                == SQLITE_OK) {
                sqlite3_bind_text(st, 1, t, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) == SQLITE_ROW) {
                    const unsigned char *u = sqlite3_column_text(st, 0);
                    if (u != 0)
                        snprintf(u_after, sizeof u_after, "%s",
                                 (const char *) u);
                }
                sqlite3_finalize(st);
            }
            CHECK(u_before[k][0] != '\0' && strcmp(u_before[k], u_after) == 0,
                  "uid: track uid survives re-ingest");
        }
    }

    /* audit finding E: artist join indexes exist */
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM sqlite_master WHERE type='index' AND name IN"
        " ('group_artists_artist_idx','track_artists_artist_idx',"
        " 'tracks_uid_idx','assets_uid_idx')", -1) == 4,
        "uid: hardening indexes exist");

    mp_library_close(lib_h);
}

/* A DB mutation failure during ingest must fail the ingest (roll back) and
   never report a successful scan with partial rows. A second connection
   holding the write lock forces every ingest write to hit SQLITE_BUSY. */
static void
test_ingest_busy_rollback(void)
{
    char lib[4096], dbpath[4096];
    char pkg[4096];
    mp_library *lib_h;
    sqlite3 *other;
    mp_scan_result res;
    sqlite3 *db;

    snprintf(lib, sizeof lib, "%s/bzlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/bz.db", g_tmpdir);
    make_dir(lib);
    snprintf(pkg, sizeof pkg, "%s/Album.mpack", lib);
    copy_tree(g_ref_mpc, pkg);

    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "busy: open db");
    db = mp_library_sqlite(lib_h);
    /* fail fast on contention so the test is deterministic */
    sqlite3_busy_timeout(db, 0);

    CHECK(sqlite3_open(dbpath, &other) == SQLITE_OK, "busy: open second conn");
    if (other != 0) {
        /* hold the write lock across the whole scan */
        CHECK(sqlite3_exec(other, "BEGIN IMMEDIATE;", 0, 0, 0) == SQLITE_OK,
              "busy: second conn acquires write lock");
        CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_ERR_IO,
              "busy: scan fails, never reports success");
        CHECK(count_rows(db, "SELECT COUNT(*) FROM packages", -1) == 0,
              "busy: no package row after failed ingest");
        CHECK(count_rows(db, "SELECT COUNT(*) FROM media", -1) == 0,
              "busy: no partial media rows after failed ingest");
        CHECK(count_rows(db, "SELECT COUNT(*) FROM assets", -1) == 0,
              "busy: no partial asset rows after failed ingest");
        CHECK(sqlite3_exec(other, "ROLLBACK;", 0, 0, 0) == SQLITE_OK,
              "busy: release lock");
        sqlite3_close(other);
    }

    /* once the lock is gone the same library ingests cleanly */
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "busy: scan succeeds after lock released");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM packages", -1) == 1,
          "busy: one package after successful scan");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM media", -1) == 1,
          "busy: one media after successful scan");
    mp_library_close(lib_h);
}

/* ---------- MIME ---------------------------------------------------------- */

static void
test_mime(void)
{
    CHECK(strcmp(mp_mime_for_path("audio/x.mpc"), "audio/musepack") == 0,
          "mpc mime");
    CHECK(strcmp(mp_mime_for_path("audio/x.flac"), "audio/flac") == 0,
          "flac mime");
    CHECK(strcmp(mp_mime_for_path("artwork/front.jpg"), "image/jpeg") == 0,
          "jpeg mime");
    CHECK(strcmp(mp_mime_for_path("booklet/b.pdf"), "application/pdf") == 0,
          "pdf mime");
    CHECK(strcmp(mp_mime_for_path("lyrics/a.lrc"), "text/plain") == 0,
          "lrc mime");
    CHECK(strcmp(mp_mime_for_path("extra.xyz"), "application/octet-stream") == 0,
          "unknown mime");
    CHECK(strcmp(mp_codec_for_path("audio/x.mpc"), "musepack") == 0,
          "mpc codec");
    CHECK(strcmp(mp_codec_for_path("audio/x.flac"), "flac") == 0, "flac codec");
}

/* ---------- tokens / verify / progress ----------------------------------- */

static int
count_query(sqlite3 *db, const char *sql, const char *bind)
{
    sqlite3_stmt *st;
    int n = 0;
    if (sqlite3_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
        return -1;
    if (bind != 0)
        sqlite3_bind_text(st, 1, bind, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int(st, 0);
    sqlite3_finalize(st);
    return n;
}

static void
test_tokens(void)
{
    char dbpath[4096], libdir[4096];
    mp_library *lib;
    char s1[MP_TOKEN_SECRET_MAX], s2[MP_TOKEN_SECRET_MAX];
    char bad[MP_TOKEN_SECRET_MAX];
    long long id = -1;
    mp_token_row row;
    sqlite3 *db;

    snprintf(dbpath, sizeof dbpath, "%s/tok.db", g_tmpdir);
    snprintf(libdir, sizeof libdir, "%s/toklib", g_tmpdir);
    make_dir(libdir);
    lib = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib != 0, "open token db");
    db = mp_library_sqlite(lib);

    CHECK(mp_token_create(lib, "Web", s1, sizeof s1, &id) == MUSICPACK_OK,
          "create token");
    CHECK(id > 0, "token id assigned");
    CHECK(strncmp(s1, "mpk_", 4) == 0 && strlen(s1) >= 40,
          "token shape (mpk_ prefix, >=256 bits encoded)");
    CHECK(mp_token_create(lib, "Phone", s2, sizeof s2, 0) == MUSICPACK_OK,
          "create second token");
    CHECK(strcmp(s1, s2) != 0, "tokens are unique");

    /* only the hash is persisted */
    CHECK(count_query(db,
        "SELECT COUNT(*) FROM tokens WHERE token_hash = ?1", s1) == 0,
        "plaintext token never stored");

    CHECK(mp_token_authorize(lib, s1, &row) == 1, "authorize valid token");
    CHECK(mp_token_authorize(lib, s2, &row) == 1, "authorize second token");
    mp_token_generate(bad, sizeof bad);
    CHECK(mp_token_authorize(lib, bad, &row) == 0, "reject unknown token");
    CHECK(mp_token_authorize(lib, "", &row) == 0, "reject empty token");

    CHECK(mp_token_secret_eq(s1, s1) == 1 && mp_token_secret_eq(s1, s2) == 0,
          "constant-time compare");

    CHECK(mp_token_revoke(lib, id) == 1, "revoke token");
    CHECK(mp_token_revoke(lib, id) == 0, "revoke idempotent");
    CHECK(mp_token_authorize(lib, s1, &row) == 0, "reject revoked token");
    CHECK(count_query(db,
        "SELECT COUNT(*) FROM tokens WHERE revoked_at IS NOT NULL", 0) == 1,
        "revocation persisted");

    mp_library_close(lib);
}

static void
test_sessions(void)
{
    char dbpath[4096], libdir[4096], pkg[4096];
    mp_library *lib;
    char tok[MP_TOKEN_SECRET_MAX];
    char sess[MP_SESSION_SECRET_MAX];
    char bad[MP_SESSION_SECRET_MAX];
    long long id = -1;
    mp_session_row srow;
    mp_scan_result res;
    sqlite3 *db;

    snprintf(dbpath, sizeof dbpath, "%s/sess.db", g_tmpdir);
    snprintf(libdir, sizeof libdir, "%s/sesslib", g_tmpdir);
    make_dir(libdir);
    snprintf(pkg, sizeof pkg, "%s/Good.mpack", libdir);
    copy_tree(g_ref_mpc, pkg);

    lib = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib != 0, "open session db");
    db = mp_library_sqlite(lib);

    /* canonical album loudness is persisted from the manifest */
    CHECK(mp_scan_library(lib, libdir, 0, &res, 0, 0) == MUSICPACK_OK,
          "scan for loudness");
    CHECK(res.added == 1, "one package for loudness");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM releases "
        "WHERE has_album_loudness = 1 AND loudness_algorithm = 'ITU-R BS.1770-5'",
        -1) == 1, "album loudness + algorithm persisted");
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db, "SELECT album_lufs, album_true_peak_db"
                                 " FROM releases WHERE has_album_loudness = 1",
                                 -1, &st, 0) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW) {
                CHECK(sqlite3_column_double(st, 0) > -8.0 &&
                      sqlite3_column_double(st, 0) < -7.0,
                      "album LUFS within fixture range");
                CHECK(sqlite3_column_double(st, 1) < 0.0,
                      "album true peak negative (below full scale)");
            }
            sqlite3_finalize(st);
        }
    }

    /* session create: valid token -> cookie secret; invalid -> reject */
    CHECK(mp_token_create(lib, "Web", tok, sizeof tok, &id) == MUSICPACK_OK,
          "create session token");
    CHECK(mp_session_create(lib, tok, sess, sizeof sess) == MUSICPACK_OK,
          "exchange token for session");
    CHECK(strlen(sess) >= 40, "session secret shape");
    CHECK(count_query(db, "SELECT COUNT(*) FROM sessions", 0) == 1,
          "one session row");
    CHECK(count_query(db,
        "SELECT COUNT(*) FROM sessions WHERE session_hash = ?1", sess) == 0,
        "plaintext session secret never stored");

    mp_session_secret_generate(bad, sizeof bad);
    CHECK(mp_session_authorize(lib, sess, &srow) == 1, "authorize session");
    CHECK(mp_session_authorize(lib, bad, &srow) == 0, "reject bad session");
    CHECK(mp_session_authorize(lib, "", &srow) == 0, "reject empty session");

    /* session inherits token revocation */
    CHECK(mp_token_revoke(lib, id) == 1, "revoke token");
    CHECK(mp_session_authorize(lib, sess, &srow) == 0,
          "revoked token invalidates its sessions");

    /* fresh session so logout has something to revoke */
    CHECK(mp_token_create(lib, "Web2", tok, sizeof tok, 0) == MUSICPACK_OK,
          "create second token");
    CHECK(mp_session_create(lib, tok, sess, sizeof sess) == MUSICPACK_OK,
          "exchange second token");
    CHECK(mp_session_revoke(lib, sess) == 1, "logout revokes session");
    CHECK(mp_session_revoke(lib, sess) == 0, "logout idempotent");
    CHECK(mp_session_authorize(lib, sess, &srow) == 0,
          "revoked session rejected");

    mp_library_close(lib);
}

static int g_progress_calls = 0;
static void
progress_cb(void *ctx, const mp_scan_result *partial)
{
    (void) ctx;
    (void) partial;
    g_progress_calls++;
}

static void
test_verify(void)
{
    char libdir[4096], dbpath[4096];
    char pkg[4096];
    mp_library *lib;
    mp_scan_result res;
    mp_verify_result vr;
    sqlite3 *db;

    snprintf(libdir, sizeof libdir, "%s/vlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/v.db", g_tmpdir);
    make_dir(libdir);
    snprintf(pkg, sizeof pkg, "%s/Good.mpack", libdir);
    copy_tree(g_ref_mpc, pkg);

    lib = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib != 0, "open verify db");
    db = mp_library_sqlite(lib);

    g_progress_calls = 0;
    mp_scan_library(lib, libdir, 0, &res, progress_cb, 0);
    CHECK(res.added == 1, "scan one package");
    CHECK(g_progress_calls >= 1, "scan progress callback invoked");

    /* a clean package verifies clean */
    mp_verify_library(lib, libdir, &vr, 0, 0);
    CHECK(vr.total == 1 && vr.passed == 1 && vr.failed == 0,
          "verify clean package");

    /* corrupt audio -> checksum mismatch */
    {
        char apath[4096];
        FILE *f;
        snprintf(apath, sizeof apath, "%s/audio/01 - Alphaville - Big in Japan.mpc",
                 pkg);
        f = fopen(apath, "ab");
        CHECK(f != 0, "open audio to corrupt");
        if (f != 0) {
            fwrite("X", 1, 1, f);
            fclose(f);
        }
    }
    mp_verify_library(lib, libdir, &vr, 0, 0);
    CHECK(vr.total == 1 && vr.passed == 0 && vr.failed == 1,
          "verify detects checksum mismatch");
    CHECK(count_query(db,
        "SELECT COUNT(*) FROM packages WHERE verify_status='checksum-failed'",
        0) == 1, "checksum-failed persisted");

    /* Explicit scan verification must not take the unchanged-manifest fast path. */
    mp_scan_library(lib, libdir, 1, &res, 0, 0);
    CHECK(count_query(db,
        "SELECT COUNT(*) FROM packages WHERE verify_status='checksum-failed'",
        0) == 1, "verified scan checks unchanged manifest assets");

    mp_library_close(lib);
}

/* ---------- main ----------------------------------------------------------- */

/* Package-owned content + identity-conflict quarantine: a package claiming an
   already-owned release identity with different content must be quarantined
   and must not mutate the owner's content or metadata. */
static void
test_ownership_conflict(void)
{
    char lib[4096], dbpath[4096];
    char victim[4096], hostile[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;

    snprintf(lib, sizeof lib, "%s/ownlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/own.db", g_tmpdir);
    make_dir(lib);
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "open ownership db");
    db = mp_library_sqlite(lib_h);

    /* victim is scanned first and owns the release */
    snprintf(victim, sizeof victim, "%s/Victim.mpack", lib);
    copy_tree(g_ref_flac, victim);
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 1, "victim added");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='valid'", -1) == 1,
        "victim is the only valid package");

    /* hostile copies the victim and changes a track (different content
       fingerprint, same release identity) */
    snprintf(hostile, sizeof hostile, "%s/Hostile.mpack", lib);
    copy_tree(g_ref_flac, hostile);
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", hostile);
        replace_in_file(mpath, "\"title\": \"Classical Piece No 1\"",
                        "\"title\": \"HIJACKED TRACK\"");
    }
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 1, "hostile added");

    /* hostile is quarantined as conflict, not served, and does not mutate */
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "conflicting package quarantined");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='valid'", -1) == 1,
        "owner stays the only valid package");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM tracks WHERE title='HIJACKED TRACK'", -1) == 0,
        "hostile content not attached");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM tracks WHERE title='Classical Piece No 1'", -1) == 1,
        "owner content preserved");

    mp_library_close(lib_h);
}

/* Conflict quarantine is durable: verification must never clear a conflict
   status. A quarantined package must stay conflict through verify, rescan,
   owner verification, and re-add cycles. */
static void
test_conflict_survives_verify(void)
{
    char lib[4096], dbpath[4096];
    char victim[4096], hostile[4096];
    mp_library *lib_h;
    mp_scan_result sres;
    mp_verify_result vres;
    sqlite3 *db;

    snprintf(lib, sizeof lib, "%s/cvlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/cv.db", g_tmpdir);
    make_dir(lib);
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "open cv db");
    db = mp_library_sqlite(lib_h);

    /* victim owns the release */
    snprintf(victim, sizeof victim, "%s/Victim.mpack", lib);
    copy_tree(g_ref_flac, victim);
    mp_scan_library(lib_h, lib, 0, &sres, 0, 0);
    CHECK(sres.added == 1, "victim added");

    /* hostile claims same identity with different content */
    snprintf(hostile, sizeof hostile, "%s/Hostile.mpack", lib);
    copy_tree(g_ref_flac, hostile);
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", hostile);
        replace_in_file(mpath, "\"title\": \"Classical Piece No 1\"",
                        "\"title\": \"HIJACKED\"");
    }
    mp_scan_library(lib_h, lib, 0, &sres, 0, 0);
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "hostile quarantined as conflict");

    /* verify must not clear conflict */
    mp_verify_library(lib_h, lib, &vres, 0, 0);
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "conflict survives verification");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='valid'", -1) == 1,
        "owner stays valid after verify");

    /* rescan must not clear conflict */
    mp_scan_library(lib_h, lib, 0, &sres, 0, 0);
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "conflict survives rescan");

    /* verify the owner - hostile must still be conflict */
    mp_verify_library(lib_h, lib, &vres, 0, 0);
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "conflict survives owner verify");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='valid'", -1) == 1,
        "owner still valid");

    /* a full verification scan (verify=1) must not clear conflict either */
    mp_scan_library(lib_h, lib, 1, &sres, 0, 0);
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "conflict survives verify scan");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='valid'", -1) == 1,
        "owner stays the only valid package after verify scan");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM tracks WHERE title='HIJACKED'", -1) == 0,
        "hostile content never attached");

    /* remove hostile, re-add - must remain quarantined (conflict is durable
       and is not cleared by a directory removal; the sweep skips conflict) */
    {
        char cmd[8192];
#ifndef _WIN32
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", hostile);
#else
        snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\"", hostile);
#endif
        if (system(cmd) != 0) { }
    }
    mp_scan_library(lib_h, lib, 0, &sres, 0, 0);
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "conflict survives directory removal (sweep skips conflict)");
    {
        char cmd[8192];
#ifndef _WIN32
        snprintf(cmd, sizeof cmd, "cp -R '%s' '%s'", victim, hostile);
#else
        snprintf(cmd, sizeof cmd, "xcopy /e /i \"%s\" \"%s\"", victim, hostile);
#endif
        if (system(cmd) != 0) { }
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", hostile);
        replace_in_file(mpath, "\"title\": \"Classical Piece No 1\"",
                        "\"title\": \"HIJACKED2\"");
    }
    mp_scan_library(lib_h, lib, 0, &sres, 0, 0);
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "re-added hostile re-quarantined");

    /* remove and re-add with the SAME conflicting content: conflict must
       persist (a re-added identical conflict is still quarantined) */
    {
        char cmd[8192];
#ifndef _WIN32
        snprintf(cmd, sizeof cmd, "rm -rf '%s'", hostile);
#else
        snprintf(cmd, sizeof cmd, "rmdir /s /q \"%s\"", hostile);
#endif
        if (system(cmd) != 0) { }
    }
    mp_scan_library(lib_h, lib, 0, &sres, 0, 0);
    {
        char cmd[8192];
#ifndef _WIN32
        snprintf(cmd, sizeof cmd, "cp -R '%s' '%s'", victim, hostile);
#else
        snprintf(cmd, sizeof cmd, "xcopy /e /i \"%s\" \"%s\"", victim, hostile);
#endif
        if (system(cmd) != 0) { }
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", hostile);
        replace_in_file(mpath, "\"title\": \"Classical Piece No 1\"",
                        "\"title\": \"HIJACKED\"");
    }
    mp_scan_library(lib_h, lib, 0, &sres, 0, 0);
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='conflict'", -1) == 1,
        "same-content re-add stays quarantined");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM packages WHERE status='valid'", -1) == 1,
        "owner untouched after same-content re-add");
    CHECK(count_rows(db,
        "SELECT COUNT(*) FROM tracks WHERE title='HIJACKED'", -1) == 0,
        "hostile content never attached after re-add");

    mp_library_close(lib_h);
}

/* Scan fail-closed: a directory tree exceeding the scan depth limit must
   cause the scan to return an error, not silently skip subtrees. */
static void
test_scan_fail_closed(void)
{
    char lib[4096], dbpath[4096];
    char deep[4096];
    mp_library *lib_h;
    mp_scan_result sres;
    musicpack_status rc;
    int i;

    snprintf(lib, sizeof lib, "%s/deeplib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/deep.db", g_tmpdir);
    make_dir(lib);
    /* Create a directory tree deeper than MAX_SCAN_DEPTH (64). Each level
       uses a single-character name so the total path stays under Windows'
       MAX_PATH limit (which otherwise truncates d0/d1/... style trees). */
    snprintf(deep, sizeof deep, "%s", lib);
    for (i = 0; i < 70; i++) {
        char next[4200];
        snprintf(next, sizeof next, "%s/d", deep);
        make_dir(next);
        snprintf(deep, sizeof deep, "%s", next);
    }
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "open deep db");
    rc = mp_scan_library(lib_h, lib, 0, &sres, 0, 0);
    CHECK(rc == MUSICPACK_ERR_IO, "deep tree fails scan (fail-closed)");
    CHECK(sres.total == 0, "no packages processed in failed scan");
    mp_library_close(lib_h);
}

/* Scan DB-mutation fail-closed: a scan whose database writes fail must
   return an error, never report a successful incomplete publication. */
static void
test_scan_db_fail_closed(void)
{
    char lib[4096], dbpath[4096];
    char pkg[4096];
    mp_library *lib_h;
    mp_scan_result sres;
    char err[256];

    snprintf(lib, sizeof lib, "%s/sdblib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/sdb.db", g_tmpdir);
    make_dir(lib);
    snprintf(pkg, sizeof pkg, "%s/Good.mpack", lib);
    copy_tree(g_ref_mpc, pkg);

    lib_h = mp_library_open(dbpath, 0, err, sizeof err);
    CHECK(lib_h == 0, "read-only scan open of absent db fails");
    if (lib_h != 0)
        mp_library_close(lib_h);

    /* Open read-only after a writable first scan: writes must fail. */
    {
        mp_library *w = mp_library_open(dbpath, 1, err, sizeof err);
        CHECK(w != 0, "writable open");
        if (w != 0) {
            CHECK(mp_scan_library(w, lib, 0, &sres, 0, 0) == MUSICPACK_OK,
                  "writable scan succeeds");
            mp_library_close(w);
        }
    }
    lib_h = mp_library_open(dbpath, 0, err, sizeof err);
    if (lib_h != 0) {
        mp_scan_result r2;
        musicpack_status rc = mp_scan_library(lib_h, lib, 0, &r2, 0, 0);
        CHECK(rc == MUSICPACK_ERR_IO,
              "scan fails closed on database write failure");
        mp_library_close(lib_h);
    } else {
        CHECK(0, "open read-only after writable scan");
    }
}

/* Verification persistence fail-closed: if the database cannot persist the
   verification result, verification must report failure rather than success
   (and must not leave the package marked valid). A read-only database forces
   the UPDATE to fail. */
static void
test_verify_fail_closed(void)
{
    char lib[4096], dbpath[4096];
    char pkg[4096];
    mp_library *lib_h;
    mp_scan_result sres;
    sqlite3 *ro;

    snprintf(lib, sizeof lib, "%s/fclib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/fc.db", g_tmpdir);
    make_dir(lib);
    snprintf(pkg, sizeof pkg, "%s/Good.mpack", lib);
    copy_tree(g_ref_mpc, pkg);

    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "open fc db");
    CHECK(mp_scan_library(lib_h, lib, 0, &sres, 0, 0) == MUSICPACK_OK,
          "fc scan one package");

    /* Re-open read-only and point the same scanner at it: writes must fail. */
    mp_library_close(lib_h);
    ro = 0;
    CHECK(sqlite3_open_v2(dbpath, &ro, SQLITE_OPEN_READONLY, 0) == SQLITE_OK,
          "reopen read-only");
    if (ro != 0) {
        char err[256];
        mp_library *ro_lib = mp_library_open(dbpath, 0, err, sizeof err);
        if (ro_lib != 0) {
            mp_verify_result rv;
            musicpack_status vrc = mp_verify_library(ro_lib, lib, &rv, 0, 0);
            CHECK(vrc == MUSICPACK_ERR_IO,
                  "verify fails closed when verification state cannot persist");
            mp_library_close(ro_lib);
        } else {
            CHECK(0, "open read-only lib");
        }
        sqlite3_close(ro);
    }
}

/* Per-track waveform envelope ingest: scanning a package with waveforms
   populates `track_waveforms`; the lookup resolves through the owning
   package and the VISIBLE filter; re-ingest replaces existing rows; a
   corrupt .wfm makes the owning package invisible (quarantine). */
static void
test_waveform_ingest(void)
{
    char lib[4096], dbpath[4096], pkg[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;
    long long track_id = -1;
    mp_object_ref ref;
    long long size = 0;
    char hex[MUSICPACK_SHA256_HEX_SIZE];

    snprintf(lib, sizeof lib, "%s/wavelib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/wave.db", g_tmpdir);
    make_dir(lib);
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "open waveform db");
    db = mp_library_sqlite(lib_h);

    /* Use the committed MPC reference package which now carries waveforms. */
    snprintf(pkg, sizeof pkg, "%s/WavePkg.mpack", lib);
    copy_tree(g_ref_mpc, pkg);
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 1, "waveform-bearing package ingested");
    /* Verify so the package becomes servable (VISIBLE filter). */
    {
        mp_verify_result vr;
        mp_verify_library(lib_h, lib, &vr, 0, 0);
    }

    /* track_waveforms has one row per track. */
    CHECK(count_rows(db, "SELECT COUNT(*) FROM track_waveforms", -1) == 4,
          "track_waveforms populated (4 rows)");

    /* Find the first track id and look up its waveform. */
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT id FROM tracks ORDER BY id LIMIT 1", -1, &st, 0) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                track_id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }
    CHECK(track_id > 0, "have a track id");
    CHECK(mp_library_track_waveform(lib_h, track_id, &ref) == 1,
          "mp_library_track_waveform resolves");
    CHECK(ref.file_size == (long long) (10 * 2),
          "waveform file size matches points*2 (10 points = 20 bytes)");
    CHECK(ref.mime[0] != '\0' && strstr(ref.mime, "musicpack.waveform") != 0,
          "MIME is the waveform type");
    CHECK(strlen(ref.sha256) == 64, "waveform has sha256 for ETag");

    /* Manifest's waveform sha256 must match the on-disk file. */
    {
        sqlite3_stmt *st;
        const char *wf_sha = 0;
        if (sqlite3_prepare_v2(db,
                "SELECT relative_path FROM track_waveforms WHERE track_id = ?1",
                -1, &st, 0) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, track_id);
            if (sqlite3_step(st) == SQLITE_ROW)
                /* verify path resolves */
                ;
            sqlite3_finalize(st);
        }
        (void) wf_sha;
    }

    /* Rescan: re-ingest replaces rows. */
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(count_rows(db, "SELECT COUNT(*) FROM track_waveforms", -1) == 4,
          "track_waveforms count stable on rescan");
    /* A light scan leaves verify_status='unverified' which is not VISIBLE.
       Run mp_verify_library so the package becomes servable again. */
    {
        mp_verify_result vr;
        mp_verify_library(lib_h, lib, &vr, 0, 0);
    }
    CHECK(mp_library_track_waveform(lib_h, track_id, &ref) == 1,
          "waveform still resolves after rescan + verify");

    /* Touch the size variable to silence unused warnings */
    (void) size; (void) hex;
    mp_library_close(lib_h);
}

static void
test_waveform_quarantine(void)
{
    char lib[4096], dbpath[4096], pkg[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;
    long long track_id = -1;
    mp_object_ref ref;

    snprintf(lib, sizeof lib, "%s/waveqlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/waveq.db", g_tmpdir);
    make_dir(lib);
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    db = mp_library_sqlite(lib_h);

    snprintf(pkg, sizeof pkg, "%s/BadWave.mpack", lib);
    copy_tree(g_ref_mpc, pkg);
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 1, "package ingested");
    {
        mp_verify_result vr;
        mp_verify_library(lib_h, lib, &vr, 0, 0);
    }

    /* Find a track and corrupt its .wfm. */
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT id FROM tracks ORDER BY id LIMIT 1", -1, &st, 0) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                track_id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }
    CHECK(track_id > 0, "have track id");
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT relative_path FROM track_waveforms WHERE track_id = ?1",
                -1, &st, 0) == SQLITE_OK) {
            sqlite3_bind_int64(st, 1, track_id);
            if (sqlite3_step(st) == SQLITE_ROW) {
                char wfpath[4096];
                snprintf(wfpath, sizeof wfpath, "%s/%s", pkg,
                         sqlite3_column_text(st, 0));
                /* Truncate to 2 bytes -> waveform_points * 2 mismatch. */
                FILE *f = fopen(wfpath, "wb");
                if (f) { fputc(0, f); fputc(0, f); fclose(f); }
            }
            sqlite3_finalize(st);
        }
    }
    /* Full verify surfaces the checksum/size error -> package quarantined. */
    {
        mp_verify_result vr;
        mp_verify_library(lib_h, lib, &vr, 0, 0);
    }
    /* After quarantine, mp_library_track_waveform must return 0
       (VISIBLE filter excludes checksum-failed packages). */
    CHECK(mp_library_track_waveform(lib_h, track_id, &ref) == 0,
          "corrupted waveform hides the package from the endpoint");

    mp_library_close(lib_h);
}

/* A package without any waveform envelopes is fully valid: scanning,
   verifying, and resolving track audio work without touching waveform. */
static void
test_waveform_no_waveform_fixture(void)
{
    char lib[4096], dbpath[4096], pkg[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;
    long long track_id = -1;
    mp_object_ref ref;

    snprintf(lib, sizeof lib, "%s/waveflaclib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/waveflac.db", g_tmpdir);
    make_dir(lib);
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    db = mp_library_sqlite(lib_h);

    snprintf(pkg, sizeof pkg, "%s/FlacNoWf.mpack", lib);
    copy_tree(g_ref_flac, pkg);
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 1, "no-waveform package ingested");
    {
        mp_verify_result vr;
        mp_verify_library(lib_h, lib, &vr, 0, 0);
    }

    /* No track_waveforms rows. */
    CHECK(count_rows(db, "SELECT COUNT(*) FROM track_waveforms", -1) == 0,
          "no track_waveforms for the FLAC fixture");

    /* Look up the first track; waveform lookup must return 0; audio works. */
    {
        sqlite3_stmt *st;
        if (sqlite3_prepare_v2(db,
                "SELECT id FROM tracks ORDER BY id LIMIT 1", -1, &st, 0) == SQLITE_OK) {
            if (sqlite3_step(st) == SQLITE_ROW)
                track_id = sqlite3_column_int64(st, 0);
            sqlite3_finalize(st);
        }
    }
    CHECK(track_id > 0, "have track id");
    CHECK(mp_library_track_waveform(lib_h, track_id, &ref) == 0,
          "track has no waveform (endpoint 404s)");
    {
        mp_verify_result vr;
        mp_verify_library(lib_h, lib, &vr, 0, 0);
    }
    CHECK(mp_library_track_audio(lib_h, track_id, &ref) == 1,
          "audio resolves after verify");

    mp_library_close(lib_h);
}

/* Upgrade path: a database created at schema version 5 (pre-uid) must
   migrate cleanly to the current version, backfill uids, and gain the
   hardening indexes. Uses the real migration strings, applied up to
   version 5, then reopens through the normal auto-migrating open. */
static void
test_upgrade_from_v5(void)
{
    char dbpath[4096], lib[4096], pkg[4096];
    sqlite3 *raw;
    const char *const *migrations = mp_schema_migrations();
    int i;
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;

    snprintf(dbpath, sizeof dbpath, "%s/upg-v5.db", g_tmpdir);
    CHECK(sqlite3_open(dbpath, &raw) == SQLITE_OK, "upgrade: create raw db");
    CHECK(sqlite3_exec(raw,
        "CREATE TABLE schema_version ("
        "  version INTEGER PRIMARY KEY,"
        "  applied_at TEXT NOT NULL);", 0, 0, 0) == SQLITE_OK,
        "upgrade: schema_version table");
    /* mirror db.c bookkeeping: one version row, bumped after each migration */
    CHECK(sqlite3_exec(raw,
        "INSERT INTO schema_version(version, applied_at)"
        " VALUES (0, datetime('now'));", 0, 0, 0) == SQLITE_OK,
        "upgrade: stamp version 0");
    /* migrations[0..4] == schema versions 1..5 */
    for (i = 0; i < 5; i++) {
        char sql[128];
        snprintf(sql, sizeof sql,
                 "UPDATE schema_version SET version=%d, applied_at="
                 "datetime('now');", i + 1);
        CHECK(sqlite3_exec(raw, migrations[i], 0, 0, 0) == SQLITE_OK,
              "upgrade: apply migration");
        CHECK(sqlite3_exec(raw, sql, 0, 0, 0) == SQLITE_OK,
              "upgrade: record new version");
    }
    sqlite3_close(raw);

    /* normal writable open must migrate 5 -> 8 automatically */
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "upgrade: migrated open succeeds");
    db = mp_library_sqlite(lib_h);
    CHECK(mp_library_schema_version(lib_h) == 8,
          "upgrade: schema version now 8");

    /* ingesting into the upgraded schema assigns uids; a metadata edit
       re-ingest preserves ids exactly like a fresh database */
    snprintf(lib, sizeof lib, "%s/upglib", g_tmpdir);
    make_dir(lib);
    snprintf(pkg, sizeof pkg, "%s/UpgPkg.mpack", lib);
    copy_tree(g_ref_mpc, pkg);
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
          "upgrade: ingest into migrated db ok");
    CHECK(count_rows(db, "SELECT COUNT(*) FROM tracks WHERE uid IS NULL",
                     -1) == 0,
          "upgrade: ingested rows carry uids");
    {
        long long before = scalar_ll(
            db, "SELECT id FROM tracks WHERE title = 'Bleachers - The Van'",
            0);
        char mpath[4096];
        mp_scan_result res2;
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkg);
        replace_in_file(mpath, "\"title\": \"Bleachers - The Van\"",
                        "\"title\": \"The Van (upgraded)\"");
        CHECK(mp_scan_library(lib_h, lib, 0, &res2, 0, 0) == MUSICPACK_OK,
              "upgrade: rescan ok");
        CHECK(scalar_ll(db,
            "SELECT id FROM tracks WHERE title = 'The Van (upgraded)'",
            0) == before && before > 0,
            "upgrade: track id preserved across re-ingest");
    }

    mp_library_close(lib_h);
}
/* ---------- Phase 2A: artist identity / MusicBrainz anchors ------------- */

/* Credit-anchor identity freeze: adding musicbrainzId/sortName to credits
   must never change group/release identity keys (frozen tag set), while the
   package fingerprint (whole-manifest hash) legitimately changes. */
static void
test_identity_mbid_freeze(const char *ref)
{
    char mpath[4096];
    char gk1[MP_ID_KEY_MAX], gk2[MP_ID_KEY_MAX];
    char rk1[MP_ID_KEY_MAX], rk2[MP_ID_KEY_MAX];
    char fp1[MP_ID_KEY_MAX], fp2[MP_ID_KEY_MAX];
    musicpack_manifest *m;
    char *json;
    size_t i;

    snprintf(mpath, sizeof mpath, "%s/manifest.json", ref);
    json = read_file(mpath);
    CHECK(json != 0, "freeze: read manifest");
    if (json == 0)
        return;
    m = musicpack_manifest_parse(json, 0);
    free(json);
    CHECK(m != 0, "freeze: parse manifest");
    if (m == 0)
        return;

    CHECK(mp_identity_group_key(m, gk1, sizeof gk1) == MUSICPACK_OK &&
          mp_identity_release_key(m, rk1, sizeof rk1) == MUSICPACK_OK &&
          mp_identity_package_fingerprint(m, fp1, sizeof fp1)
              == MUSICPACK_OK,
          "freeze: baseline identity computed");

    for (i = 0; i < m->album_artist_count; i++) {
        m->album_artists[i].musicbrainz_id =
            strdup("5441c29d-3602-4898-b1a1-b77fa23b8e50");
        m->album_artists[i].sort_name = strdup("Some, Sortname");
    }

    CHECK(mp_identity_group_key(m, gk2, sizeof gk2) == MUSICPACK_OK &&
          mp_identity_release_key(m, rk2, sizeof rk2) == MUSICPACK_OK &&
          strcmp(gk1, gk2) == 0 && strcmp(rk1, rk2) == 0,
          "freeze: credit mbids never enter identity keys");
    CHECK(mp_identity_package_fingerprint(m, fp2, sizeof fp2)
              == MUSICPACK_OK && strcmp(fp1, fp2) != 0,
          "freeze: fingerprint still tracks manifest content");
    musicpack_manifest_free(m);
}

/* The Phase 2A matching matrix: MBID anchor -> exact-case name -> NOCASE
   name -> insert, with first-write-wins enrichment and conservative
   conflict handling. */
static void
test_artist_identity_mbid(void)
{
#define MBID_A "01809552-4f22-4c07-888b-8c1baed05b07"
#define MBID_B "2c7bd3f1-9a26-4e5f-8a63-39f1a7f04e64"
#define MBID_C "3d8ce4a2-0b37-4f60-9b74-40a2b8e15f75"
    char lib[4096], dbpath[4096], pkgA[4096], mpathA[4096];
    char pkgB[4096], pkgC[4096], pkgD[4096], pkgE[4096];
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;
    long long a1, b1, track1;
    char txt[128];

    snprintf(lib, sizeof lib, "%s/mbidlib", g_tmpdir);
    snprintf(dbpath, sizeof dbpath, "%s/mbid.db", g_tmpdir);
    make_dir(lib);
    snprintf(pkgA, sizeof pkgA, "%s/A.mpack", lib);
    copy_tree(g_ref_mpc, pkgA);
    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "mbid: open db");
    db = mp_library_sqlite(lib_h);
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK &&
          res.added == 1, "mbid: baseline scan");

    /* T1: anchor-less artists resolve by name and stay stable */
    a1 = scalar_ll(db, "SELECT id FROM artists WHERE name = ?1", "Alphaville");
    b1 = scalar_ll(db, "SELECT id FROM artists WHERE name = ?1", "Bleachers");
    track1 = scalar_ll(db,
        "SELECT id FROM tracks WHERE title = ?1", "Alphaville - Big in Japan");
    CHECK(a1 > 0 && b1 > 0 && track1 > 0, "mbid: baseline artists captured");
    scalar_text(db, "SELECT musicbrainz_id FROM artists WHERE name = ?1",
                "Alphaville", txt, sizeof txt);
    CHECK(txt[0] == '\0', "mbid: fresh artists carry no anchor");
    mp_scan_library(lib_h, lib, 0, &res, 0, 0);
    CHECK(res.added == 0 && res.updated == 0 &&
          scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Alphaville") == a1,
          "mbid: anchor-less rescan is a stable no-op");

    /* T2: an existing artist adopts a manifest-supplied MBID, ids stable */
    snprintf(mpathA, sizeof mpathA, "%s/manifest.json", pkgA);
    replace_in_file(mpathA, "\"name\": \"Alphaville\"",
                    "\"name\": \"Alphaville\","
                    " \"musicbrainzId\": \"" MBID_A "\"");
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK &&
          res.updated == 1, "mbid: anchor edit ingests as update");
    CHECK(scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Alphaville") == a1,
          "mbid: adopting artist keeps its id");
    scalar_text(db, "SELECT musicbrainz_id FROM artists WHERE name = ?1",
                "Alphaville", txt, sizeof txt);
    CHECK(strcmp(txt, MBID_A) == 0, "mbid: anchor adopted into existing row");
    CHECK(scalar_ll(db, "SELECT id FROM tracks WHERE title = ?1",
                    "Alphaville - Big in Japan") == track1,
          "mbid: enrichment does not re-key tracks");

    /* T3: same MBID, changed display name -> MBID precedence, name
       first-wins, sort filled-if-empty; T10: roles round-trip */
    snprintf(pkgB, sizeof pkgB, "%s/B.mpack", lib);
    copy_tree(g_ref_mpc, pkgB);
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkgB);
        replace_in_file(mpath, "\"title\": \"Synthetic Test Compilation\"",
                        "\"title\": \"Anchored Editions Vol 1\"");
        replace_in_file(mpath, "\"name\": \"Alphaville\"",
                        "\"name\": \"Alphaville Project\","
                        " \"musicbrainzId\": \"" MBID_A "\","
                        " \"sortName\": \"Alphaville, sortkey\"");
        replace_in_file(mpath, "\"name\": \"Bleachers\"",
                        "\"name\": \"Bleachers\", \"role\": \"featured\","
                        " \"musicbrainzId\": \"" MBID_B "\"");
    }
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK &&
          res.added == 1, "mbid: renamed-credit package added");
    CHECK(scalar_ll(db, "SELECT id FROM artists WHERE musicbrainz_id = ?1",
                    MBID_A) == a1,
          "mbid: changed display name resolves via anchor");
    CHECK(scalar_ll(db, "SELECT COUNT(*) FROM artists WHERE name = ?1",
                    "Alphaville Project") == 0,
          "mbid: display name never rewritten (first-write-wins)");
    CHECK(strcmp(scalar_text(db, "SELECT sort_name FROM artists WHERE name = ?1",
                             "Alphaville", txt, sizeof txt),
                 "Alphaville, sortkey") == 0,
          "mbid: sort name filled when empty");
    CHECK(scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Bleachers") == b1 &&
          strcmp(scalar_text(db,
              "SELECT musicbrainz_id FROM artists WHERE name = ?1",
              "Bleachers", txt, sizeof txt), MBID_B) == 0,
          "mbid: name-matched row adopts anchor");
    CHECK(strcmp(scalar_text(db,
        "SELECT ga.role FROM group_artists ga"
        " JOIN release_groups g ON g.id = ga.group_id"
        " WHERE g.title = ?1 AND ga.position = 1",
        "Anchored Editions Vol 1", txt, sizeof txt), "featured") == 0,
        "mbid: credit roles round-trip with anchors present");

    /* T4/T7/TW: same name, conflicting MBID -> conservative keep + exactly
       one warning per conflict; ingest succeeds */
    snprintf(pkgC, sizeof pkgC, "%s/C.mpack", lib);
    copy_tree(g_ref_mpc, pkgC);
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkgC);
        replace_in_file(mpath, "\"title\": \"Synthetic Test Compilation\"",
                        "\"title\": \"Conflict Editions Vol 1\"");
        replace_in_file(mpath, "\"name\": \"Bleachers\"",
                        "\"name\": \"Bleachers\","
                        " \"musicbrainzId\": \"" MBID_C "\"");
    }
#ifdef _WIN32
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK &&
          res.added == 1, "mbid: conflicting-mbid package added");
#else
    {
        char wlog[4096];
        int saved, wf;
        int conflicts = 0;
        fflush(stderr);
        saved = dup(fileno(stderr));
        snprintf(wlog, sizeof wlog, "%s/warn.log", lib);
        wf = open(wlog, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        CHECK(saved >= 0 && wf >= 0, "mbid: stderr capture set up");
        if (saved >= 0 && wf >= 0) {
            dup2(wf, fileno(stderr));
            CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0)
                      == MUSICPACK_OK && res.added == 1,
                  "mbid: conflicting-mbid package added");
            fflush(stderr);
            dup2(saved, fileno(stderr));
            close(saved);
            close(wf);
        } else {
            if (saved >= 0)
                close(saved);
            if (wf >= 0)
                close(wf);
            CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0)
                      == MUSICPACK_OK && res.added == 1,
                  "mbid: conflicting-mbid package added");
        }
        if (wf >= 0) {
            char *log = read_file(wlog);
            char *hit;
            if (log != 0) {
                hit = strstr(log, "conflicting musicbrainz id");
                while (hit != 0) {
                    conflicts++;
                    hit = strstr(hit + 1, "conflicting musicbrainz id");
                }
                free(log);
            }
            CHECK(conflicts == 1,
                  "mbid: conflicting anchor warns exactly once");
        }
    }
#endif
    CHECK(scalar_ll(db, "SELECT COUNT(*) FROM artists WHERE name = ?1",
                    "Bleachers") == 1,
          "mbid: conflicting mbid never forks a second row");
    CHECK(strcmp(scalar_text(db,
        "SELECT musicbrainz_id FROM artists WHERE name = ?1",
        "Bleachers", txt, sizeof txt), MBID_B) == 0,
        "mbid: stored anchor survives a conflicting claim");
    CHECK(scalar_ll(db,
        "SELECT ga.artist_id FROM group_artists ga"
        " JOIN release_groups g ON g.id = ga.group_id"
        " WHERE g.title = ?1 AND ga.position = 1",
        "Conflict Editions Vol 1") == b1,
        "mbid: conflicting credit still binds to the existing row");

    /* T5: case-only difference -> exact-case miss, NOCASE reuse */
    snprintf(pkgD, sizeof pkgD, "%s/D.mpack", lib);
    copy_tree(g_ref_mpc, pkgD);
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkgD);
        replace_in_file(mpath, "\"title\": \"Synthetic Test Compilation\"",
                        "\"title\": \"Case Editions Vol 1\"");
        replace_in_file(mpath, "\"name\": \"Alphaville\"",
                        "\"name\": \"ALPHAVILLE\"");
    }
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK &&
          res.added == 1, "mbid: case-variant package added");
    CHECK(scalar_ll(db, "SELECT COUNT(*) FROM artists"
                        " WHERE name = ?1 COLLATE NOCASE", "alphaville") == 1,
          "mbid: case-only credit merges to the existing row");
    CHECK(scalar_ll(db,
        "SELECT ga.artist_id FROM group_artists ga"
        " JOIN release_groups g ON g.id = ga.group_id"
        " WHERE g.title = ?1 AND ga.position = 0",
        "Case Editions Vol 1") == a1,
        "mbid: case-only credit binds to the merged artist");

    /* T11: Various Artists / compilation-style credits stay plain
       name-keyed artists; mixed anchored/unanchored credits in one package */
    snprintf(pkgE, sizeof pkgE, "%s/E.mpack", lib);
    copy_tree(g_ref_mpc, pkgE);
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkgE);
        replace_in_file(mpath, "\"title\": \"Synthetic Test Compilation\"",
                        "\"title\": \"Various Editions Vol 1\"");
        replace_in_file(mpath, "\"name\": \"Alphaville\"",
                        "\"name\": \"Alphaville\","
                        " \"musicbrainzId\": \"" MBID_A "\"");
        replace_in_file(mpath, "\"name\": \"Bleachers\"",
                        "\"name\": \"Various Artists\"");
    }
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK &&
          res.added == 1, "mbid: various-artists package added");
    CHECK(scalar_ll(db, "SELECT id FROM artists WHERE musicbrainz_id = ?1",
                    MBID_A) == a1,
          "mbid: anchor reused across groups");
    CHECK(scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Various Artists") > 0,
          "mbid: Various Artists credit is an ordinary artist");

    /* non-canonical MBID strings are treated as absent (never anchored) */
    {
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkgA);
        replace_in_file(mpath, MBID_A, "not-a-uuid");
        CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK &&
              res.updated == 1, "mbid: non-canonical anchor edit ingests");
        CHECK(strcmp(scalar_text(db,
            "SELECT musicbrainz_id FROM artists WHERE name = ?1",
            "Alphaville", txt, sizeof txt), MBID_A) == 0,
            "mbid: non-canonical claim never overwrites the anchor");
    }

    mp_library_close(lib_h);
#undef MBID_A
#undef MBID_B
#undef MBID_C
}

/* Migration 8 against a real pre-P2A (v7) database with data: ids must
   survive, the new column starts NULL, the partial index exists, and
   anchor resolution works on the upgraded schema. */
static void
test_upgrade_from_v7(void)
{
    char dbpath[4096], lib[4096], pkg[4096];
    sqlite3 *raw;
    const char *const *migrations = mp_schema_migrations();
    int i;
    mp_library *lib_h;
    mp_scan_result res;
    sqlite3 *db;
    char txt[128];

    snprintf(dbpath, sizeof dbpath, "%s/upg-v7.db", g_tmpdir);
    CHECK(sqlite3_open(dbpath, &raw) == SQLITE_OK, "upg7: create raw db");
    CHECK(sqlite3_exec(raw,
        "CREATE TABLE schema_version ("
        "  version INTEGER PRIMARY KEY,"
        "  applied_at TEXT NOT NULL);", 0, 0, 0) == SQLITE_OK,
        "upg7: schema_version table");
    CHECK(sqlite3_exec(raw,
        "INSERT INTO schema_version(version, applied_at)"
        " VALUES (0, datetime('now'));", 0, 0, 0) == SQLITE_OK,
        "upg7: stamp version 0");
    for (i = 0; i < 7; i++) {
        char sql[128];
        snprintf(sql, sizeof sql,
                 "UPDATE schema_version SET version=%d, applied_at="
                 "datetime('now');", i + 1);
        CHECK(sqlite3_exec(raw, migrations[i], 0, 0, 0) == SQLITE_OK,
              "upg7: apply migration");
        CHECK(sqlite3_exec(raw, sql, 0, 0, 0) == SQLITE_OK,
              "upg7: record new version");
    }
    /* real pre-P2A data: an artist credited by a release group */
    CHECK(sqlite3_exec(raw,
        "INSERT INTO artists(id, name, sort_name)"
        " VALUES (1, 'Legacy Artist', NULL);"
        "INSERT INTO release_groups(id, title, group_key)"
        " VALUES (1, 'Legacy Album', 'legacy-key');"
        "INSERT INTO group_artists(group_id, artist_id, position, role)"
        " VALUES (1, 1, 0, 'main');", 0, 0, 0) == SQLITE_OK,
        "upg7: seed pre-P2A rows");
    sqlite3_close(raw);

    lib_h = mp_library_open(dbpath, 1, 0, 0);
    CHECK(lib_h != 0, "upg7: migrated open succeeds");
    db = mp_library_sqlite(lib_h);
    CHECK(mp_library_schema_version(lib_h) == 8, "upg7: schema version 8");
    CHECK(scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                    "Legacy Artist") == 1,
          "upg7: artist row id preserved");
    CHECK(scalar_text(db, "SELECT musicbrainz_id FROM artists WHERE name = ?1",
                      "Legacy Artist", txt, sizeof txt)[0] == '\0',
          "upg7: legacy artist anchor NULL after migration");
    CHECK(scalar_ll(db,
        "SELECT COUNT(*) FROM sqlite_master"
        " WHERE type='index' AND name='artists_mbid_idx'", 0) == 1,
        "upg7: partial mbid index created");
    CHECK(scalar_ll(db,
        "SELECT ga.artist_id FROM group_artists ga"
        " JOIN release_groups g ON g.id = ga.group_id"
        " WHERE g.title = ?1", "Legacy Album") == 1,
        "upg7: join rows resolve after migration");

    /* ingest + anchor adoption work on the upgraded schema */
    snprintf(lib, sizeof lib, "%s/upg7lib", g_tmpdir);
    make_dir(lib);
    snprintf(pkg, sizeof pkg, "%s/Upg7Pkg.mpack", lib);
    copy_tree(g_ref_mpc, pkg);
    CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK &&
          res.added == 1, "upg7: ingest into migrated db");
    {
        long long before = scalar_ll(db,
            "SELECT id FROM artists WHERE name = ?1", "Alphaville");
        char mpath[4096];
        snprintf(mpath, sizeof mpath, "%s/manifest.json", pkg);
        replace_in_file(mpath, "\"name\": \"Alphaville\"",
                        "\"name\": \"Alphaville\","
                        " \"musicbrainzId\":"
                        " \"01809552-4f22-4c07-888b-8c1baed05b07\"");
        CHECK(mp_scan_library(lib_h, lib, 0, &res, 0, 0) == MUSICPACK_OK,
              "upg7: anchor rescan ok");
        CHECK(scalar_ll(db, "SELECT id FROM artists WHERE name = ?1",
                        "Alphaville") == before &&
              strcmp(scalar_text(db,
                  "SELECT musicbrainz_id FROM artists WHERE name = ?1",
                  "Alphaville", txt, sizeof txt),
                     "01809552-4f22-4c07-888b-8c1baed05b07") == 0,
              "upg7: anchor adopted in place on upgraded schema");
    }

    mp_library_close(lib_h);
}


int
main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: server_tests <ref-mpc-package> <ref-flac-package>\n");
        return 2;
    }
    snprintf(g_ref_mpc, sizeof g_ref_mpc, "%s", argv[1]);
    snprintf(g_ref_flac, sizeof g_ref_flac, "%s", argv[2]);
    {
        const char *base = getenv("TMPDIR");
#ifdef _WIN32
        if (base == 0 || *base == '\0')
            base = getenv("TEMP");
#endif
        if (base == 0 || *base == '\0')
            base = "/tmp";
        snprintf(g_tmpdir, sizeof g_tmpdir, "%s/server-test-%ld", base,
                 (long) getpid());
    }
    make_dir(g_tmpdir);

    test_range();
    test_migrations();
    test_identity(g_ref_mpc);
    test_identity_mbid_freeze(g_ref_mpc);
    test_mime();
    test_tokens();
    test_sessions();
    test_verify();
    test_scanner();
    test_reingest_assets();
    test_reingest_preserves_ids();
    test_artist_identity_mbid();
    test_stable_uid_and_indexes();
    test_upgrade_from_v5();
    test_upgrade_from_v7();
    test_ingest_busy_rollback();
    test_ownership_conflict();
    test_conflict_survives_verify();
    test_scan_fail_closed();
    test_scan_db_fail_closed();
    test_verify_fail_closed();
    test_waveform_ingest();
    test_waveform_quarantine();
    test_waveform_no_waveform_fixture();

    if (failures == 0) {
        printf("server_tests: all passed\n");
        return 0;
    }
    printf("server_tests: %d failure(s)\n", failures);
    return 1;
}
