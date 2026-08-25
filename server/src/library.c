/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  SPDX-License-Identifier: BSD-3-Clause
  (BSD 3-clause, see library.h)
*/
#include "library.h"
#include "codec.h"
#include "log.h"
#include "mime.h"
#include "random.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/stat.h>
#include <sqlite3.h>

struct mp_library {
    mp_db *db;
};

/* ---- lifecycle --------------------------------------------------------- */

mp_library *
mp_library_open(const char *path, int writable, char *err, size_t errcap)
{
    mp_library *lib = (mp_library *) calloc(1, sizeof *lib);
    if (lib == 0)
        return 0;
    if (mp_db_open(&lib->db, path, writable, err, errcap) != 0) {
        free(lib);
        return 0;
    }
    return lib;
}

void
mp_library_close(mp_library *lib)
{
    if (lib == 0)
        return;
    mp_db_close(lib->db);
    free(lib);
}

sqlite3 *
mp_library_sqlite(mp_library *lib)
{
    return lib != 0 ? mp_db_sqlite(lib->db) : 0;
}

const char *
mp_library_sqlite_err(mp_library *lib)
{
    sqlite3 *db = mp_library_sqlite(lib);
    if (db == 0)
        return "library is not open";
    const char *msg = sqlite3_errmsg(db);
    return msg != 0 ? msg : "unknown sqlite error";
}

int
mp_library_schema_version(mp_library *lib)
{
    return lib != 0 ? mp_db_schema_version(lib->db) : 0;
}

int
mp_library_begin(mp_library *lib)
{
    return sqlite3_exec(mp_db_sqlite(lib->db), "BEGIN;", 0, 0, 0);
}

int
mp_library_commit(mp_library *lib)
{
    return sqlite3_exec(mp_db_sqlite(lib->db), "COMMIT;", 0, 0, 0);
}

void
mp_library_rollback(mp_library *lib)
{
    sqlite3_exec(mp_db_sqlite(lib->db), "ROLLBACK;", 0, 0, 0);
}

/* ---- prepared statement helper ---------------------------------------- */

typedef struct {
    sqlite3_stmt *st;
} stmt;

static sqlite3_stmt *
stmt_prepare(mp_library *lib, const char *sql)
{
    sqlite3_stmt *st = 0;
    if (sqlite3_prepare_v2(mp_db_sqlite(lib->db), sql, -1, &st, 0)
        != SQLITE_OK)
        return 0;
    return st;
}

/* ---- package rows ------------------------------------------------------ */

/* Copies a possibly-NULL text column into a buffer (empty when NULL). */
static void
col_cpy(char *dst, size_t cap, sqlite3_stmt *st, int idx)
{
    const unsigned char *t;
    if (cap == 0)
        return;
    dst[0] = '\0';
    t = sqlite3_column_text(st, idx);
    if (t != 0)
        snprintf(dst, cap, "%s", (const char *) t);
}

static void
fill_package_row(sqlite3_stmt *st, mp_package_row *row)
{
    memset(row, 0, sizeof *row);
    row->id = sqlite3_column_int64(st, 0);
    row->release_id = sqlite3_column_int64(st, 1);
    col_cpy(row->path, sizeof row->path, st, 2);
    col_cpy(row->fingerprint, sizeof row->fingerprint, st, 3);
    col_cpy(row->manifest_sha256, sizeof row->manifest_sha256, st, 4);
    col_cpy(row->status, sizeof row->status, st, 5);
    col_cpy(row->verify_status, sizeof row->verify_status, st, 6);
}

int
mp_library_package_by_path(mp_library *lib, const char *path,
                           mp_package_row *row)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT id, release_id, path, fingerprint, manifest_sha256,"
        "       status, verify_status FROM packages WHERE path = ?1");
    int rc = 0;
    if (st == 0)
        return 0;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        fill_package_row(st, row);
        rc = 1;
    }
    sqlite3_finalize(st);
    return rc;
}

int
mp_library_package_by_fingerprint(mp_library *lib, const char *fp,
                                  mp_package_row *row)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT id, release_id, path, fingerprint, manifest_sha256,"
        "       status, verify_status FROM packages WHERE fingerprint = ?1"
        " ORDER BY id LIMIT 1");
    int rc = 0;
    if (st == 0)
        return 0;
    sqlite3_bind_text(st, 1, fp, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        fill_package_row(st, row);
        rc = 1;
    }
    sqlite3_finalize(st);
    return rc;
}

int
mp_library_release_has_package(mp_library *lib, long long release_id)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT 1 FROM packages WHERE release_id = ?1 LIMIT 1");
    int rc = 0;
    if (st == 0)
        return 0;
    sqlite3_bind_int64(st, 1, release_id);
    rc = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return rc;
}

int
mp_library_package_fingerprint(mp_library *lib, long long id,
                               char *fp, size_t cap)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT fingerprint FROM packages WHERE id = ?1");
    int rc = 0;
    if (st == 0 || fp == 0 || cap == 0)
        return 0;
    fp[0] = '\0';
    sqlite3_bind_int64(st, 1, id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        const char *v = (const char *) sqlite3_column_text(st, 0);
        if (v != 0) {
            snprintf(fp, cap, "%s", v);
            rc = 1;
        }
    }
    sqlite3_finalize(st);
    return rc;
}

int
mp_library_package_owner_present(mp_library *lib, long long package_id)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT 1 FROM packages WHERE id = ?1"
        " AND status NOT IN ('unavailable','invalid','conflict')");
    int rc = 0;
    if (st == 0)
        return 0;
    sqlite3_bind_int64(st, 1, package_id);
    rc = sqlite3_step(st) == SQLITE_ROW;
    sqlite3_finalize(st);
    return rc;
}

long long
mp_library_package_insert(mp_library *lib, const char *path,
                          long long release_id, const char *fingerprint,
                          const char *manifest_sha256, const char *status,
                          const char *verify_status, const char *last_scan,
                          const char *last_error)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "INSERT INTO packages(path, release_id, fingerprint, manifest_sha256,"
        "  status, verify_status, last_scan, last_error)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
    if (st == 0)
        return -1;
    sqlite3_bind_text(st, 1, path, -1, SQLITE_TRANSIENT);
    if (release_id <= 0)
        sqlite3_bind_null(st, 2);
    else
        sqlite3_bind_int64(st, 2, release_id);
    sqlite3_bind_text(st, 3, fingerprint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, manifest_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, verify_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, last_scan, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, last_error, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(mp_db_sqlite(lib->db));
}

int
mp_library_package_update(mp_library *lib, long long id, long long release_id,
                          const char *path, const char *fingerprint,
                          const char *manifest_sha256, const char *status,
                          const char *verify_status, const char *last_scan,
                          const char *last_error)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "UPDATE packages SET release_id=?2, path=?3, fingerprint=?4,"
        "  manifest_sha256=?5, status=?6, verify_status=?7, last_scan=?8,"
        "  last_error=?9, updated_at=datetime('now') WHERE id=?1");
    int rc;
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, id);
    if (release_id <= 0)
        sqlite3_bind_null(st, 2);
    else
        sqlite3_bind_int64(st, 2, release_id);
    sqlite3_bind_text(st, 3, path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, fingerprint, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, manifest_sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, verify_status, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, last_scan, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, last_error, -1, SQLITE_TRANSIENT);
    rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

