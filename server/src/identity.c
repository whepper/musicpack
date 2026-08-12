/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  (BSD 3-clause, see identity.h)
*/
#include "identity.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/checksum.h>

/* ---------- small helpers ---------- */

/* Canonical identity serialization: each field is encoded as a 1-byte tag
   followed by a 4-byte big-endian length and the raw value bytes. Absent
   fields are simply not emitted; present-but-empty values emit a zero
   length. This removes the delimiter ambiguity of the previous free-text
   `key=value\n` encoding, so no two distinct field sets can collide. */
#define TAG_TITLE       1
#define TAG_DATE        2
#define TAG_TYPE        3
#define TAG_ARTIST_NAME 4
#define TAG_ARTIST_ROLE 5
#define TAG_EDITION     6
#define TAG_RELEASE_DATE 7
#define TAG_COUNTRY     8
#define TAG_LABEL       9
#define TAG_CATALOGUE   10
#define TAG_BARCODE     11

static musicpack_status
hash_text(const char *s, size_t len, char *out, size_t cap)
{
    return musicpack_sha256(s, len, out, cap);
}

/* Appends raw bytes while growing the canonical identity buffer. */
static musicpack_status
append_bytes(char **buf, size_t *cap, size_t *len, const char *data, size_t n)
{
    char *next;
    if (n > (size_t) -1 - *len - 1)
        return MUSICPACK_ERR_NOMEM;
    if (*len + n + 1 > *cap) {
        size_t next_cap = *cap;
        while (next_cap < *len + n + 1) {
            if (next_cap > (size_t) -1 / 2) {
                next_cap = *len + n + 1;
                break;
            }
            next_cap *= 2;
        }
        next = (char *) realloc(*buf, next_cap);
        if (next == 0)
            return MUSICPACK_ERR_NOMEM;
        *buf = next;
        *cap = next_cap;
    }
    memcpy(*buf + *len, data, n);
    *len += n;
    (*buf)[*len] = '\0';
    return MUSICPACK_OK;
}

/* Sorts album artists by (name, role) so a reordered artist list does not
   change identity; roles are part of the identity and a missing role sorts
   before an empty role deterministically. */
static int
artist_cmp(const musicpack_artist *a, const musicpack_artist *b)
{
    int c = strcmp(a->name, b->name);
    if (c != 0)
        return c;
    {
        const char *ra = a->role != 0 ? a->role : "";
        const char *rb = b->role != 0 ? b->role : "";
        return strcmp(ra, rb);
    }
}

