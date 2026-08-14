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
    CHECK(db != 0 && mp_db_schema_version(db) == 4, "schema version 4");
    mp_db_close(db);
    CHECK(mp_db_open(&db, dbpath, 1, err, sizeof err) == 0, "reopen db");
    CHECK(mp_db_schema_version(db) == 4, "version stable on reopen");
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
   ingest_valid -> mp_library_replace_release_content, which must delete the
   previous assets rows before re-inserting, so stale artwork/booklet/lyrics
   rows never remain servable and rows never duplicate. */
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
    test_mime();
    test_tokens();
    test_sessions();
    test_verify();
    test_scanner();
    test_reingest_assets();
    test_ingest_busy_rollback();
    test_ownership_conflict();
    test_conflict_survives_verify();
    test_scan_fail_closed();
    test_scan_db_fail_closed();
    test_verify_fail_closed();

    if (failures == 0) {
        printf("server_tests: all passed\n");
        return 0;
    }
    printf("server_tests: %d failure(s)\n", failures);
    return 1;
}