int
mp_library_package_sweep(mp_library *lib, const char *last_scan)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "UPDATE packages SET status='unavailable', last_error='package "
        "directory not found', updated_at=datetime('now')"
        " WHERE last_scan != ?1 AND status != 'unavailable'"
        " AND status != 'conflict'");
    int changed = 0;
    if (st == 0)
        return -1;
    sqlite3_bind_text(st, 1, last_scan, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_DONE)
        changed = sqlite3_changes(mp_db_sqlite(lib->db));
    else {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return changed;
}

/* ---- hierarchy writes --------------------------------------------------- */

/* Phase 2A artist resolution (content-anchored first-write-wins):
     1. valid credit MBID already anchored  -> reuse that row, no text writes
     2. exact-case name, then NOCASE name   -> reuse; adopt MBID into an
        anchor-less row; keep a conflicting anchor + warn; fill NULL sort name
     3. otherwise insert. Names are never rewritten; anchors are never
     overwritten. Role never affects resolution. */
long long
mp_library_upsert_artist(mp_library *lib, const musicpack_artist *credit)
{
    sqlite3 *db = mp_db_sqlite(lib->db);
    const char *mbid = credit->musicbrainz_id;
    sqlite3_stmt *st;
    long long id = -1;

    if (mbid != 0 && !mp_identity_valid_mbid(mbid)) {
        MP_LOGD("artist '%s': non-canonical musicbrainz id ignored",
                credit->name);
        mbid = 0;
    }

    /* 1. MBID anchor: id preserved; display name and anchor never touched;
       sort name is filled if empty (P2A.7: "sort_name filled if empty") */
    if (mbid != 0) {
        int fill_sort = 0;
        st = stmt_prepare(lib,
            "SELECT id, sort_name FROM artists WHERE musicbrainz_id = ?1");
        if (st == 0)
            return -1;
        sqlite3_bind_text(st, 1, mbid, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            id = sqlite3_column_int64(st, 0);
            fill_sort = credit->sort_name != 0 &&
                        sqlite3_column_text(st, 1) == 0;
        }
        sqlite3_finalize(st);
        if (id > 0) {
            if (fill_sort) {
                st = stmt_prepare(lib,
                    "UPDATE artists SET sort_name = ?2 WHERE id = ?1");
                if (st == 0)
                    return -1;
                sqlite3_bind_int64(st, 1, id);
                sqlite3_bind_text(st, 2, credit->sort_name, -1,
                                  SQLITE_TRANSIENT);
                if (sqlite3_step(st) != SQLITE_DONE) {
                    sqlite3_finalize(st);
                    return -1;
                }
                sqlite3_finalize(st);
            }
            return id;
        }
    }

    /* 2. exact-case (binary) name match, then the legacy NOCASE merge.
       Column text is only read while the matched statement is stepped. */
    {
        int found = 0, adopt_mbid = 0, fill_sort = 0, conflict = 0;
        char stored_mbid[64];

        stored_mbid[0] = '\0';
        st = stmt_prepare(lib,
            "SELECT id, musicbrainz_id, sort_name FROM artists"
            " WHERE name = ?1 COLLATE BINARY");
        if (st == 0)
            return -1;
        sqlite3_bind_text(st, 1, credit->name, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) == SQLITE_ROW) {
            const char *sm = (const char *) sqlite3_column_text(st, 1);
            found = 1;
            id = sqlite3_column_int64(st, 0);
            snprintf(stored_mbid, sizeof stored_mbid, "%s", sm != 0 ? sm : "");
            if (mbid != 0) {
                if (sm == 0)
                    adopt_mbid = 1;
                else if (strcmp(sm, mbid) != 0)
                    conflict = 1;
            }
            fill_sort = credit->sort_name != 0 &&
                        sqlite3_column_text(st, 2) == 0;
        }
        sqlite3_finalize(st);

        if (!found) {
            st = stmt_prepare(lib,
                "SELECT id, musicbrainz_id, sort_name FROM artists"
                " WHERE name = ?1 COLLATE NOCASE LIMIT 1");
            if (st == 0)
                return -1;
            sqlite3_bind_text(st, 1, credit->name, -1, SQLITE_TRANSIENT);
            if (sqlite3_step(st) == SQLITE_ROW) {
                const char *sm = (const char *) sqlite3_column_text(st, 1);
                found = 1;
                id = sqlite3_column_int64(st, 0);
                snprintf(stored_mbid, sizeof stored_mbid, "%s",
                         sm != 0 ? sm : "");
                if (mbid != 0) {
                    if (sm == 0)
                        adopt_mbid = 1;
                    else if (strcmp(sm, mbid) != 0)
                        conflict = 1;
                }
                fill_sort = credit->sort_name != 0 &&
                            sqlite3_column_text(st, 2) == 0;
            }
            sqlite3_finalize(st);
        }

        /* 3. adopt / fill / warn on the matched row (never rewrite) */
        if (found && id > 0) {
            if (adopt_mbid) {
                st = stmt_prepare(lib,
                    "UPDATE artists SET musicbrainz_id = ?2 WHERE id = ?1");
                if (st == 0)
                    return -1;
                sqlite3_bind_int64(st, 1, id);
                sqlite3_bind_text(st, 2, mbid, -1, SQLITE_TRANSIENT);
                if (sqlite3_step(st) != SQLITE_DONE) {
                    sqlite3_finalize(st);
                    return -1;
                }
                sqlite3_finalize(st);
            }
            if (fill_sort) {
                st = stmt_prepare(lib,
                    "UPDATE artists SET sort_name = ?2 WHERE id = ?1");
                if (st == 0)
                    return -1;
                sqlite3_bind_int64(st, 1, id);
                sqlite3_bind_text(st, 2, credit->sort_name, -1,
                                  SQLITE_TRANSIENT);
                if (sqlite3_step(st) != SQLITE_DONE) {
                    sqlite3_finalize(st);
                    return -1;
                }
                sqlite3_finalize(st);
            }
            if (conflict) {
                /* the stored anchor wins; the conflicting claim is only
                   reported, ingest always succeeds */
                MP_LOGW("artist '%s': conflicting musicbrainz id %s kept %s",
                        credit->name, stored_mbid, mbid);
            }
            return id;
        }
    }

    /* 4. insert */
    st = stmt_prepare(lib,
        "INSERT INTO artists(name, sort_name, musicbrainz_id)"
        " VALUES (?1, ?2, ?3)");
    if (st == 0)
        return -1;
    sqlite3_bind_text(st, 1, credit->name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, credit->sort_name, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, mbid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return sqlite3_last_insert_rowid(db);
}

static int
replace_group_artists(mp_library *lib, long long group_id,
                      const musicpack_manifest *m)
{
    sqlite3_stmt *st;
    size_t i;
    st = stmt_prepare(lib, "DELETE FROM group_artists WHERE group_id = ?1");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, group_id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    for (i = 0; i < m->album_artist_count; i++) {
        long long artist_id = mp_library_upsert_artist(lib,
                                                        &m->album_artists[i]);
        if (artist_id <= 0)
            return -1;

        st = stmt_prepare(lib,
            "INSERT INTO group_artists(group_id, artist_id, position, role)"
            " VALUES (?1, ?2, ?3, ?4)");
        if (st == 0)
            return -1;
        sqlite3_bind_int64(st, 1, group_id);
        sqlite3_bind_int64(st, 2, artist_id);
        sqlite3_bind_int64(st, 3, (long long) i);
        sqlite3_bind_text(st, 4, m->album_artists[i].role, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
    }
    return 0;
}

int
mp_library_release_lookup(mp_library *lib, const char *group_key,
                          const char *release_key, long long *group_id,
                          long long *release_id, long long *owner_id)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT g.id, r.id, COALESCE(r.owner_package_id, 0)"
        "  FROM release_groups g"
        "  JOIN releases r ON r.group_id = g.id"
        " WHERE g.group_key = ?1 AND r.release_key = ?2 LIMIT 1");
    int found = 0;
    if (st == 0)
        return 0;
    sqlite3_bind_text(st, 1, group_key, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, release_key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        found = 1;
        if (group_id != 0)
            *group_id = sqlite3_column_int64(st, 0);
        if (release_id != 0)
            *release_id = sqlite3_column_int64(st, 1);
        if (owner_id != 0)
            *owner_id = sqlite3_column_int64(st, 2);
    }
    sqlite3_finalize(st);
    return found;
}