static void
sort_artists(musicpack_artist *a, size_t n)
{
    size_t i, j;
    for (i = 1; i < n; i++) {
        musicpack_artist key = a[i];
        j = i;
        while (j > 0 && artist_cmp(&a[j - 1], &key) > 0) {
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

static musicpack_status
append_field(char **buf, size_t *cap, size_t *len, unsigned char tag,
             const char *value)
{
    size_t n;
    unsigned char hdr[5];

    if (value == 0)
        return MUSICPACK_OK; /* absent: not emitted */
    n = strlen(value);
    if (n > 0xFFFFFFFFu)
        return MUSICPACK_ERR_INVALID;
    hdr[0] = tag;
    hdr[1] = (unsigned char) (n >> 24);
    hdr[2] = (unsigned char) (n >> 16);
    hdr[3] = (unsigned char) (n >> 8);
    hdr[4] = (unsigned char) n;
    if (append_bytes(buf, cap, len, (const char *) hdr, 5) != MUSICPACK_OK)
        return MUSICPACK_ERR_NOMEM;
    return n > 0 ? append_bytes(buf, cap, len, value, n) : MUSICPACK_OK;
}

static musicpack_status
prefixed_key(const char *prefix, const char *value, char *out, size_t cap)
{
    size_t n = strlen(prefix) + strlen(value) + 1;
    if (n > cap)
        return MUSICPACK_ERR_INVALID;
    memcpy(out, prefix, strlen(prefix));
    strcpy(out + strlen(prefix), value);
    return MUSICPACK_OK;
}

/* Canonical MusicBrainz UUID: 8-4-4-4-12 hex digits with hyphens. A
   non-conforming value is not trusted as a global identity authority. */
static int
valid_mbid(const char *s)
{
    size_t i, n;
    if (s == 0)
        return 0;
    n = strlen(s);
    if (n != 36)
        return 0;
    for (i = 0; i < n; i++) {
        int is_dash = (i == 8 || i == 13 || i == 18 || i == 23);
        if (is_dash) {
            if (s[i] != '-')
                return 0;
        } else {
            if (!isxdigit((unsigned char) s[i]))
                return 0;
        }
    }
    return 1;
}

/* ---------- public API ---------- */

musicpack_status
mp_identity_manifest_hash(const char *json, size_t len, char *out, size_t cap)
{
    return musicpack_sha256(json, len, out, cap);
}

musicpack_status
mp_identity_package_fingerprint(const musicpack_manifest *m, char *out,
                                size_t cap)
{
    char *json = 0;
    musicpack_status s = musicpack_manifest_write(m, &json);
    if (s != MUSICPACK_OK)
        return s;
    s = musicpack_sha256(json, strlen(json), out, cap);
    free(json);
    return s;
}

musicpack_status
mp_identity_group_key(const musicpack_manifest *m, char *out, size_t cap)
{
    musicpack_artist *copy;
    size_t len = 0, bufcap = 256, i;
    char *buf;
    musicpack_status s;

    if (m->musicbrainz_release_group_id != 0 &&
        *m->musicbrainz_release_group_id != '\0' &&
        valid_mbid(m->musicbrainz_release_group_id)) {
        return prefixed_key("mb:", m->musicbrainz_release_group_id, out, cap);
    }
    buf = (char *) malloc(bufcap);
    if (buf == 0)
        return MUSICPACK_ERR_NOMEM;
    buf[0] = '\0';
    copy = (musicpack_artist *) calloc(m->album_artist_count,
                                       sizeof *copy);
    if (m->album_artist_count > 0 && copy == 0) {
        free(buf);
        return MUSICPACK_ERR_NOMEM;
    }
    if (m->album_artist_count > 0) {
        for (i = 0; i < m->album_artist_count; i++) {
            copy[i].name = m->album_artists[i].name;
            copy[i].role = m->album_artists[i].role;
        }
    }
    sort_artists(copy, m->album_artist_count);
    s = append_field(&buf, &bufcap, &len, TAG_TITLE, m->album_title);
    if (s == MUSICPACK_OK)
        s = append_field(&buf, &bufcap, &len, TAG_DATE, m->original_release_date);
    if (s == MUSICPACK_OK)
        s = append_field(&buf, &bufcap, &len, TAG_TYPE, m->release_type);
    for (i = 0; s == MUSICPACK_OK && i < m->album_artist_count; i++) {
        s = append_field(&buf, &bufcap, &len, TAG_ARTIST_NAME, copy[i].name);
        if (s == MUSICPACK_OK)
            s = append_field(&buf, &bufcap, &len, TAG_ARTIST_ROLE, copy[i].role);
    }
    free(copy);
    if (s == MUSICPACK_OK && cap >= 3) {
        memcpy(out, "h:", 2);
        s = hash_text(buf, len, out + 2, cap - 2);
    } else if (s == MUSICPACK_OK) {
        s = MUSICPACK_ERR_INVALID;
    }
    free(buf);
    return s;
}

musicpack_status
mp_identity_release_key(const musicpack_manifest *m, char *out, size_t cap)
{
    size_t len = 0, bufcap = 256;
    char *buf;
    musicpack_status s;

    if (m->musicbrainz_release_id != 0 &&
        *m->musicbrainz_release_id != '\0' &&
        valid_mbid(m->musicbrainz_release_id)) {
        return prefixed_key("mb:", m->musicbrainz_release_id, out, cap);
    }
    buf = (char *) malloc(bufcap);
    if (buf == 0)
        return MUSICPACK_ERR_NOMEM;
    buf[0] = '\0';
    s = append_field(&buf, &bufcap, &len, TAG_EDITION, m->release.edition);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, TAG_RELEASE_DATE, m->release.release_date);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, TAG_COUNTRY, m->release.country);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, TAG_LABEL, m->release.label);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, TAG_CATALOGUE, m->release.catalogue_number);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, TAG_BARCODE, m->barcode);
    if (s == MUSICPACK_OK && cap >= 3) {
        memcpy(out, "h:", 2);
        s = hash_text(buf, len, out + 2, cap - 2);
    } else if (s == MUSICPACK_OK) {
        s = MUSICPACK_ERR_INVALID;
    }
    free(buf);
    return s;
}
