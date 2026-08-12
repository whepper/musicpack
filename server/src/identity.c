/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.
  (BSD 3-clause, see identity.h)
*/
#include "identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <musicpack/checksum.h>

/* ---------- small helpers ---------- */

static musicpack_status
hash_text(const char *s, size_t len, char *out, size_t cap)
{
    return musicpack_sha256(s, len, out, cap);
}

/* Appends text while growing the canonical identity buffer. */
static musicpack_status
append_text(char **buf, size_t *cap, size_t *len, const char *text)
{
    size_t n = strlen(text);
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
    memcpy(*buf + *len, text, n);
    *len += n;
    (*buf)[*len] = '\0';
    return MUSICPACK_OK;
}

/* Sorts album artists by (name, role) so a reordered artist list does not
   change identity; roles are part of the identity. */
static void
sort_artists(musicpack_artist *a, size_t n)
{
    size_t i, j;
    for (i = 1; i < n; i++) {
        musicpack_artist key = a[i];
        j = i;
        while (j > 0) {
            int c = strcmp(a[j - 1].name, key.name);
            if (c == 0 && a[j - 1].role != 0 && key.role != 0)
                c = strcmp(a[j - 1].role, key.role);
            if (c <= 0)
                break;
            a[j] = a[j - 1];
            j--;
        }
        a[j] = key;
    }
}

static musicpack_status
append_field(char **buf, size_t *cap, size_t *len, const char *key,
             const char *value)
{
    musicpack_status s;
    if (value == 0)
        return MUSICPACK_OK;
    s = append_text(buf, cap, len, key);
    if (s == MUSICPACK_OK)
        s = append_text(buf, cap, len, "=");
    if (s == MUSICPACK_OK)
        s = append_text(buf, cap, len, value);
    if (s == MUSICPACK_OK)
        s = append_text(buf, cap, len, "\n");
    return s;
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
        *m->musicbrainz_release_group_id != '\0') {
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
    s = append_field(&buf, &bufcap, &len, "title", m->album_title);
    if (s == MUSICPACK_OK)
        s = append_field(&buf, &bufcap, &len, "date", m->original_release_date);
    if (s == MUSICPACK_OK)
        s = append_field(&buf, &bufcap, &len, "type", m->release_type);
    for (i = 0; s == MUSICPACK_OK && i < m->album_artist_count; i++) {
        s = append_field(&buf, &bufcap, &len, "artist", copy[i].name);
        if (s == MUSICPACK_OK)
            s = append_field(&buf, &bufcap, &len, "role", copy[i].role != 0 ? copy[i].role : "");
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
        *m->musicbrainz_release_id != '\0') {
        return prefixed_key("mb:", m->musicbrainz_release_id, out, cap);
    }
    buf = (char *) malloc(bufcap);
    if (buf == 0)
        return MUSICPACK_ERR_NOMEM;
    buf[0] = '\0';
    s = append_field(&buf, &bufcap, &len, "edition", m->release.edition);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, "date", m->release.release_date);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, "country", m->release.country);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, "label", m->release.label);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, "catalogue", m->release.catalogue_number);
    if (s == MUSICPACK_OK) s = append_field(&buf, &bufcap, &len, "barcode", m->barcode);
    if (s == MUSICPACK_OK && cap >= 3) {
        memcpy(out, "h:", 2);
        s = hash_text(buf, len, out + 2, cap - 2);
    } else if (s == MUSICPACK_OK) {
        s = MUSICPACK_ERR_INVALID;
    }
    free(buf);
    return s;
}