int
mp_library_release_set_owner(mp_library *lib, long long release_id,
                             long long package_id)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "UPDATE releases SET owner_package_id = ?2,"
        " updated_at = datetime('now') WHERE id = ?1");
    int rc;
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, release_id);
    if (package_id < 0)
        sqlite3_bind_null(st, 2);
    else
        sqlite3_bind_int64(st, 2, package_id);
    rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

/* Builds a JSON array string from manifest genres, or returns NULL when
   no genres are supplied. The caller owns the returned allocation. */
static char *
genres_json(const musicpack_manifest *m)
{
    size_t i, cap, pos;
    char *json;
    if (m->genre_count == 0)
        return 0;
    /* worst case: every char might need escaping (\ or ") */
    cap = 3; /* [ + ] + NUL */
    for (i = 0; i < m->genre_count; i++)
        cap += strlen(m->genres[i]) * 2 + 3; /* "str", */
    json = (char *) malloc(cap);
    if (json == 0)
        return 0;
    pos = 0;
    json[pos++] = '[';
    for (i = 0; i < m->genre_count; i++) {
        const char *s = m->genres[i];
        if (i > 0)
            json[pos++] = ',';
        json[pos++] = '"';
        while (*s != '\0') {
            if (*s == '"' || *s == '\\')
                json[pos++] = '\\';
            json[pos++] = *s++;
        }
        json[pos++] = '"';
    }
    json[pos++] = ']';
    json[pos] = '\0';
    return json;
}

long long
mp_library_upsert_group(mp_library *lib, const musicpack_manifest *m,
                        const char *group_key, int update_metadata)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT id FROM release_groups WHERE group_key = ?1");
    long long id = -1;
    if (st == 0)
        return -1;
    sqlite3_bind_text(st, 1, group_key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW) {
        id = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);
    if (id < 0) {
        st = stmt_prepare(lib,
            "INSERT INTO release_groups(title, release_type,"
            " original_release_date, mbid, group_key, genres_json)"
            " VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
        if (st == 0)
            return -1;
        sqlite3_bind_text(st, 1, m->album_title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, m->release_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, m->original_release_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, m->musicbrainz_release_group_id, -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, group_key, -1, SQLITE_TRANSIENT);
        {
            char *gj = genres_json(m);
            sqlite3_bind_text(st, 6, gj, -1, free);
        }
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
        id = sqlite3_last_insert_rowid(mp_db_sqlite(lib->db));
    } else if (update_metadata) {
        st = stmt_prepare(lib,
            "UPDATE release_groups SET title=?1, release_type=?2,"
            " original_release_date=?3, mbid=?4, genres_json=?5,"
            " updated_at=datetime('now') WHERE id=?6");
        if (st == 0)
            return -1;
        sqlite3_bind_text(st, 1, m->album_title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, m->release_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, m->original_release_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, m->musicbrainz_release_group_id, -1,
                          SQLITE_TRANSIENT);
        {
            char *gj = genres_json(m);
            sqlite3_bind_text(st, 5, gj, -1, free);
        }
        sqlite3_bind_int64(st, 6, id);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
    }
    if (update_metadata) {
        if (replace_group_artists(lib, id, m) != 0)
            return -1;
    }
    return id;
}

long long
mp_library_upsert_release(mp_library *lib, const musicpack_manifest *m,
                          long long group_id, const char *release_key,
                          int update_metadata)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT id FROM releases WHERE group_id = ?1 AND release_key = ?2");
    long long id = -1;
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, group_id);
    sqlite3_bind_text(st, 2, release_key, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) == SQLITE_ROW)
        id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    if (id < 0) {
        st = stmt_prepare(lib,
            "INSERT INTO releases(group_id, edition, release_date, country,"
            "  label, catalogue_number, notes, barcode, mbid, release_key,"
            "  source_type, source_store, source_id, identity_source,"
            "  identity_confidence, provenance_tool, provenance_tool_version,"
            "  album_lufs, album_true_peak_db, has_album_loudness,"
            "  loudness_algorithm)"
            " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16,"
            " ?17,?18,?19,?20,?21)");
        if (st == 0)
            return -1;
        sqlite3_bind_int64(st, 1, group_id);
        sqlite3_bind_text(st, 2, m->release.edition, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, m->release.release_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, m->release.country, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, m->release.label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, m->release.catalogue_number, -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 7, m->release.notes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 8, m->barcode, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 9, m->musicbrainz_release_id, -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 10, release_key, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 11, m->source_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 12, m->source_store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 13, m->source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 14, m->identity_source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 15, m->identity_confidence, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 16, m->provenance_tool, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 17, m->provenance_tool_version, -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 18, m->album_loudness.lufs);
        sqlite3_bind_double(st, 19, m->album_loudness.true_peak_db);
        sqlite3_bind_int(st, 20, m->has_album_loudness);
        sqlite3_bind_text(st, 21, m->loudness_algorithm, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
        id = sqlite3_last_insert_rowid(mp_db_sqlite(lib->db));
    } else if (update_metadata) {
        st = stmt_prepare(lib,
            "UPDATE releases SET edition=?2, release_date=?3, country=?4,"
            " label=?5, catalogue_number=?6, notes=?7, barcode=?8, mbid=?9,"
            " source_type=?10, source_store=?11, source_id=?12,"
            " identity_source=?13, identity_confidence=?14,"
            " provenance_tool=?15, provenance_tool_version=?16,"
            " album_lufs=?17, album_true_peak_db=?18, has_album_loudness=?19,"
            " loudness_algorithm=?20, updated_at=datetime('now') WHERE id=?1");
        if (st == 0)
            return -1;
        sqlite3_bind_int64(st, 1, id);
        sqlite3_bind_text(st, 2, m->release.edition, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 3, m->release.release_date, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, m->release.country, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 5, m->release.label, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 6, m->release.catalogue_number, -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 7, m->release.notes, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 8, m->barcode, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 9, m->musicbrainz_release_id, -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 10, m->source_type, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 11, m->source_store, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 12, m->source_id, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 13, m->identity_source, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 14, m->identity_confidence, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 15, m->provenance_tool, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 16, m->provenance_tool_version, -1,
                          SQLITE_TRANSIENT);
        sqlite3_bind_double(st, 17, m->album_loudness.lufs);
        sqlite3_bind_double(st, 18, m->album_loudness.true_peak_db);
        sqlite3_bind_int(st, 19, m->has_album_loudness);
        sqlite3_bind_text(st, 20, m->loudness_algorithm, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
    }
    return id;
}

/* Generates a fresh 128-bit uid rendered as 32 lowercase hex chars (the
   same format the schema migration backfill uses). Returns 0 on success. */
static int
generate_uid(char out[33])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char b[16];
    int i;
    if (mp_random_bytes(b, sizeof b) != 0)
        return -1;
    for (i = 0; i < 16; i++) {
        out[i * 2] = hex[b[i] >> 4];
        out[i * 2 + 1] = hex[b[i] & 0x0f];
    }
    out[32] = '\0';
    return 0;
}

/* Assigns a uid to a row that predates migration 6 without overwriting an
   existing one: uids are generated once and never change afterwards. */
static int
ensure_row_uid(mp_library *lib, const char *table, long long id)
{
    char sql[96];
    char uid[33];
    sqlite3_stmt *st;
    int rc;
    if (generate_uid(uid) != 0)
        return -1;
    snprintf(sql, sizeof sql,
             "UPDATE %s SET uid = ?1 WHERE id = ?2 AND uid IS NULL", table);
    st = stmt_prepare(lib, sql);
    if (st == 0)
        return -1;
    sqlite3_bind_text(st, 1, uid, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, id);
    rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

static long long
file_size_of(const char *path)
{
    struct stat st;
    if (path == 0 || stat(path, &st) != 0)
        return 0;
    return (long long) st.st_size;
}

static int
insert_track_artists(mp_library *lib, long long track_id,
                     const musicpack_track *t)
{
    size_t i;
    for (i = 0; i < t->artist_count; i++) {
        long long aid = mp_library_upsert_artist(lib, &t->artists[i]);
        sqlite3_stmt *st;
        if (aid <= 0)
            return -1;
        st = stmt_prepare(lib,
            "INSERT INTO track_artists(track_id, artist_id, position, role)"
            " VALUES (?1,?2,?3,?4)");
        if (st == 0)
            return -1;
        sqlite3_bind_int64(st, 1, track_id);
        sqlite3_bind_int64(st, 2, aid);
        sqlite3_bind_int64(st, 3, (long long) i);
        sqlite3_bind_text(st, 4, t->artists[i].role, -1, SQLITE_TRANSIENT);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
    }
    return 0;
}

static int
insert_asset_row(mp_library *lib, long long release_id, const char *root,
                 const char *kind, const char *role,
                 const char *rel, const char *sha)
{
    char abs[MUSICPACK_PATH_MAX + 2];
    char uid[33];
    sqlite3_stmt *st;
    long long size;

    if (musicpack_path_resolve(root, rel, abs, sizeof abs) != MUSICPACK_OK)
        return 0;
    size = file_size_of(abs);
    if (generate_uid(uid) != 0)
        return -1;
    st = stmt_prepare(lib,
        "INSERT INTO assets(release_id, kind, role, relative_path, sha256,"
        "  file_size, mime_type, uid) VALUES (?1,?2,?3,?4,?5,?6,?7,?8)");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, release_id);
    sqlite3_bind_text(st, 2, kind, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, role, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, rel, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 6, size);
    sqlite3_bind_text(st, 7, mp_mime_for_path(rel), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, uid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

/* ---------- diff-aware release content sync ------------------------------ */

/* A track row currently stored under the release, used to match incoming
   manifest tracks against existing rows so logically unchanged entities
   keep their row ids. */
typedef struct existing_track {
    long long id;
    long long media_id;
    int track_number;
    int disc_number;
    char sha256[MUSICPACK_SHA256_HEX_SIZE];
    int matched;
} existing_track;

/* An asset row currently stored under the release. */
typedef struct existing_asset {
    long long id;
    char *kind;
    char *role; /* NULL when SQL NULL */
    char *path;
    int matched;
} existing_asset;

static int
load_existing_tracks(mp_library *lib, long long release_id,
                     existing_track **out, size_t *out_n)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT t.id, t.media_id, t.track_number, me.disc_number, a.sha256"
        "  FROM tracks t"
        "  JOIN media me ON me.id = t.media_id"
        "  LEFT JOIN audio_objects a ON a.track_id = t.id"
        " WHERE me.release_id = ?1"
        " ORDER BY me.disc_number, t.track_number");
    existing_track *rows = 0;
    size_t n = 0, cap = 0;
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, release_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        existing_track *r;
        if (n == cap) {
            size_t next = cap != 0 ? cap * 2 : 32;
            existing_track *grown =
                (existing_track *) realloc(rows, next * sizeof *grown);
            if (grown == 0) {
                sqlite3_finalize(st);
                free(rows);
                return -1;
            }
            rows = grown;
            cap = next;
        }
        r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->id = sqlite3_column_int64(st, 0);
        r->media_id = sqlite3_column_int64(st, 1);
        r->track_number = sqlite3_column_int(st, 2);
        r->disc_number = sqlite3_column_int(st, 3);
        col_cpy(r->sha256, sizeof r->sha256, st, 4);
    }
    sqlite3_finalize(st);
    *out = rows;
    *out_n = n;
    return 0;
}

static int
load_existing_assets(mp_library *lib, long long release_id,
                     existing_asset **out, size_t *out_n)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT id, kind, role, relative_path FROM assets"
        " WHERE release_id = ?1 ORDER BY id");
    existing_asset *rows = 0;
    size_t n = 0, cap = 0;
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, release_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        existing_asset *r;
        if (n == cap) {
            size_t next = cap != 0 ? cap * 2 : 32;
            existing_asset *grown =
                (existing_asset *) realloc(rows, next * sizeof *grown);
            if (grown == 0) {
                sqlite3_finalize(st);
                goto fail;
            }
            rows = grown;
            cap = next;
        }
        r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->id = sqlite3_column_int64(st, 0);
        if (sqlite3_column_type(st, 1) != SQLITE_NULL)
            r->kind = strdup((const char *) sqlite3_column_text(st, 1));
        if (sqlite3_column_type(st, 2) != SQLITE_NULL)
            r->role = strdup((const char *) sqlite3_column_text(st, 2));
        if (sqlite3_column_type(st, 3) != SQLITE_NULL)
            r->path = strdup((const char *) sqlite3_column_text(st, 3));
        if (r->kind == 0 || r->path == 0) {
            sqlite3_finalize(st);
            goto fail;
        }
    }
    sqlite3_finalize(st);
    *out = rows;
    *out_n = n;
    return 0;
fail:
    for (cap = 0; cap < n; cap++) {
        free(rows[cap].kind);
        free(rows[cap].role);
        free(rows[cap].path);
    }
    free(rows);
    return -1;
}

static void
free_existing_assets(existing_asset **rows, size_t *n)
{
    size_t i;
    if (*rows == 0)
        return;
    for (i = 0; i < *n; i++) {
        free((*rows)[i].kind);
        free((*rows)[i].role);
        free((*rows)[i].path);
    }
    free(*rows);
    *rows = 0;
    *n = 0;
}

/* Finds the media row for a disc number, creating it when absent; updates
   format/title/position in place when present. Disc number is the identity
   of a medium within an edition. */
static int
upsert_media_row(mp_library *lib, long long release_id,
                 const musicpack_disc *disc, size_t position,
                 long long *media_id)
{
    sqlite3_stmt *st;
    long long id = -1;

    st = stmt_prepare(lib,
        "SELECT id FROM media WHERE release_id = ?1 AND disc_number = ?2");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, release_id);
    sqlite3_bind_int64(st, 2, disc->disc);
    if (sqlite3_step(st) == SQLITE_ROW)
        id = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);

    if (id >= 0) {
        st = stmt_prepare(lib,
            "UPDATE media SET format = ?1, title = ?2, position = ?3"
            " WHERE id = ?4");
        if (st == 0)
            return -1;
        sqlite3_bind_text(st, 1, disc->format, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 2, disc->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 3, (long long) position);
        sqlite3_bind_int64(st, 4, id);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
    } else {
        st = stmt_prepare(lib,
            "INSERT INTO media(release_id, disc_number, format, title,"
            "  position) VALUES (?1,?2,?3,?4,?5)");
        if (st == 0)
            return -1;
        sqlite3_bind_int64(st, 1, release_id);
        sqlite3_bind_int64(st, 2, disc->disc);
        sqlite3_bind_text(st, 3, disc->format, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(st, 4, disc->title, -1, SQLITE_TRANSIENT);
        sqlite3_bind_int64(st, 5, (long long) position);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
        id = sqlite3_last_insert_rowid(mp_db_sqlite(lib->db));
    }
    *media_id = id;
    return 0;
}

/* Positional identity: same disc + track number AND identical audio
   content. This is the common metadata-only re-ingest case. */
static existing_track *
find_track_by_position(existing_track *rows, size_t n, int disc_number,
                       int track_number, const char *sha256)
{
    size_t i;
    if (sha256 == 0 || sha256[0] == '\0')
        return 0; /* no content anchor: refuse to guess */
    for (i = 0; i < n; i++) {
        existing_track *r = &rows[i];
        if (!r->matched && r->disc_number == disc_number &&
            r->track_number == track_number &&
            strcmp(r->sha256, sha256) == 0)
            return r;
    }
    return 0;
}

/* Content identity fallback: exactly one unmatched row carries the same
   audio sha256 anywhere in the release. Survives renumbering within a disc
   and moves across discs. Ambiguous matches (duplicate audio) never match:
   both copies stay distinct entities keyed by their own positions. */
static existing_track *
find_track_by_content(existing_track *rows, size_t n, const char *sha256)
{
    existing_track *hit = 0;
    size_t i;
    if (sha256 == 0 || sha256[0] == '\0')
        return 0;
    for (i = 0; i < n; i++) {
        existing_track *r = &rows[i];
        if (!r->matched && strcmp(r->sha256, sha256) == 0) {
            if (hit != 0)
                return 0; /* ambiguous */
            hit = r;
        }
    }
    return hit;
}

/* Codec probe result precedence shared by the insert and update paths. */
static void
resolve_audio_codec(char *codec, size_t cap, const mp_track_ingest *ti,
                    const musicpack_track *tr)
{
    if (ti != 0 && ti->codec.codec[0] != '\0')
        snprintf(codec, cap, "%s", ti->codec.codec);
    else
        snprintf(codec, cap, "%s", mp_codec_for_path(tr->audio.path));
}

static long long
audio_file_size(const mp_track_ingest *ti)
{
    return ti != 0 ? file_size_of(ti->abs_path) : 0;
}

/* Writes one track_waveforms row (the caller clears any previous row). */
static int
insert_waveform_row(mp_library *lib, long long track_id, const char *root,
                    const musicpack_waveform_ref *w)
{
    char wpath[MUSICPACK_PATH_MAX + 2];
    long long wsize;
    sqlite3_stmt *st;
    if (snprintf(wpath, sizeof wpath, "%s/%s", root, w->path)
        >= (int) sizeof wpath)
        return -1;
    wsize = file_size_of(wpath);
    st = stmt_prepare(lib,
        "INSERT INTO track_waveforms(track_id, version, relative_path,"
        "  sha256, file_size, mime_type, interval_ms, encoding,"
        "  floor_db, points) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10)");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, track_id);
    sqlite3_bind_int(st, 2, w->version);
    sqlite3_bind_text(st, 3, w->path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, w->sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 5, wsize);
    sqlite3_bind_text(st, 6, mp_mime_for_path(w->path), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, w->interval_ms);
    sqlite3_bind_text(st, 8, w->encoding, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 9, w->floor_db);
    sqlite3_bind_int64(st, 10, (long long) w->points);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

/* Refreshes waveform state to match the manifest: remove any stored row,
   then store the new reference when the manifest declares one. */
static int
sync_track_waveform(mp_library *lib, long long track_id, const char *root,
                    const musicpack_waveform_ref *w)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "DELETE FROM track_waveforms WHERE track_id = ?1");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, track_id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    if (!w->present)
        return 0;
    return insert_waveform_row(lib, track_id, root, w);
}

/* ---------- alternate audio representations (Phase 3) --------------------- */

/* A variant row currently stored under the release, used to match incoming
   manifest representations so unchanged entities keep their row ids
   (natural key: owning track + relative path, mirroring assets). */
typedef struct existing_variant {
    long long id;
    long long track_id;
    char *path;
    int matched;
} existing_variant;

static void
free_existing_variants(existing_variant **rows, size_t *n)
{
    size_t i;
    if (*rows == 0)
        return;
    for (i = 0; i < *n; i++)
        free((*rows)[i].path);
    free(*rows);
    *rows = 0;
    *n = 0;
}

static int
load_existing_variants(mp_library *lib, long long release_id,
                       existing_variant **out, size_t *out_n)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT v.id, v.track_id, v.relative_path"
        "  FROM audio_variants v"
        "  JOIN tracks t ON t.id = v.track_id"
        "  JOIN media me ON me.id = t.media_id"
        " WHERE me.release_id = ?1");
    existing_variant *rows = 0;
    size_t n = 0, cap = 0;
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, release_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        existing_variant *r;
        if (n == cap) {
            size_t next = cap != 0 ? cap * 2 : 8;
            existing_variant *grown =
                (existing_variant *) realloc(rows, next * sizeof *grown);
            if (grown == 0) {
                sqlite3_finalize(st);
                free(rows);
                return -1;
            }
            rows = grown;
            cap = next;
        }
        r = &rows[n++];
        memset(r, 0, sizeof *r);
        r->id = sqlite3_column_int64(st, 0);
        r->track_id = sqlite3_column_int64(st, 1);
        r->path = strdup((const char *) sqlite3_column_text(st, 2));
        if (r->path == 0) {
            sqlite3_finalize(st);
            free_existing_variants(&rows, &n);
            return -1;
        }
    }
    sqlite3_finalize(st);
    *out = rows;
    *out_n = n;
    return 0;
}

static existing_variant *
find_variant_slot(existing_variant *rows, size_t n, long long track_id,
                  const char *rel)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (rows[i].track_id == track_id && !rows[i].matched &&
            strcmp(rows[i].path, rel) == 0)
            return &rows[i];
    }
    return 0;
}

/* Syncs one manifest representation against an existing row (update in
   place: content/label may change; the id survives). */
static int
update_variant_row(mp_library *lib, const existing_variant *ea,
                   const char *root, const musicpack_representation *rep,
                   size_t position)
{
    sqlite3_stmt *st;
    char abs[MUSICPACK_PATH_MAX + 2];
    mp_codec_info probe;
    long long size = 0;

    memset(&probe, 0, sizeof probe);
    if (musicpack_path_resolve(root, rep->path, abs, sizeof abs)
        == MUSICPACK_OK) {
        size = file_size_of(abs);
        /* Never fails on unreadable/unsupported files: best-effort fields. */
        (void) mp_codec_probe(abs, rep->path, &probe);
    }
    st = stmt_prepare(lib,
        "UPDATE audio_variants SET sha256 = ?1, file_size = ?2,"
        "  mime_type = ?3, codec = ?4, stream_version = ?5,"
        "  sample_rate = ?6, channels = ?7, label = ?8, position = ?9"
        " WHERE id = ?10");
    if (st == 0)
        return -1;
    sqlite3_bind_text(st, 1, rep->sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, size);
    sqlite3_bind_text(st, 3, mp_mime_for_path(rep->path), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, probe.codec[0] != '\0' ? probe.codec
                          : mp_codec_for_path(rep->path), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 5, probe.stream_version);
    sqlite3_bind_int64(st, 6, probe.sample_rate);
    sqlite3_bind_int64(st, 7, probe.channels);
    sqlite3_bind_text(st, 8, rep->label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 9, (long long) position);
    sqlite3_bind_int64(st, 10, ea->id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return ensure_row_uid(lib, "audio_variants", ea->id);
}

/* Inserts a brand-new variant row for a freshly seen representation. */
static int
insert_variant_row(mp_library *lib, long long track_id, const char *root,
                   const musicpack_representation *rep, size_t position)
{
    sqlite3_stmt *st;
    char abs[MUSICPACK_PATH_MAX + 2];
    mp_codec_info probe;
    char uid[33];
    long long size = 0;

    memset(&probe, 0, sizeof probe);
    if (musicpack_path_resolve(root, rep->path, abs, sizeof abs)
        == MUSICPACK_OK) {
        size = file_size_of(abs);
        (void) mp_codec_probe(abs, rep->path, &probe);
    }
    if (generate_uid(uid) != 0)
        return -1;
    st = stmt_prepare(lib,
        "INSERT INTO audio_variants(track_id, relative_path, sha256,"
        "  file_size, mime_type, codec, stream_version, sample_rate,"
        "  channels, label, position, uid)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12)");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, track_id);
    sqlite3_bind_text(st, 2, rep->path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, rep->sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, size);
    sqlite3_bind_text(st, 5, mp_mime_for_path(rep->path), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, probe.codec[0] != '\0' ? probe.codec
                          : mp_codec_for_path(rep->path), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, probe.stream_version);
    sqlite3_bind_int64(st, 8, probe.sample_rate);
    sqlite3_bind_int64(st, 9, probe.channels);
    sqlite3_bind_text(st, 10, rep->label, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 11, (long long) position);
    sqlite3_bind_text(st, 12, uid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return 0;
}

/* Diff-aware sync of one track's representations: match by natural key
   (track_id + relative_path), update matched rows in place, insert new
   ones, delete whatever vanished. Runs inside the caller's transaction. */
static int
sync_track_variants(mp_library *lib, long long track_id, const char *root,
                    const musicpack_track *tr,
                    existing_variant *rows, size_t row_n)
{
    size_t r;

    for (r = 0; r < tr->representation_count; r++) {
        existing_variant *ev =
            find_variant_slot(rows, row_n, track_id,
                              tr->representations[r].path);
        if (ev != 0) {
            ev->matched = 1;
            if (update_variant_row(lib, ev, root, &tr->representations[r],
                                   r) != 0)
                return -1;
        } else if (insert_variant_row(lib, track_id, root,
                                      &tr->representations[r], r) != 0) {
            return -1;
        }
    }
    /* Delete variants of this track whose entity no longer exists.
       (Cross-track leftovers are handled by the caller's sweep.) */
    for (r = 0; r < row_n; r++) {
        sqlite3_stmt *st;
        if (rows[r].matched || rows[r].track_id != track_id)
            continue;
        /* Only delete when this sync owns that track's current pass; other
           tracks' stale rows are swept in their own sync call. */
        st = stmt_prepare(lib, "DELETE FROM audio_variants WHERE id = ?1");
        if (st == 0)
            return -1;
        sqlite3_bind_int64(st, 1, rows[r].id);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            return -1;
        }
        sqlite3_finalize(st);
        rows[r].matched = 1; /* deleted: never revisit */
    }
    return 0;
}

/* Updates an existing track row in place (same rowid), including its
   1:1 audio object, artist credits, and waveform. */
static int
update_track_row(mp_library *lib, const existing_track *r, long long media_id,
                 const musicpack_track *tr, const mp_track_ingest *ti,
                 const char *root, existing_variant *variants,
                 size_t variant_n)
{
    sqlite3_stmt *st;
    char codec[24];
    long long size;

    st = stmt_prepare(lib,
        "UPDATE tracks SET media_id = ?1, track_number = ?2, title = ?3,"
        "  isrc = ?4, mbid_track = ?5, mbid_recording = ?6,"
        "  source_store = ?7, source_track_id = ?8, source_audio_codec = ?9,"
        "  source_audio_md5 = ?10, has_duration = ?11, duration = ?12,"
        "  has_loudness = ?13, loudness_lufs = ?14,"
        "  loudness_true_peak_db = ?15 WHERE id = ?16");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, media_id);
    sqlite3_bind_int64(st, 2, tr->number);
    sqlite3_bind_text(st, 3, tr->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, tr->isrc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, tr->musicbrainz_track_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, tr->musicbrainz_recording_id, -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, tr->source_store, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, tr->source_track_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, tr->source_audio_codec, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, tr->source_audio_md5, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 11, tr->has_duration);
    sqlite3_bind_double(st, 12, tr->duration);
    sqlite3_bind_int(st, 13, tr->loudness.present);
    sqlite3_bind_double(st, 14, tr->loudness.lufs);
    sqlite3_bind_double(st, 15, tr->loudness.true_peak_db);
    sqlite3_bind_int64(st, 16, r->id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    if (ensure_row_uid(lib, "tracks", r->id) != 0)
        return -1;

    resolve_audio_codec(codec, sizeof codec, ti, tr);
    size = audio_file_size(ti);
    st = stmt_prepare(lib,
        "UPDATE audio_objects SET relative_path = ?1, sha256 = ?2,"
        "  file_size = ?3, mime_type = ?4, codec = ?5,"
        "  stream_version = ?6, sample_rate = ?7, channels = ?8"
        " WHERE track_id = ?9");
    if (st == 0)
        return -1;
    sqlite3_bind_text(st, 1, tr->audio.path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 2, tr->audio.sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 3, size);
    sqlite3_bind_text(st, 4, mp_mime_for_path(tr->audio.path), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, codec, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 6, ti != 0 ? ti->codec.stream_version : 0);
    sqlite3_bind_int64(st, 7, ti != 0 ? ti->codec.sample_rate : 0);
    sqlite3_bind_int64(st, 8, ti != 0 ? ti->codec.channels : 0);
    sqlite3_bind_int64(st, 9, r->id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);

    /* Artist credits may change freely; artist rows themselves are shared
       and stable (name-keyed upsert). */
    st = stmt_prepare(lib, "DELETE FROM track_artists WHERE track_id = ?1");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, r->id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    if (insert_track_artists(lib, r->id, tr) != 0)
        return -1;

    if (sync_track_waveform(lib, r->id, root, &tr->waveform) != 0)
        return -1;
    return sync_track_variants(lib, r->id, root, tr, variants, variant_n);
}

/* Inserts a brand-new track entity with fresh row ids. */
static int
insert_track_row(mp_library *lib, long long media_id,
                 const musicpack_track *tr, const mp_track_ingest *ti,
                 const char *root)
{
    sqlite3_stmt *st;
    char codec[24];
    char uid[33];
    long long track_id;
    long long size;

    if (generate_uid(uid) != 0)
        return -1;
    st = stmt_prepare(lib,
        "INSERT INTO tracks(media_id, track_number, title, isrc,"
        "  mbid_track, mbid_recording, source_store, source_track_id,"
        "  source_audio_codec, source_audio_md5, has_duration,"
        "  duration, has_loudness, loudness_lufs, loudness_true_peak_db,"
        "  uid)"
        " VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9,?10,?11,?12,?13,?14,?15,?16)");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, media_id);
    sqlite3_bind_int64(st, 2, tr->number);
    sqlite3_bind_text(st, 3, tr->title, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 4, tr->isrc, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 5, tr->musicbrainz_track_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, tr->musicbrainz_recording_id, -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 7, tr->source_store, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 8, tr->source_track_id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 9, tr->source_audio_codec, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 10, tr->source_audio_md5, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 11, tr->has_duration);
    sqlite3_bind_double(st, 12, tr->duration);
    sqlite3_bind_int(st, 13, tr->loudness.present);
    sqlite3_bind_double(st, 14, tr->loudness.lufs);
    sqlite3_bind_double(st, 15, tr->loudness.true_peak_db);
    sqlite3_bind_text(st, 16, uid, -1, SQLITE_TRANSIENT);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    track_id = sqlite3_last_insert_rowid(mp_db_sqlite(lib->db));

    resolve_audio_codec(codec, sizeof codec, ti, tr);
    size = audio_file_size(ti);
    st = stmt_prepare(lib,
        "INSERT INTO audio_objects(track_id, relative_path, sha256,"
        "  file_size, mime_type, codec, stream_version, sample_rate,"
        "  channels) VALUES (?1,?2,?3,?4,?5,?6,?7,?8,?9)");
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, track_id);
    sqlite3_bind_text(st, 2, tr->audio.path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 3, tr->audio.sha256, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, size);
    sqlite3_bind_text(st, 5, mp_mime_for_path(tr->audio.path), -1,
                      SQLITE_TRANSIENT);
    sqlite3_bind_text(st, 6, codec, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(st, 7, ti != 0 ? ti->codec.stream_version : 0);
    sqlite3_bind_int64(st, 8, ti != 0 ? ti->codec.sample_rate : 0);
    sqlite3_bind_int64(st, 9, ti != 0 ? ti->codec.channels : 0);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);

    if (insert_track_artists(lib, track_id, tr) != 0)
        return -1;

    if (tr->waveform.present &&
        insert_waveform_row(lib, track_id, root, &tr->waveform) != 0)
        return -1;
    /* A new track's representations are all inserts by definition; pass a
       null snapshot so every entry takes the insert path. */
    if (sync_track_variants(lib, track_id, root, tr, 0, 0) != 0)
        return -1;
    return 0;
}

static int
delete_track_row(mp_library *lib, long long track_id)
{
    sqlite3_stmt *st = stmt_prepare(lib, "DELETE FROM tracks WHERE id = ?1");
    int rc;
    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, track_id);
    rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
    sqlite3_finalize(st);
    return rc;
}

/* Removes media rows whose disc number no longer exists in the manifest.
   Their tracks were already handled above (re-parented by content or
   deleted); this only removes the empty medium shells. */
static int
delete_absent_media(mp_library *lib, long long release_id,
                    const musicpack_manifest *m)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT id, disc_number FROM media WHERE release_id = ?1");
    long long *ids = 0;
    size_t n = 0, cap = 0, i;
    int rc;

    if (st == 0)
        return -1;
    sqlite3_bind_int64(st, 1, release_id);
    while (sqlite3_step(st) == SQLITE_ROW) {
        int disc = sqlite3_column_int(st, 1);
        size_t d;
        int present = 0;
        for (d = 0; d < m->disc_count; d++) {
            if (m->discs[d].disc == disc) {
                present = 1;
                break;
            }
        }
        if (present)
            continue;
        if (n == cap) {
            size_t next = cap != 0 ? cap * 2 : 8;
            long long *grown =
                (long long *) realloc(ids, next * sizeof *grown);
            if (grown == 0) {
                sqlite3_finalize(st);
                free(ids);
                return -1;
            }
            ids = grown;
            cap = next;
        }
        ids[n++] = sqlite3_column_int64(st, 0);
    }
    sqlite3_finalize(st);

    rc = 0;
    for (i = 0; rc == 0 && i < n; i++) {
        st = stmt_prepare(lib, "DELETE FROM media WHERE id = ?1");
        if (st == 0)
            return -1;
        sqlite3_bind_int64(st, 1, ids[i]);
        rc = sqlite3_step(st) == SQLITE_DONE ? 0 : -1;
        sqlite3_finalize(st);
    }
    free(ids);
    return rc;
}

static int
asset_key_match(const existing_asset *ea, const char *kind,
                const char *role, const char *path)
{
    int role_eq = (ea->role == 0 && role == 0) ||
                  (ea->role != 0 && role != 0 &&
                   strcmp(ea->role, role) == 0);
    return strcmp(ea->kind, kind) == 0 && role_eq &&
           strcmp(ea->path, path) == 0;
}

static existing_asset *
find_asset_slot(existing_asset *rows, size_t n, const char *kind,
                const char *role, const char *path)
{
    size_t i;
    for (i = 0; i < n; i++)
        if (!rows[i].matched && asset_key_match(&rows[i], kind, role, path))
            return &rows[i];
    return 0;
}

/* Syncs one incoming asset against the existing rows: update in place when
   the (kind, role, relative_path) key already exists, insert when new. */
static int
sync_one_asset(mp_library *lib, long long release_id, const char *root,
               const char *kind, const char *role, const char *rel,
               const char *sha, existing_asset *rows, size_t n)
{
    existing_asset *ea = find_asset_slot(rows, n, kind, role, rel);
    char abs[MUSICPACK_PATH_MAX + 2];
    long long size = 0;
    sqlite3_stmt *st;

    if (ea == 0)
        return insert_asset_row(lib, release_id, root, kind, role, rel, sha);
    ea->matched = 1;
    if (musicpack_path_resolve(root, rel, abs, sizeof abs) == MUSICPACK_OK)
        size = file_size_of(abs);
    st = stmt_prepare(lib,
        "UPDATE assets SET sha256 = ?1, file_size = ?2, mime_type = ?3"
        " WHERE id = ?4");
    if (st == 0)
        return -1;
    sqlite3_bind_text(st, 1, sha, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 2, size);
    sqlite3_bind_text(st, 3, mp_mime_for_path(rel), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(st, 4, ea->id);
    if (sqlite3_step(st) != SQLITE_DONE) {
        sqlite3_finalize(st);
        return -1;
    }
    sqlite3_finalize(st);
    return ensure_row_uid(lib, "assets", ea->id);
}

int
mp_library_replace_release_content(mp_library *lib, long long release_id,
                                   const musicpack_manifest *m,
                                   const char *root,
                                   const mp_track_ingest *codecs,
                                   size_t codec_count)
{
    existing_track *old_tracks = 0;
    size_t old_track_n = 0;
    existing_asset *old_assets = 0;
    size_t old_asset_n = 0;
    long long *media_ids = 0;
    existing_variant *variants = 0;
    size_t variant_n = 0;
    size_t ci = 0;
    size_t d, t, i;
    int rc = -1;

    /* Snapshot what this release currently stores so entities that still
       exist in the incoming manifest can be updated in place instead of
       deleted and recreated (public ids survive). */
    if (load_existing_tracks(lib, release_id, &old_tracks, &old_track_n) != 0)
        return -1;
    if (load_existing_assets(lib, release_id, &old_assets, &old_asset_n)
        != 0)
        goto out;
    if (load_existing_variants(lib, release_id, &variants, &variant_n) != 0)
        goto out;
    media_ids = (long long *) calloc(m->disc_count ? m->disc_count : 1,
                                     sizeof *media_ids);
    if (media_ids == 0)
        goto out;

    /* Media: upsert per disc number. */
    for (d = 0; d < m->disc_count; d++) {
        if (upsert_media_row(lib, release_id, &m->discs[d], d,
                             &media_ids[d]) != 0)
            goto out;
    }

    /* Tracks: match each manifest entry against existing rows, update the
       matched ones in place, insert the genuinely new ones. */
    for (d = 0; d < m->disc_count; d++) {
        const musicpack_disc *disc = &m->discs[d];
        for (t = 0; t < disc->track_count; t++) {
            const musicpack_track *tr = &disc->tracks[t];
            const mp_track_ingest *ti = ci < codec_count ? &codecs[ci] : 0;
            existing_track *mt;
            ci++;
            mt = find_track_by_position(old_tracks, old_track_n,
                                        disc->disc, tr->number,
                                        tr->audio.sha256);
            if (mt == 0)
                mt = find_track_by_content(old_tracks, old_track_n,
                                           tr->audio.sha256);
            if (mt != 0) {
                mt->matched = 1;
                if (update_track_row(lib, mt, media_ids[d], tr, ti, root,
                                     variants, variant_n)
                    != 0)
                    goto out;
            } else if (insert_track_row(lib, media_ids[d], tr, ti, root)
                       != 0) {
                goto out;
            }
        }
    }

    /* Delete tracks whose entity no longer exists (cascades audio objects,
       waveforms, and artist credits). */
    for (i = 0; i < old_track_n; i++) {
        if (!old_tracks[i].matched &&
            delete_track_row(lib, old_tracks[i].id) != 0)
            goto out;
    }

    if (delete_absent_media(lib, release_id, m) != 0)
        goto out;

    /* Assets: sync by natural key, then delete whatever vanished. */
    for (i = 0; i < m->artwork_count; i++) {
        if (sync_one_asset(lib, release_id, root, "artwork",
                           m->artwork[i].role, m->artwork[i].asset.path,
                           m->artwork[i].asset.sha256, old_assets,
                           old_asset_n) != 0)
            goto out;
    }
    for (i = 0; i < m->booklet_count; i++) {
        if (sync_one_asset(lib, release_id, root, "booklet", 0,
                           m->booklet[i].path, m->booklet[i].sha256,
                           old_assets, old_asset_n) != 0)
            goto out;
    }
    for (i = 0; i < m->lyrics_count; i++) {
        if (sync_one_asset(lib, release_id, root, "lyrics", 0,
                           m->lyrics[i].path, m->lyrics[i].sha256,
                           old_assets, old_asset_n) != 0)
            goto out;
    }
    for (i = 0; i < m->extras_count; i++) {
        if (sync_one_asset(lib, release_id, root, "extras", 0,
                           m->extras[i].path, m->extras[i].sha256,
                           old_assets, old_asset_n) != 0)
            goto out;
    }
    for (i = 0; i < old_asset_n; i++) {
        sqlite3_stmt *st;
        if (old_assets[i].matched)
            continue;
        st = stmt_prepare(lib, "DELETE FROM assets WHERE id = ?1");
        if (st == 0)
            goto out;
        sqlite3_bind_int64(st, 1, old_assets[i].id);
        if (sqlite3_step(st) != SQLITE_DONE) {
            sqlite3_finalize(st);
            goto out;
        }
        sqlite3_finalize(st);
    }

    rc = 0;
out:
    free(media_ids);
    free_existing_variants(&variants, &variant_n);
    free_existing_assets(&old_assets, &old_asset_n);
    free(old_tracks);
    return rc; /* nonzero rolls back the caller's transaction */
}

/* ---- object resolution for streaming ----------------------------------- */

static void
fill_object_ref(sqlite3_stmt *st, mp_object_ref *ref)
{
    memset(ref, 0, sizeof *ref);
    ref->id = sqlite3_column_int64(st, 0);
    ref->release_id = sqlite3_column_int64(st, 1);
    col_cpy(ref->package_path, sizeof ref->package_path, st, 2);
    col_cpy(ref->relative_path, sizeof ref->relative_path, st, 3);
    col_cpy(ref->mime, sizeof ref->mime, st, 4);
    col_cpy(ref->codec, sizeof ref->codec, st, 5);
    ref->file_size = sqlite3_column_int64(st, 6);
    col_cpy(ref->status, sizeof ref->status, st, 7);
    ref->stream_version = sqlite3_column_int(st, 8);
    ref->sample_rate = sqlite3_column_int64(st, 9);
    ref->channels = sqlite3_column_int64(st, 10);
    col_cpy(ref->sha256, sizeof ref->sha256, st, 11);
}

int
mp_library_track_audio(mp_library *lib, long long track_id, mp_object_ref *ref)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT a.id, r.id, p.path, a.relative_path, a.mime_type, a.codec,"
        "       a.file_size, p.status, a.stream_version, a.sample_rate,"
        "       a.channels, a.sha256"
        "  FROM tracks t"
        "  JOIN media me ON me.id = t.media_id"
        "  JOIN releases r ON r.id = me.release_id"
        "  JOIN packages p ON p.id = r.owner_package_id"
        "  JOIN audio_objects a ON a.track_id = t.id"
         " WHERE t.id = ?1 AND p.status IN ('valid','warning') AND p.verify_status IN ('valid','warning')"
        " LIMIT 1");
    int rc = 0;
    if (st == 0)
        return 0;
    sqlite3_bind_int64(st, 1, track_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        fill_object_ref(st, ref);
        rc = 1;
    }
    sqlite3_finalize(st);
    return rc;
}

int
mp_library_asset(mp_library *lib, long long asset_id, mp_object_ref *ref)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT a.id, r.id, p.path, a.relative_path, a.mime_type, '',"
        "       a.file_size, p.status, 0, 0, 0, a.sha256"
        "  FROM assets a"
        "  JOIN releases r ON r.id = a.release_id"
        "  JOIN packages p ON p.id = r.owner_package_id"
         " WHERE a.id = ?1 AND a.kind IN ('artwork','booklet','lyrics')"
         "   AND p.status IN ('valid','warning')"
         "   AND p.verify_status IN ('valid','warning')"
         " LIMIT 1");
    int rc = 0;
    if (st == 0)
        return 0;
    sqlite3_bind_int64(st, 1, asset_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        fill_object_ref(st, ref);
        rc = 1;
    }
    sqlite3_finalize(st);
    return rc;
}

int
mp_library_track_waveform(mp_library *lib, long long track_id, mp_object_ref *ref)
{
    sqlite3_stmt *st = stmt_prepare(lib,
        "SELECT w.track_id, r.id, p.path, w.relative_path, w.mime_type, '',"
        "       w.file_size, p.status, 0, 0, 0, w.sha256"
        "  FROM track_waveforms w"
        "  JOIN tracks t ON t.id = w.track_id"
        "  JOIN media me ON me.id = t.media_id"
        "  JOIN releases r ON r.id = me.release_id"
        "  JOIN packages p ON p.id = r.owner_package_id"
         " WHERE w.track_id = ?1"
         "   AND p.status IN ('valid','warning')"
         "   AND p.verify_status IN ('valid','warning')"
         " LIMIT 1");
    int rc = 0;
    if (st == 0)
        return 0;
    sqlite3_bind_int64(st, 1, track_id);
    if (sqlite3_step(st) == SQLITE_ROW) {
        fill_object_ref(st, ref);
        rc = 1;
    }
    sqlite3_finalize(st);
    return rc;
}
