/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved.

  Redistribution and use in source and binary forms, with or without
  modification, are permitted provided that the following conditions are
  met:

  * Redistributions of source code must retain the above copyright
  notice, this list of conditions and the following disclaimer.

  * Redistributions in binary form must reproduce the above
  copyright notice, this list of conditions and the following
  disclaimer in the documentation and/or other materials provided
  with the distribution.

  * Neither the name of the The MusicPack Development Team nor the
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
/// \file main.c
/// `musicpack` CLI: info / verify / create / import.

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <getopt.h>
#include <musicpack/musicpack.h>
#include <musepack/musepack.h>

#include <cJSON.h>

#include "draft.h"

#ifdef _WIN32
# include <direct.h>
# include <process.h>
# include <sys/stat.h>
# define mkdir_p_one(p) _mkdir(p)
# define POPEN _popen
# define POPEN_MODE "rb" /* binary mode matters on Windows */
# define PCLOSE _pclose
# define getpid _getpid
#else
# include <dirent.h>
# include <signal.h>
# include <sys/stat.h>
# include <sys/wait.h>
# include <unistd.h>
# define mkdir_p_one(p) mkdir(p, 0755)
# define POPEN popen
# define POPEN_MODE "r"  /* POSIX popen only accepts "r"/"w"; "rb" fails on macOS */
# define PCLOSE pclose
#endif

#define ABOUT "musicpack - MusicPack package tool " MUSICPACK_VERSION "\n"

/* Version of the JSON authoring surface consumed by MusicPack Author. The
   GUI refuses to talk to a backend whose authorApi does not match, so the
   CLI and the GUI can evolve independently without coupling to exact patch
   versions. Version 2 adds `encode-draft` (the FLAC/WAV -> Musepack stage). */
#define MUSICPACK_AUTHOR_API 2

static char *read_file_bounded(const char *path, size_t max, musicpack_status *status);
static void json_error_out(const char *code, const char *msg);
static void rm_rf(const char *path);
static int unique_target(char *path, size_t cap);

static int usage_error(const char *msg)
{
    fprintf(stderr, "%s: %s\n", ABOUT, msg);
    return 2;
}

/* ------------------------------------------------------------------ */
/* small file helpers                                                  */
/* ------------------------------------------------------------------ */

static int
mkdir_p(const char *path)
{
    char tmp[MUSICPACK_PATH_MAX + 2];
    size_t len = strlen(path), i;

    if (len == 0 || len >= sizeof tmp)
        return -1;
    memcpy(tmp, path, len + 1);
    if (tmp[len - 1] == '/')
        tmp[len - 1] = '\0';
    for (i = 1; tmp[i] != '\0'; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir_p_one(tmp) != 0 && errno != EEXIST)
                return -1;
            tmp[i] = '/';
        }
    }
    if (mkdir_p_one(tmp) != 0 && errno != EEXIST)
        return -1;
    return 0;
}

static int
copy_file(const char *src, const char *dst)
{
    FILE *in, *out;
    char buf[65536];
    size_t n;

    in = fopen(src, "rb");
    if (in == 0)
        return -1;
    out = fopen(dst, "wb");
    if (out == 0) { fclose(in); return -1; }
    while ((n = fread(buf, 1, sizeof buf, in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { fclose(in); fclose(out); return -1; }
    if (ferror(in)) { fclose(in); fclose(out); return -1; }
    fclose(in);
    if (fclose(out) != 0)
        return -1;
    return 0;
}

static int
write_all(const char *path, const char *data)
{
    FILE *f = fopen(path, "wb");
    size_t len = strlen(data);
    int ok;
    if (f == 0)
        return -1;
    ok = (len == 0 || fwrite(data, 1, len, f) == len) && fclose(f) == 0;
    return ok ? 0 : -1;
}

/* Create beside, rather than inside, the destination so rename is atomic. */
static int
prepare_stage(const char *final_dir, const char *kind, char *stage, size_t cap)
{
    struct stat st;

#ifdef _WIN32
    if (stat(final_dir, &st) == 0)
#else
    if (lstat(final_dir, &st) == 0)
#endif
        return 0;
    if (snprintf(stage, cap, "%s.%s-%ld", final_dir, kind, (long) getpid())
            >= (int) cap)
        return 0;
#ifdef _WIN32
    if (stat(stage, &st) == 0)
#else
    if (lstat(stage, &st) == 0)
#endif
        return 0;
    return 1;
}

static int
verify_staged_package(const char *dir)
{
    musicpack_package *pkg;
    musicpack_report rep = { 0, 0 };
    int ok;

    pkg = musicpack_package_open_dir(dir, 0);
    if (pkg == 0)
        return 0;
    ok = musicpack_package_verify(pkg, &rep, 0, 0) == MUSICPACK_OK;
    musicpack_package_close(pkg);
    return ok;
}

/* ------------------------------------------------------------------ */
/* codec + loudness                                                    */
/* ------------------------------------------------------------------ */

static const char *
codec_for_path(const char *path)
{
    const char *dot = strrchr(path, '.');
    if (dot == 0)
        return "?";
    if (strcmp(dot, ".mpc") == 0) return "musepack";
    if (strcmp(dot, ".flac") == 0) return "flac";
    if (strcmp(dot, ".wav") == 0) return "wav";
    if (strcmp(dot, ".ogg") == 0) return "ogg";
    return dot + 1;
}

/* Measures integrated loudness + true peak of an audio file. Returns 0 on
   success (has=1), 1 if loudness could not be measured.
   If `album` is non-NULL it holds a package-wide album meter (created lazily
   from the first measured track's format); the same PCM is fed to both the
   per-track meter and the album meter so album loudness is measured over the
   concatenated program, never aggregated from per-track values. */
static int
measure_loudness(const char *path, int *has, double *lufs, double *peak,
                 double *duration, musicpack_meter **album)
{
    musicpack_meter *meter = 0;
    int rc = 1;
    const char *codec = codec_for_path(path);

    *has = 0;
    *lufs = 0;
    *peak = 0;

    if (strcmp(codec, "musepack") == 0) {
        mpc_reader reader;
        musepack_decoder *dec;
        musepack_stream_info info;
        float pcm[1152 * 2];
        uint64_t frames;
        unsigned ch, rate;

        if (mpc_reader_init_stdio(&reader, path) != MPC_STATUS_OK)
            return 1;
        dec = musepack_decoder_open(&reader, 0);
        if (dec == 0) { mpc_reader_exit_stdio(&reader); return 1; }
        memset(&info, 0, sizeof info);
        info.size = sizeof info;
        musepack_decoder_get_stream_info(dec, &info);
        if (info.channels < 1 || info.channels > 2) {
            musepack_decoder_close(dec);
            mpc_reader_exit_stdio(&reader);
            return 1;
        }
        ch = info.channels;
        rate = info.sample_rate;
        if (rate == 0) {
            musepack_decoder_close(dec);
            mpc_reader_exit_stdio(&reader);
            return 1;
        }
        if (duration != 0)
            *duration = (double) musepack_decoder_length_samples(dec) / (double) rate;
        meter = musicpack_meter_new(ch, rate, 0);
        if (meter != 0) {
            if (album != 0 && *album == 0)
                *album = musicpack_meter_new(ch, rate, 0);
            while (musepack_decoder_read(dec, pcm, 1152, &frames) == MUSEPACK_OK) {
                musicpack_meter_add_frames(meter, pcm, frames);
                if (album != 0 && *album != 0)
                    musicpack_meter_add_frames(*album, pcm, frames);
            }
            rc = 0;
        }
        musepack_decoder_close(dec);
        mpc_reader_exit_stdio(&reader);
    } else {
        /* decode via ffmpeg to interleaved f32le stereo 44.1k */
        char cmd[4096];
        FILE *pipe;
        float buf[8192];
        size_t n;
        double total_frames = 0;

        meter = musicpack_meter_new(2, 44100, 0);
        if (meter == 0)
            return 1;
        if (album != 0 && *album == 0)
            *album = musicpack_meter_new(2, 44100, 0);
        snprintf(cmd, sizeof cmd,
                 "ffmpeg -v error -i '%s' -f f32le -ac 2 -ar 44100 - 2>/dev/null",
                 path);
        pipe = POPEN(cmd, POPEN_MODE);
        if (pipe == 0)
            goto out;
        while ((n = fread(buf, sizeof(float), sizeof buf / sizeof(float), pipe)) > 0) {
            musicpack_meter_add_frames(meter, buf, n / 2);
            if (album != 0 && *album != 0)
                musicpack_meter_add_frames(*album, buf, n / 2);
            total_frames += n / 2;
        }
        if (PCLOSE(pipe) != 0)
            goto out;
        if (duration != 0)
            *duration = total_frames / 44100.0;
        rc = 0;
    }

    if (rc == 0 && meter != 0) {
        if (musicpack_meter_result(meter, lufs, peak) != MUSICPACK_OK)
            rc = 1;
        else
            *has = 1;
    }
out:
    musicpack_meter_free(meter);
    return rc;
}

/* ------------------------------------------------------------------ */
/* command: info                                                       */
/* ------------------------------------------------------------------ */

/* Sonic analysis summary for `info`: reads the referenced sonic document
   (if any) and reports its shape without validating the whole package. */
typedef struct {
    int present;   /* a sonic document was found and parsed */
    int failed;    /* referenced but unreadable/unparseable */
    int format;
    char *profile;
    musicpack_sonic_profile_state profile_state;
    size_t track_count;
    size_t present_count;
    int album_present;
} sonic_summary;

static void
sonic_summary_free(sonic_summary *ss)
{
    free(ss->profile);
}

static void
load_sonic_summary(const musicpack_package *pkg, const musicpack_manifest *m,
                   sonic_summary *ss)
{
    size_t i;
    char abs[MUSICPACK_PATH_MAX + 2];
    char *json;
    musicpack_status s;

    memset(ss, 0, sizeof *ss);
    ss->format = -1;
    for (i = 0; i < m->analysis_count; i++) {
        musicpack_sonic *sonic;
        size_t t;
        if (strcmp(m->analysis[i].type, "sonic") != 0)
            continue;
        if (musicpack_package_resolve_path(pkg, m->analysis[i].asset.path,
                                           abs, sizeof abs) != MUSICPACK_OK)
            break;
        json = read_file_bounded(abs, MUSICPACK_SONIC_DOC_MAX, &s);
        if (json == 0) {
            ss->failed = 1;
            break;
        }
        sonic = musicpack_sonic_parse(json, strlen(json), &s);
        free(json);
        if (sonic == 0) {
            ss->failed = 1;
            break;
        }
        ss->present = 1;
        ss->format = MUSICPACK_SONIC_VERSION;
        ss->profile = strdup(sonic->profile_id);
        musicpack_sonic_validate(sonic, 0, &ss->profile_state);
        ss->track_count = sonic->track_count;
        for (t = 0; t < sonic->track_count; t++)
            if (sonic->tracks[t].embedding.present)
                ss->present_count++;
        ss->album_present = sonic->album.present;
        musicpack_sonic_free(sonic);
        break;
    }
}

static void
print_track(const musicpack_track *t)
{
    printf("  Track %d: %s\n", t->number, t->title);
    printf("    audio: %s (%s)", t->audio.path, codec_for_path(t->audio.path));
    if (t->has_duration)
        printf(", %.1fs", t->duration);
    if (t->audio.sha256 != 0)
        printf(", sha256 %s", t->audio.sha256);
    printf("\n");
    if (t->loudness.present)
        printf("    loudness: %.1f LUFS, %.1f dBTP\n", t->loudness.lufs,
               t->loudness.true_peak_db);
}

/* Medium format summary: "CD", "CD x 2", "CD + Digital". NULL when no medium
   carries a format. */
static const char *
medium_format_display(const musicpack_manifest *m, char *buf, size_t cap)
{
    char seen[8][32];
    size_t seen_count = 0, d, s;

    for (d = 0; d < m->disc_count; d++) {
        const char *f = m->discs[d].format;
        int dup = 0;
        if (f == 0)
            continue;
        for (s = 0; s < seen_count; s++)
            if (strcmp(seen[s], f) == 0) { dup = 1; break; }
        if (!dup && seen_count < sizeof seen / sizeof *seen)
            snprintf(seen[seen_count++], sizeof seen[0], "%s", f);
    }
    if (seen_count == 0)
        return 0;
    if (seen_count == 1)
        snprintf(buf, cap, "%s x %zu", seen[0], m->disc_count);
    else {
        size_t n = 0;
        buf[0] = '\0';
        for (s = 0; s < seen_count; s++) {
            int k = snprintf(buf + n, cap - n, "%s%s", s > 0 ? " + " : "", seen[s]);
            if (k < 0 || (size_t) k >= cap - n)
                break;
            n += (size_t) k;
        }
    }
    return buf;
}

/* Dominant codec across the package: single display name when all tracks
   share a codec, NULL otherwise (unknown or mixed). */
static const char *
package_codec(const musicpack_manifest *m)
{
    size_t d, t, n = 0;
    const char *first = 0;

    for (d = 0; d < m->disc_count; d++)
        for (t = 0; t < m->discs[d].track_count; t++) {
            const char *c = codec_for_path(m->discs[d].tracks[t].audio.path);
            if (n == 0)
                first = c;
            else if (strcmp(first, c) != 0)
                return 0;
            n++;
        }
    if (n == 0 || first == 0)
        return 0;
    if (strcmp(first, "musepack") == 0) return "Musepack SV8";
    if (strcmp(first, "flac") == 0) return "FLAC";
    if (strcmp(first, "wav") == 0) return "WAV";
    if (strcmp(first, "ogg") == 0) return "Ogg Vorbis";
    return first;
}

static int
cmd_info(const char *dir, int json)
{
    musicpack_package *pkg;
    const musicpack_manifest *m;
    musicpack_report rep = { 0, 0 };
    sonic_summary ss;
    musicpack_status s;
    size_t d, t, i, track_total = 0;

    pkg = musicpack_package_open_dir(dir, &s);
    if (pkg == 0) {
        if (json)
            json_error_out("not_found", "cannot open package");
        else
            fprintf(stderr, "cannot open package '%s' (error %d)\n", dir, (int) s);
        return 1;
    }
    m = musicpack_package_manifest(pkg);
    load_sonic_summary(pkg, m, &ss);

    if (json) {
        cJSON *root = cJSON_CreateObject();
        cJSON *o, *arr, *item, *so;
        char buf[128];

        cJSON_AddStringToObject(root, "package", dir);
        o = cJSON_AddObjectToObject(root, "album");
        cJSON_AddStringToObject(o, "title", m->album_title);
        {
            cJSON *arts = cJSON_AddArrayToObject(o, "artists");
            for (i = 0; i < m->album_artist_count; i++) {
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "name", m->album_artists[i].name);
                if (m->album_artists[i].role != 0)
                    cJSON_AddStringToObject(a, "role", m->album_artists[i].role);
                cJSON_AddItemToArray(arts, a);
            }
        }
        if (m->release_type != 0)
            cJSON_AddStringToObject(o, "releaseType", m->release_type);
        if (m->original_release_date != 0)
            cJSON_AddStringToObject(o, "originalReleaseDate", m->original_release_date);
        if (m->genre_count > 0) {
            arr = cJSON_AddArrayToObject(o, "genres");
            for (i = 0; i < m->genre_count; i++)
                cJSON_AddItemToArray(arr, cJSON_CreateString(m->genres[i]));
        }
        if (m->release.present) {
            o = cJSON_AddObjectToObject(root, "release");
            if (m->release.release_date != 0)
                cJSON_AddStringToObject(o, "releaseDate", m->release.release_date);
            if (m->release.edition != 0)
                cJSON_AddStringToObject(o, "edition", m->release.edition);
            if (m->release.country != 0)
                cJSON_AddStringToObject(o, "country", m->release.country);
            if (m->release.label != 0)
                cJSON_AddStringToObject(o, "label", m->release.label);
            if (m->release.catalogue_number != 0)
                cJSON_AddStringToObject(o, "catalogueNumber", m->release.catalogue_number);
            if (m->release.notes != 0)
                cJSON_AddStringToObject(o, "notes", m->release.notes);
        }
        {
            const char *fmt = medium_format_display(m, buf, sizeof buf);
            if (fmt != 0)
                cJSON_AddStringToObject(root, "medium", fmt);
        }
        if (m->barcode != 0)
            cJSON_AddStringToObject(root, "barcode", m->barcode);
        if (m->identity_source != 0 || m->identity_confidence != 0) {
            o = cJSON_AddObjectToObject(root, "identity");
            if (m->identity_source != 0)
                cJSON_AddStringToObject(o, "source", m->identity_source);
            if (m->identity_confidence != 0)
                cJSON_AddStringToObject(o, "confidence", m->identity_confidence);
        }
        if (m->musicbrainz_release_group_id != 0)
            cJSON_AddStringToObject(root, "musicbrainzReleaseGroupId",
                                    m->musicbrainz_release_group_id);
        if (m->musicbrainz_release_id != 0)
            cJSON_AddStringToObject(root, "musicbrainzReleaseId", m->musicbrainz_release_id);
        if (m->source_type != 0 || m->source_store != 0) {
            o = cJSON_AddObjectToObject(root, "source");
            if (m->source_type != 0)
                cJSON_AddStringToObject(o, "type", m->source_type);
            if (m->source_store != 0)
                cJSON_AddStringToObject(o, "store", m->source_store);
        }
        {
            const char *codec = package_codec(m);
            if (codec != 0)
                cJSON_AddStringToObject(root, "codec", codec);
        }

        arr = cJSON_AddArrayToObject(root, "discs");
        for (d = 0; d < m->disc_count; d++) {
            cJSON *disc = cJSON_CreateObject();
            cJSON_AddNumberToObject(disc, "disc", m->discs[d].disc);
            item = cJSON_AddArrayToObject(disc, "tracks");
            for (t = 0; t < m->discs[d].track_count; t++) {
                const musicpack_track *tr = &m->discs[d].tracks[t];
                cJSON *to = cJSON_CreateObject();
                cJSON_AddNumberToObject(to, "track", tr->number);
                cJSON_AddStringToObject(to, "title", tr->title);
                if (tr->has_duration)
                    cJSON_AddNumberToObject(to, "duration", tr->duration);
                cJSON_AddStringToObject(to, "audio", tr->audio.path);
                if (tr->loudness.present) {
                    cJSON *lo = cJSON_AddObjectToObject(to, "loudness");
                    cJSON_AddNumberToObject(lo, "trackLUFS", tr->loudness.lufs);
                    cJSON_AddNumberToObject(lo, "truePeakDbTP", tr->loudness.true_peak_db);
                }
                cJSON_AddItemToArray(item, to);
            }
            cJSON_AddItemToArray(arr, disc);
        }
        for (d = 0; d < m->disc_count; d++)
            for (t = 0; t < m->discs[d].track_count; t++)
                track_total++;
        cJSON_AddNumberToObject(root, "totalTracks", (double) track_total);
        if (m->has_album_loudness) {
            o = cJSON_AddObjectToObject(root, "albumLoudness");
            cJSON_AddNumberToObject(o, "albumLUFS", m->album_loudness.lufs);
            cJSON_AddNumberToObject(o, "albumTruePeakDbTP", m->album_loudness.true_peak_db);
            if (m->loudness_algorithm != 0)
                cJSON_AddStringToObject(o, "algorithm", m->loudness_algorithm);
        }
        if (m->provenance_tool != 0 || m->provenance_tool_version != 0) {
            o = cJSON_AddObjectToObject(root, "provenance");
            if (m->provenance_tool != 0)
                cJSON_AddStringToObject(o, "tool", m->provenance_tool);
            if (m->provenance_tool_version != 0)
                cJSON_AddStringToObject(o, "toolVersion", m->provenance_tool_version);
        }

        s = musicpack_package_verify(pkg, &rep, 0, 0);
        o = cJSON_AddObjectToObject(root, "integrity");
        cJSON_AddBoolToObject(o, "ok", s == MUSICPACK_OK ? 1 : 0);
        cJSON_AddNumberToObject(o, "errors", (double) rep.errors);
        cJSON_AddNumberToObject(o, "warnings", (double) rep.warnings);

        if (ss.present) {
            so = cJSON_AddObjectToObject(root, "sonic");
            cJSON_AddNumberToObject(so, "format", (double) ss.format);
            cJSON_AddStringToObject(so, "profile", ss.profile);
            cJSON_AddNumberToObject(so, "tracks", (double) ss.track_count);
            cJSON_AddNumberToObject(so, "tracksWithEmbedding", (double) ss.present_count);
            cJSON_AddBoolToObject(so, "albumEmbedding", ss.album_present ? 1 : 0);
            cJSON_AddStringToObject(so, "profileState",
                                    ss.profile_state == MUSICPACK_SONIC_PROFILE_SUPPORTED
                                        ? "supported"
                                        : ss.profile_state == MUSICPACK_SONIC_PROFILE_RESERVED
                                              ? "reserved"
                                              : "unknown");
        } else {
            cJSON_AddNullToObject(root, "sonic");
        }
        draft_print(root);
        cJSON_Delete(root);
        musicpack_package_close(pkg);
        sonic_summary_free(&ss);
        return s == MUSICPACK_OK ? 0 : 1;
    }

    printf("Package: %s\n", dir);
    printf("Album: %s\n", m->album_title);
    for (i = 0; i < m->album_artist_count; i++) {
        if (m->album_artists[i].role != 0)
            printf("Artist: %s (%s)\n", m->album_artists[i].name, m->album_artists[i].role);
        else
            printf("Artist: %s\n", m->album_artists[i].name);
    }
    if (m->release_type != 0)
        printf("Type: %s\n", m->release_type);
    if (m->release.edition != 0)
        printf("Edition: %s\n", m->release.edition);
    if (m->release.release_date != 0)
        printf("Release date: %s\n", m->release.release_date);
    if (m->original_release_date != 0)
        printf("Original release: %s\n", m->original_release_date);
    if (m->release.country != 0)
        printf("Country: %s\n", m->release.country);
    if (m->release.label != 0)
        printf("Label: %s\n", m->release.label);
    if (m->release.catalogue_number != 0)
        printf("Catalogue: %s\n", m->release.catalogue_number);
    {
        char buf[128];
        const char *fmt = medium_format_display(m, buf, sizeof buf);
        if (fmt != 0)
            printf("Medium: %s\n", fmt);
    }
    if (m->barcode != 0)
        printf("Barcode: %s\n", m->barcode);
    if (m->identity_source != 0 || m->identity_confidence != 0)
        printf("Identity: %s%s%s\n",
               m->identity_source != 0 ? m->identity_source : "unknown",
               m->identity_confidence != 0 ? " " : "",
               m->identity_confidence != 0 ? m->identity_confidence : "");
    if (m->musicbrainz_release_group_id != 0)
        printf("MusicBrainz release group: %s\n", m->musicbrainz_release_group_id);
    if (m->musicbrainz_release_id != 0)
        printf("MusicBrainz release: %s\n", m->musicbrainz_release_id);
    if (m->source_type != 0 || m->source_store != 0)
        printf("Source: %s%s%s%s\n",
               m->source_type != 0 ? m->source_type : "unknown",
               m->source_store != 0 ? " (" : "",
               m->source_store != 0 ? m->source_store : "",
               m->source_store != 0 ? ")" : "");
    if (m->genre_count > 0) {
        printf("Genres:");
        for (i = 0; i < m->genre_count; i++)
            printf(" %s", m->genres[i]);
        printf("\n");
    }
    {
        const char *codec = package_codec(m);
        if (codec != 0)
            printf("Codec: %s\n", codec);
    }

    printf("Discs: %zu\n", m->disc_count);
    for (d = 0; d < m->disc_count; d++) {
        printf("Disc %d: %zu tracks\n", m->discs[d].disc, m->discs[d].track_count);
        for (t = 0; t < m->discs[d].track_count; t++) {
            print_track(&m->discs[d].tracks[t]);
            track_total++;
        }
    }
    printf("Total tracks: %zu\n", track_total);

    if (m->has_album_loudness) {
        printf("Album loudness: %.1f LUFS, %.1f dBTP",
               m->album_loudness.lufs, m->album_loudness.true_peak_db);
        if (m->loudness_algorithm != 0)
            printf(" (%s)", m->loudness_algorithm);
        printf("\n");
    }
    if (m->provenance_tool != 0)
        printf("Provenance: %s %s\n", m->provenance_tool,
               m->provenance_tool_version != 0 ? m->provenance_tool_version : "");

    if (ss.present) {
        printf("Sonic Analysis:\n");
        printf("  format: %d\n", ss.format);
        printf("  profile: %s\n", ss.profile);
        printf("  tracks: %zu/%zu\n", ss.present_count, ss.track_count);
        printf("  album embedding: %s\n", ss.album_present ? "yes" : "no");
        if (ss.profile_state != MUSICPACK_SONIC_PROFILE_SUPPORTED)
            printf("  profile: not supported for comparison\n");
    } else if (ss.failed) {
        printf("Sonic Analysis: referenced document is unreadable or malformed\n");
    }

    s = musicpack_package_verify(pkg, &rep, 0, 0);
    printf("Integrity: %s (%zu errors, %zu warnings)\n",
           s == MUSICPACK_OK ? "OK" : "FAILED", rep.errors, rep.warnings);

    musicpack_package_close(pkg);
    sonic_summary_free(&ss);
    return s == MUSICPACK_OK ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* command: verify                                                     */
/* ------------------------------------------------------------------ */

static void
verify_report(void *ctx, const char *message, int is_error)
{
    (void) ctx;
    printf("%s%s\n", is_error ? "error: " : "warning: ", message);
}

typedef struct report_bag {
    char **errors;
    size_t errors_count, errors_cap;
    char **warnings;
    size_t warnings_count, warnings_cap;
} report_bag;

static void
collect_report(void *ctx, const char *message, int is_error)
{
    report_bag *b = (report_bag *) ctx;
    char ***arr = is_error ? &b->errors : &b->warnings;
    size_t *n = is_error ? &b->errors_count : &b->warnings_count;
    size_t *cap = is_error ? &b->errors_cap : &b->warnings_cap;
    char **na;
    if (*n >= *cap) {
        size_t nc = *cap == 0 ? 8 : *cap * 2;
        na = (char **) realloc(*arr, nc * sizeof **arr);
        if (na == 0)
            return;
        *arr = na;
        *cap = nc;
    }
    (*arr)[*n] = strdup(message);
    if ((*arr)[*n] != 0)
        (*n)++;
}

static void
report_bag_free(report_bag *b)
{
    size_t i;
    for (i = 0; i < b->errors_count; i++)
        free(b->errors[i]);
    for (i = 0; i < b->warnings_count; i++)
        free(b->warnings[i]);
    free(b->errors);
    free(b->warnings);
}

/* defined later (authoring-draft section) */
static void json_error_out(const char *code, const char *msg);
static void add_string_array(cJSON *root, const char *key, char *const *items, size_t n);

static int
cmd_verify(const char *dir, int quiet, int json)
{
    musicpack_package *pkg;
    musicpack_report rep = { 0, 0 };
    musicpack_status s;
    report_bag bag;

    memset(&bag, 0, sizeof bag);
    pkg = musicpack_package_open_dir(dir, 0);
    if (pkg == 0) {
        if (json)
            json_error_out("not_found", "cannot open package");
        else
            fprintf(stderr, "cannot open package '%s'\n", dir);
        return 1;
    }
    s = musicpack_package_verify(pkg, &rep, json ? collect_report : verify_report,
                                 json ? (void *) &bag : 0);
    if (json) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", s == MUSICPACK_OK ? 1 : 0);
        add_string_array(root, "errors", bag.errors, bag.errors_count);
        add_string_array(root, "warnings", bag.warnings, bag.warnings_count);
        draft_print(root);
        cJSON_Delete(root);
    } else if (!quiet) {
        printf("verify: %zu error(s), %zu warning(s)\n", rep.errors, rep.warnings);
    }
    report_bag_free(&bag);
    musicpack_package_close(pkg);
    return s == MUSICPACK_OK ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* command: identify                                                   */
/* ------------------------------------------------------------------ */

static int
all_digits(const char *s)
{
    if (s == 0 || *s == '\0')
        return 0;
    for (; *s != '\0'; s++)
        if (*s < '0' || *s > '9')
            return 0;
    return 1;
}

static int
valid_uuid(const char *s)
{
    size_t n = strlen(s), i;
    if (n != 36)
        return 0;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-')
                return 0;
        } else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
                     (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

static char *
read_file_bounded(const char *path, size_t max, musicpack_status *status)
{
    FILE *f;
    long len;
    char *buf;

    f = fopen(path, "rb");
    if (f == 0) {
        *status = MUSICPACK_ERR_IO;
        return 0;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); *status = MUSICPACK_ERR_IO; return 0; }
    len = ftell(f);
    if (len < 0 || (size_t) len > max) { fclose(f); *status = MUSICPACK_ERR_INVALID; return 0; }
    if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); *status = MUSICPACK_ERR_IO; return 0; }
    buf = (char *) malloc((size_t) len + 1);
    if (buf == 0) { fclose(f); *status = MUSICPACK_ERR_NOMEM; return 0; }
    if (len > 0 && fread(buf, 1, (size_t) len, f) != (size_t) len) {
        free(buf);
        fclose(f);
        *status = MUSICPACK_ERR_IO;
        return 0;
    }
    fclose(f);
    buf[len] = '\0';
    *status = MUSICPACK_OK;
    return buf;
}

/* Fetches a URL into a NUL-terminated buffer (bounded). The URL is built
   only from validated UUIDs / digit barcodes, so no shell metacharacters
   can reach the command. Returns NULL on failure. */
static char *
curl_fetch(const char *url, size_t max)
{
    char cmd[4096];
    FILE *pipe;
    char *buf;
    size_t cap = 65536, len = 0;

    if (snprintf(cmd, sizeof cmd,
                 "curl -s -m 30 --max-filesize %zu -A \"musicpack/%s (https://musicpack.dev)\" \"%s\"",
                 max, MUSICPACK_VERSION, url) >= (int) sizeof cmd)
        return 0;
    pipe = POPEN(cmd, POPEN_MODE);
    if (pipe == 0)
        return 0;
    buf = (char *) malloc(cap);
    if (buf == 0) {
        PCLOSE(pipe);
        return 0;
    }
    for (;;) {
        size_t n;
        if (len + 65536 + 1 > cap) {
            char *nb = (char *) realloc(buf, cap * 2);
            if (nb == 0)
                break;
            buf = nb;
            cap *= 2;
        }
        n = fread(buf + len, 1, 65536, pipe);
        len += n;
        if (n < 65536)
            break;
    }
    PCLOSE(pipe);
    buf[len] = '\0';
    return buf;
}

static void
usage_identify(void)
{
    fprintf(stderr,
        "usage: musicpack identify <package> [--mb-json FILE]\n");
}

static int
cmd_identify(const char *dir, const char *mb_json_path)
{
    musicpack_package *pkg;
    musicpack_manifest *m;
    musicpack_status s;
    const char *conf = "none";
    int changed = 0;

    pkg = musicpack_package_open_dir(dir, &s);
    if (pkg == 0) {
        fprintf(stderr, "cannot open package '%s' (error %d)\n", dir, (int) s);
        return 1;
    }
    m = musicpack_package_manifest_mutable(pkg);

    if (mb_json_path != 0) {
        char *json = read_file_bounded(mb_json_path, 8u * 1024u * 1024u, &s);
        if (json == 0) {
            fprintf(stderr, "cannot read '%s'\n", mb_json_path);
            musicpack_package_close(pkg);
            return 1;
        }
        conf = musicpack_mb_match_confidence(json, m);
        if (strcmp(conf, "none") != 0) {
            musicpack_mb_apply_release(json, m);
            changed = 1;
        }
        free(json);
    } else if (m->musicbrainz_release_id != 0 &&
               valid_uuid(m->musicbrainz_release_id)) {
        char url[512];
        char *json;
        snprintf(url, sizeof url,
                 "https://musicbrainz.org/ws/2/release/%s?inc=artist-credits+labels+recordings+media&fmt=json",
                 m->musicbrainz_release_id);
        json = curl_fetch(url, 1u * 1024u * 1024u);
        if (json == 0) {
            fprintf(stderr, "identify: network lookup failed; identity unchanged\n");
        } else {
            conf = musicpack_mb_match_confidence(json, m);
            if (strcmp(conf, "none") != 0) {
                musicpack_mb_apply_release(json, m);
                changed = 1;
            }
            free(json);
        }
    } else if (m->barcode != 0 && all_digits(m->barcode)) {
        char url[512];
        char *json;
        snprintf(url, sizeof url,
                 "https://musicbrainz.org/ws/2/release/?query=barcode:%s&fmt=json&limit=5",
                 m->barcode);
        json = curl_fetch(url, 1u * 1024u * 1024u);
        if (json == 0) {
            fprintf(stderr, "identify: network lookup failed; identity unchanged\n");
        } else {
            conf = musicpack_mb_match_confidence(json, m);
            if (strcmp(conf, "none") != 0) {
                musicpack_mb_apply_release(json, m);
                changed = 1;
            }
            free(json);
        }
    } else {
        fprintf(stderr,
                "identify: no MusicBrainz release id or barcode to match;\n"
                "         use --mb-json with a release document to apply offline\n");
    }

    if (changed) {
        if (m->identity_source == 0)
            m->identity_source = strdup("musicbrainz");
        if (m->identity_confidence == 0)
            m->identity_confidence = strdup(conf);
        if (musicpack_package_save_manifest(pkg) != MUSICPACK_OK) {
            fprintf(stderr, "identify: cannot save manifest\n");
            musicpack_package_close(pkg);
            return 1;
        }
        printf("identify: %s\n", conf);
    } else {
        printf("identify: no match applied\n");
    }

    musicpack_package_close(pkg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* command: update-metadata                                            */
/* ------------------------------------------------------------------ */

static void
usage_update_metadata(void)
{
    fprintf(stderr,
        "usage: musicpack update-metadata <package> [--sync-tags]\n"
        "       --sync-tags: rewrite APEv2 tags on .mpc tracks from the\n"
        "       manifest and refresh their checksums\n");
}

/* Reads embedded tags from a package audio file (FLAC Vorbis / MPC APEv2). */
static int
read_package_track_tags(const char *apath, musicpack_tag_set *tags)
{
    const char *dot = strrchr(apath, '.');
    if (dot == 0)
        return 0;
    if (strcmp(dot, ".flac") == 0)
        return musicpack_flac_read_metadata(apath, tags, 0) == MUSICPACK_OK;
    if (strcmp(dot, ".mpc") == 0)
        return musicpack_ape_read(apath, tags) == MUSICPACK_OK;
    return 0;
}

static int
cmd_update_metadata(const char *dir, int sync_tags)
{
    musicpack_package *pkg;
    musicpack_manifest *m;
    musicpack_status s;
    size_t d, t;
    int album_done = 0, hash_changed = 0, reconciled = 0;

    pkg = musicpack_package_open_dir(dir, &s);
    if (pkg == 0) {
        fprintf(stderr, "cannot open package '%s'\n", dir);
        return 1;
    }
    m = musicpack_package_manifest_mutable(pkg);

    /* album-level: fill empty fields from the first audio track's tags */
    for (d = 0; d < m->disc_count && !album_done; d++)
        for (t = 0; t < m->discs[d].track_count && !album_done; t++) {
            char apath[MUSICPACK_PATH_MAX + 2];
            musicpack_tag_set tags;
            if (musicpack_package_track_path(pkg, d, t, apath, sizeof apath)
                != MUSICPACK_OK)
                continue;
            memset(&tags, 0, sizeof tags);
            if (read_package_track_tags(apath, &tags)) {
                if (musicpack_tag_map_album(&tags, m) == MUSICPACK_OK)
                    reconciled = 1;
                album_done = 1;
            }
            musicpack_tag_set_free(&tags);
        }

    /* per-track reconcile, then optional manifest -> APEv2 re-projection */
    for (d = 0; d < m->disc_count; d++) {
        musicpack_disc *disc = &m->discs[d];
        for (t = 0; t < disc->track_count; t++) {
            musicpack_track *tr = &disc->tracks[t];
            char apath[MUSICPACK_PATH_MAX + 2];
            musicpack_tag_set tags;
            const char *dot;

            if (musicpack_package_track_path(pkg, d, t, apath, sizeof apath)
                != MUSICPACK_OK)
                continue;
            memset(&tags, 0, sizeof tags);
            if (read_package_track_tags(apath, &tags)) {
                if (musicpack_tag_map_track(&tags, tr) == MUSICPACK_OK)
                    reconciled = 1;
            }
            musicpack_tag_set_free(&tags);

            dot = strrchr(apath, '.');
            if (sync_tags && dot != 0 && strcmp(dot, ".mpc") == 0) {
                musicpack_tag_set ape;
                char hex[MUSICPACK_SHA256_HEX_SIZE];
                if (musicpack_manifest_to_ape_tags(m, tr, disc->disc,
                                                   (int) m->disc_count,
                                                   (int) disc->track_count,
                                                   &ape) == MUSICPACK_OK) {
                    if (musicpack_ape_write(apath, &ape) == MUSICPACK_OK &&
                        musicpack_sha256_file(apath, hex, sizeof hex)
                            == MUSICPACK_OK) {
                        free(tr->audio.sha256);
                        tr->audio.sha256 = strdup(hex);
                        hash_changed = 1;
                    } else {
                        fprintf(stderr,
                                "update-metadata: cannot re-tag '%s'\n",
                                tr->audio.path);
                    }
                    musicpack_tag_set_free(&ape);
                }
            } else if (sync_tags && dot != 0 && strcmp(dot, ".flac") == 0) {
                fprintf(stderr,
                        "update-metadata: --sync-tags only writes APEv2 (.mpc); "
                        "skipping '%s'\n", tr->audio.path);
            }
        }
    }

    if (reconciled || hash_changed) {
        if (musicpack_package_save_manifest(pkg) != MUSICPACK_OK) {
            fprintf(stderr, "update-metadata: cannot save manifest\n");
            musicpack_package_close(pkg);
            return 1;
        }
        printf("update-metadata: manifest updated (%s)\n",
               hash_changed ? "tags synced and checksums refreshed" : "reconciled");
    } else {
        printf("update-metadata: no changes\n");
    }

    musicpack_package_close(pkg);
    return 0;
}

/* ------------------------------------------------------------------ */
/* command: create                                                     */
/* ------------------------------------------------------------------ */

typedef struct {
    char *path;
    char *title;
    int has_title;
} create_track;

static void
usage_create(void)
{
    fprintf(stderr,
        "usage: musicpack create -o <dir> -t TITLE [-a ARTIST]...\n"
        "       [-d RELEASE_DATE] [-R RELEASE_TYPE] [-O ORIGINAL_RELEASE_DATE]\n"
        "       [-e EDITION] [-l LABEL] [-c CATALOGUE] [-C COUNTRY]\n"
        "       [-m MEDIUM_FORMAT] [-N NOTES] [-T FILE [-n TRACK_TITLE]]... [-A ARTWORK]\n");
}

static int
cmd_create(int argc, char **argv)
{
    const char *out_dir = 0, *title = 0, *release_date = 0, *artwork = 0;
    const char *final_dir;
    char stage_dir[MUSICPACK_PATH_MAX + 2];
    const char *release_type = 0, *orig_release_date = 0, *edition = 0;
    const char *label = 0, *catalogue = 0, *country = 0, *medium_format = 0;
    const char *notes = 0;
    create_track tracks[256];
    char *artists[64];
    size_t artist_count = 0, track_count = 0;
    musicpack_meter *album_meter = 0;
    int c;

    for (c = 0; c < (int) (sizeof tracks / sizeof *tracks); c++)
        memset(&tracks[c], 0, sizeof tracks[c]);

    while ((c = getopt(argc, argv, "o:t:a:d:R:O:e:l:c:C:m:N:T:n:A:")) != -1) {
        switch (c) {
        case 'o': out_dir = optarg; break;
        case 't': title = optarg; break;
        case 'a':
            if (artist_count >= sizeof artists / sizeof *artists)
                return usage_error("too many artists");
            artists[artist_count++] = optarg;
            break;
        case 'd': release_date = optarg; break;
        case 'R': release_type = optarg; break;
        case 'O': orig_release_date = optarg; break;
        case 'e': edition = optarg; break;
        case 'l': label = optarg; break;
        case 'c': catalogue = optarg; break;
        case 'C': country = optarg; break;
        case 'm': medium_format = optarg; break;
        case 'N': notes = optarg; break;
        case 'T':
            if (track_count >= sizeof tracks / sizeof *tracks)
                return usage_error("too many tracks");
            tracks[track_count].path = optarg;
            track_count++;
            break;
        case 'n':
            if (track_count == 0)
                return usage_error("--track-title must follow --track");
            tracks[track_count - 1].title = optarg;
            tracks[track_count - 1].has_title = 1;
            break;
        case 'A': artwork = optarg; break;
        default:
            usage_create();
            return 2;
        }
    }
    if (out_dir == 0 || title == 0 || artist_count == 0 || track_count == 0) {
        usage_create();
        return 2;
    }
    final_dir = out_dir;
    if (!prepare_stage(final_dir, "create", stage_dir, sizeof stage_dir)) {
        fprintf(stderr, "create: output or staging destination already exists\n");
        return 1;
    }
    out_dir = stage_dir;

    /* build the model */
    {
        musicpack_manifest m;
        char audio_dir[MUSICPACK_PATH_MAX + 2];
        char art_dir[MUSICPACK_PATH_MAX + 2];
        musicpack_disc *disc;
        char hex[MUSICPACK_SHA256_HEX_SIZE];
        size_t i;
        int bad = 0;

        memset(&m, 0, sizeof m);
        m.album_title = strdup(title);
        m.album_artists = (musicpack_artist *) calloc(artist_count, sizeof *m.album_artists);
        for (i = 0; i < artist_count; i++) {
            m.album_artists[i].name = strdup(artists[i]);
        }
        m.album_artist_count = artist_count;
        if (release_type != 0)
            m.release_type = strdup(release_type);
        if (orig_release_date != 0)
            m.original_release_date = strdup(orig_release_date);
        if (release_date != 0) {
            m.release.present = 1;
            m.release.release_date = strdup(release_date);
        }
        if (edition != 0) { m.release.present = 1; m.release.edition = strdup(edition); }
        if (label != 0) { m.release.present = 1; m.release.label = strdup(label); }
        if (catalogue != 0) { m.release.present = 1; m.release.catalogue_number = strdup(catalogue); }
        if (country != 0) { m.release.present = 1; m.release.country = strdup(country); }
        if (notes != 0) { m.release.present = 1; m.release.notes = strdup(notes); }

        m.discs = (musicpack_disc *) calloc(1, sizeof *m.discs);
        m.disc_count = 1;
        disc = &m.discs[0];
        disc->disc = 1;
        if (medium_format != 0)
            disc->format = strdup(medium_format);
        disc->tracks = (musicpack_track *) calloc(track_count, sizeof *disc->tracks);
        disc->track_count = track_count;

        snprintf(audio_dir, sizeof audio_dir, "%s/audio", out_dir);
        snprintf(art_dir, sizeof art_dir, "%s/artwork", out_dir);
        if (mkdir_p(out_dir) != 0 || mkdir_p(audio_dir) != 0 || mkdir_p(art_dir) != 0) {
            fprintf(stderr, "cannot create package directory '%s'\n", out_dir);
            rm_rf(out_dir);
            return 1;
        }

        for (i = 0; i < track_count; i++) {
            musicpack_track *t = &disc->tracks[i];
            const char *base = strrchr(tracks[i].path, '/');
            const char *dot;
            char target[MUSICPACK_PATH_MAX + 2];

            base = base != 0 ? base + 1 : tracks[i].path;
            dot = strrchr(base, '.');
            {
                size_t stem_len = dot != 0 ? (size_t) (dot - base) : strlen(base);
                t->number = (int) i + 1;
                t->title = strdup(tracks[i].has_title ? tracks[i].title : "");
                if (!t->title || t->title[0] == '\0') {
                    free(t->title);
                    t->title = (char *) malloc(stem_len + 1);
                    memcpy(t->title, base, stem_len);
                    t->title[stem_len] = '\0';
                }
                snprintf(target, sizeof target, "%s/audio/%02d - %s%s",
                         out_dir, t->number, t->title, dot != 0 ? dot : "");
            }
            /* copy + hash */
            if (copy_file(tracks[i].path, target) != 0) {
                fprintf(stderr, "cannot copy '%s'\n", tracks[i].path);
                bad = 1;
                break;
            }
            t->audio.path = strdup(target + strlen(out_dir) + 1);
            if (musicpack_sha256_file(target, hex, sizeof hex) != MUSICPACK_OK) {
                fprintf(stderr, "cannot hash '%s'\n", target);
                bad = 1;
                break;
            }
            t->audio.sha256 = strdup(hex);
            /* duration + loudness (best effort); also feeds the album meter */
            {
                int has_l;
                double lufs, peak, dur = 0;
                if (measure_loudness(target, &has_l, &lufs, &peak, &dur,
                                     &album_meter) == 0) {
                    if (has_l) {
                        t->loudness.present = 1;
                        t->loudness.lufs = lufs;
                        t->loudness.true_peak_db = peak;
                    }
                    if (dur > 0) {
                        t->has_duration = 1;
                        t->duration = dur;
                    }
                }
            }
        }

        if (!bad && artwork != 0) {
            char target[MUSICPACK_PATH_MAX + 2];
            const char *ext = strrchr(artwork, '.');
            snprintf(target, sizeof target, "%s/artwork/front%s",
                     out_dir, ext != 0 ? ext : ".jpg");
            if (copy_file(artwork, target) != 0) {
                fprintf(stderr, "cannot copy artwork '%s'\n", artwork);
                bad = 1;
            } else {
                m.artwork = (musicpack_artwork *) calloc(1, sizeof *m.artwork);
                m.artwork_count = 1;
                m.artwork[0].role = strdup("front");
                m.artwork[0].asset.path = strdup(target + strlen(out_dir) + 1);
                if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                    m.artwork[0].asset.sha256 = strdup(hex);
            }
        }

        if (!bad && album_meter != 0) {
            double alufs, apeak;
            if (musicpack_meter_result(album_meter, &alufs, &apeak) == MUSICPACK_OK) {
                m.has_album_loudness = 1;
                m.album_loudness.lufs = alufs;
                m.album_loudness.true_peak_db = apeak;
                m.loudness_algorithm = strdup(MUSICPACK_LOUDNESS_STANDARD);
            }
        }

        if (!bad) {
            char *json = 0;
            if (musicpack_manifest_write(&m, &json) == MUSICPACK_OK) {
                char mpath[MUSICPACK_PATH_MAX + 2];
                snprintf(mpath, sizeof mpath, "%s/manifest.json", out_dir);
                if (write_all(mpath, json) != 0)
                    bad = 1;
                free(json);
            } else {
                bad = 1;
            }
        }

        /* free model */
        for (i = 0; i < m.album_artist_count; i++)
            free(m.album_artists[i].name);
        free(m.album_artists);
        free(m.album_title);
        free(m.release_type);
        free(m.original_release_date);
        free(m.release.release_date);
        free(m.release.edition);
        free(m.release.country);
        free(m.release.label);
        free(m.release.catalogue_number);
        free(m.release.notes);
        free(disc->format);
        free(m.loudness_algorithm);
        musicpack_meter_free(album_meter);
        for (i = 0; i < disc->track_count; i++) {
            free(disc->tracks[i].title);
            free(disc->tracks[i].audio.path);
            free(disc->tracks[i].audio.sha256);
        }
        free(disc->tracks);
        free(m.discs);
        for (i = 0; i < m.artwork_count; i++) {
            free(m.artwork[i].role);
            free(m.artwork[i].asset.path);
            free(m.artwork[i].asset.sha256);
        }
        free(m.artwork);

        if (bad) {
            rm_rf(out_dir);
            fprintf(stderr, "create failed\n");
            return 1;
        }
    }
    if (!verify_staged_package(out_dir) || rename(out_dir, final_dir) != 0) {
        rm_rf(out_dir);
        fprintf(stderr, "create failed\n");
        return 1;
    }
    printf("created package '%s'\n", final_dir);
    return 0;
}

/* ------------------------------------------------------------------ */
/* command: import                                                     */
/* ------------------------------------------------------------------ */

#if defined(_WIN32)
# include <io.h>
typedef struct _finddata_t finddata_t;
static int walk_push(char ***files, size_t *count, size_t *cap, const char *rel);
#endif

static int
walk_push(char ***files, size_t *count, size_t *cap, const char *rel)
{
    char *copy;
    if (*count >= *cap) {
        size_t newcap = *cap == 0 ? 64 : *cap * 2;
        char **nf = (char **) realloc(*files, newcap * sizeof *nf);
        if (nf == 0)
            return -1;
        *files = nf;
        *cap = newcap;
    }
    copy = strdup(rel);
    if (copy == 0)
        return -1;
    (*files)[(*count)++] = copy;
    return 0;
}

static void
walk_dir(const char *abs, const char *rel, char ***files, size_t *count, size_t *cap)
{
#if defined(_WIN32)
    char pat[MUSICPACK_PATH_MAX + 2];
    finddata_t fd;
    intptr_t h;

    snprintf(pat, sizeof pat, "%s/*", abs);
    h = _findfirst(pat, &fd);
    if (h == -1)
        return;
    do {
        char next[MUSICPACK_PATH_MAX + 2], relnext[MUSICPACK_PATH_MAX + 2];
        if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0)
            continue;
        snprintf(next, sizeof next, "%s/%s", abs, fd.name);
        if (fd.attrib & _A_SUBDIR) {
            if (rel[0] == '\0') snprintf(relnext, sizeof relnext, "%s", fd.name);
            else snprintf(relnext, sizeof relnext, "%s/%s", rel, fd.name);
            walk_dir(next, relnext, files, count, cap);
        } else {
            if (rel[0] == '\0') snprintf(relnext, sizeof relnext, "%s", fd.name);
            else snprintf(relnext, sizeof relnext, "%s/%s", rel, fd.name);
            walk_push(files, count, cap, relnext);
        }
    } while (_findnext(h, &fd) == 0);
    _findclose(h);
#else
    DIR *d = opendir(abs);
    struct dirent *e;
    if (d == 0)
        return;
    while ((e = readdir(d)) != 0) {
        char next[MUSICPACK_PATH_MAX + 2], relnext[MUSICPACK_PATH_MAX + 2];
        struct stat st;
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(next, sizeof next, "%s/%s", abs, e->d_name);
        if (lstat(next, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode)) {
            if (rel[0] == '\0') snprintf(relnext, sizeof relnext, "%s", e->d_name);
            else snprintf(relnext, sizeof relnext, "%s/%s", rel, e->d_name);
            walk_dir(next, relnext, files, count, cap);
        } else if (S_ISREG(st.st_mode)) {
            if (rel[0] == '\0') snprintf(relnext, sizeof relnext, "%s", e->d_name);
            else snprintf(relnext, sizeof relnext, "%s/%s", rel, e->d_name);
            walk_push(files, count, cap, relnext);
        }
    }
    closedir(d);
#endif
}

static int
is_audio_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (dot == 0)
        return 0;
    return strcmp(dot, ".mpc") == 0 || strcmp(dot, ".flac") == 0 ||
           strcmp(dot, ".wav") == 0 || strcmp(dot, ".ogg") == 0;
}

/* disc number from a directory name like "disc-2" / "CD 1" (0 if not a disc). */
static int
disc_from_dirname(const char *name)
{
    const char *p = name;
    int n = 0, digits = 0;
    if (strncmp(p, "disc", 4) != 0 && strncmp(p, "cd", 2) != 0)
        return 0;
    p += (strncmp(p, "disc", 4) == 0) ? 4 : 2;
    while (*p == '-' || *p == '_' || *p == ' ')
        p++;
    while (*p >= '0' && *p <= '9') {
        n = n * 10 + (*p - '0');
        digits++;
        p++;
    }
    return digits > 0 ? n : 0;
}

static void
split_segments(const char *rel, const char **first, const char **rest)
{
    const char *slash = strchr(rel, '/');
    if (slash != 0) {
        *first = rel;
        *rest = slash + 1;
    } else {
        *first = rel;
        *rest = 0;
    }
}

typedef struct {
    int disc;
    int number;
    char *src_rel;   /* relative to source root */
    char *title;     /* without extension or number prefix */
    char *ext;       /* original file extension incl. dot (".mpc") */
    musicpack_tag_set tags;  /* embedded metadata; empty when untagged */
    musicpack_pictures pics; /* embedded FLAC pictures; empty when none */
    int has_tags;
    char *lyric_path;        /* written lyrics asset (manifest-relative) */
    char *lyric_sha;
} import_track;

static int
cmp_import_tracks(const void *a, const void *b)
{
    const import_track *ta = (const import_track *) a;
    const import_track *tb = (const import_track *) b;
    int na = ta->number > 0 ? ta->number : 0x7fffffff;
    int nb = tb->number > 0 ? tb->number : 0x7fffffff;
    if (ta->disc != tb->disc)
        return ta->disc - tb->disc;
    if (na != nb)
        return na - nb;
    return strcmp(ta->src_rel, tb->src_rel);
}

static void
usage_import(void)
{
    fprintf(stderr,
        "usage: musicpack import -o <dir> [options] <source-dir>\n"
        "       options: -t TITLE  -a ARTIST  -L (skip loudness measurement)\n"
        "       release: -d RELEASE_DATE  -R RELEASE_TYPE  -O ORIGINAL_RELEASE_DATE\n"
        "                -e EDITION  -l LABEL  -c CATALOGUE  -C COUNTRY\n"
        "                -m MEDIUM_FORMAT  -N NOTES\n");
}

/* Reads embedded metadata into it->tags (best-effort: read failures leave the
   set empty and has_tags=0). FLAC uses Vorbis Comments, Musepack uses APEv2. */
static void
read_track_tags(import_track *it, const char *srcpath)
{
    const char *dot = strrchr(it->src_rel, '.');

    it->has_tags = 0;
    if (dot == 0)
        return;
    if (strcmp(dot, ".flac") == 0) {
        if (musicpack_flac_read_metadata(srcpath, &it->tags, &it->pics) == MUSICPACK_OK)
            it->has_tags = 1;
    } else if (strcmp(dot, ".mpc") == 0) {
        if (musicpack_ape_read(srcpath, &it->tags) == MUSICPACK_OK)
            it->has_tags = 1;
    }
}

/* Reads a positive integer tag field, accepting the Vorbis and APEv2 key
   spellings. Returns 1 on success. */
static int
tag_int_field(const musicpack_tag_set *tags, const char *vorbis_key,
              const char *ape_key, int *out)
{
    const musicpack_tag *t = musicpack_tag_set_get(tags, vorbis_key);
    if (t == 0 || t->is_binary)
        t = musicpack_tag_set_get(tags, ape_key);
    if (t == 0 || t->is_binary)
        return 0;
    return musicpack_meta_parse_track_number(t->value, out);
}

/* Replaces characters that are illegal inside a single path component. */
static void
sanitize_component(char *s)
{
    for (; *s != '\0'; s++)
        if (*s == '/' || *s == '\\' || *s == ':')
            *s = '-';
}

/* Writes raw bytes (not NUL-terminated) to a file. */
static int
write_bytes(const char *path, const unsigned char *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    if (f == 0)
        return -1;
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        fclose(f);
        return -1;
    }
    if (fclose(f) != 0)
        return -1;
    return 0;
}

static const char *
ext_for_mime(const char *mime)
{
    if (mime == 0)
        return ".img";
    if (strcmp(mime, "image/jpeg") == 0) return ".jpg";
    if (strcmp(mime, "image/png") == 0) return ".png";
    if (strcmp(mime, "image/gif") == 0) return ".gif";
    if (strcmp(mime, "image/webp") == 0) return ".webp";
    if (strcmp(mime, "image/bmp") == 0) return ".bmp";
    return ".img";
}

static int
artwork_role_taken(const musicpack_manifest *m, const char *role)
{
    size_t i;
    for (i = 0; i < m->artwork_count; i++)
        if (strcmp(m->artwork[i].role, role) == 0)
            return 1;
    return 0;
}

static int
manifest_add_artwork(musicpack_manifest *m, const char *role,
                     const char *relpath, const char *sha)
{
    musicpack_artwork *na =
        (musicpack_artwork *) realloc(m->artwork,
                                      (m->artwork_count + 1) * sizeof *na);
    if (na == 0)
        return -1;
    m->artwork = na;
    m->artwork[m->artwork_count].role = strdup(role);
    m->artwork[m->artwork_count].asset.path = strdup(relpath);
    m->artwork[m->artwork_count].asset.sha256 = sha != 0 ? strdup(sha) : 0;
    if (m->artwork[m->artwork_count].role == 0 ||
        m->artwork[m->artwork_count].asset.path == 0 ||
        (sha != 0 && m->artwork[m->artwork_count].asset.sha256 == 0))
        return -1;
    m->artwork_count++;
    return 0;
}

/* ------------------------------------------------------------------ */
/* audio stream probe (authoring draft display data)                   */
/* ------------------------------------------------------------------ */

static void
probe_stream(const char *path, mpc_stream_info *out)
{
    const char *dot = strrchr(path, '.');
    memset(out, 0, sizeof *out);

    if (dot != 0 && strcmp(dot, ".mpc") == 0) {
        mpc_reader reader;
        musepack_decoder *dec;
        musepack_stream_info si;
        if (mpc_reader_init_stdio(&reader, path) == MPC_STATUS_OK) {
            dec = musepack_decoder_open(&reader, 0);
            if (dec != 0) {
                memset(&si, 0, sizeof si);
                si.size = sizeof si;
                if (musepack_decoder_get_stream_info(dec, &si) == 0) {
                    snprintf(out->codec, sizeof out->codec, "musepack-sv%u",
                             (unsigned) si.stream_version);
                    out->stream_version = (int) si.stream_version;
                    out->sample_rate = (long) si.sample_rate;
                    out->channels = (long) si.channels;
                    if (si.sample_rate > 0)
                        out->duration = (double) si.length_samples /
                                        (double) si.sample_rate;
                }
                musepack_decoder_close(dec);
            }
            mpc_reader_exit_stdio(&reader);
        }
        if (out->codec[0] == '\0')
            snprintf(out->codec, sizeof out->codec, "musepack");
    } else if (dot != 0 && strcmp(dot, ".flac") == 0) {
        /* minimal STREAMINFO parse (first metadata block, 34 bytes). The
           block header occupies bytes 4..7; the STREAMINFO body starts at
           byte 8: min/max block size (16+16), min/max frame size (24+24),
           sample rate (20), channels-1 (3), bits per sample-1 (5), total
           samples (36), then the MD5. */
        unsigned char h[42];
        FILE *f = fopen(path, "rb");
        if (f != 0) {
            size_t got = fread(h, 1, sizeof h, f);
            fclose(f);
            if (got >= 42 && memcmp(h, "fLaC", 4) == 0 && (h[4] & 0x7f) == 0) {
                long rate = ((long) h[18] << 12) | ((long) h[19] << 4) | (h[20] >> 4);
                long ch = ((h[20] & 0x0e) >> 1) + 1;
                long bits = ((h[20] & 0x01) << 4) | (h[21] >> 4);
                bits += 1;
                uint64_t samples = ((uint64_t) (h[21] & 0x0f) << 32) |
                                   ((uint64_t) h[22] << 24) |
                                   ((uint64_t) h[23] << 16) |
                                   ((uint64_t) h[24] << 8) | h[25];
                snprintf(out->codec, sizeof out->codec, "flac");
                if (rate > 0) {
                    out->sample_rate = rate;
                    out->channels = ch;
                    out->bits = bits;
                    out->duration = (double) samples / (double) rate;
                }
            }
        }
        if (out->codec[0] == '\0')
            snprintf(out->codec, sizeof out->codec, "flac");
    } else {
        snprintf(out->codec, sizeof out->codec, "%s", codec_for_path(path));
    }
}

/* ------------------------------------------------------------------ */
/* shared album-directory scan (import + inspect)                      */
/* ------------------------------------------------------------------ */

typedef struct {
    import_track *tracks;
    size_t track_count;
    char *artwork_src;   /* relative path under the source root, or NULL */
    char *booklet_src;
    char **lyrics_srcs;
    size_t lyrics_count;
    char **extras_srcs;
    size_t extras_count;
} scan_result;

static void
scan_result_clear(scan_result *s)
{
    size_t i;
    for (i = 0; i < s->track_count; i++) {
        musicpack_tag_set_free(&s->tracks[i].tags);
        musicpack_pictures_free(&s->tracks[i].pics);
        free(s->tracks[i].lyric_path);
        free(s->tracks[i].lyric_sha);
        free(s->tracks[i].src_rel);
        free(s->tracks[i].title);
        free(s->tracks[i].ext);
    }
    free(s->tracks);
    free(s->artwork_src);
    free(s->booklet_src);
    for (i = 0; i < s->lyrics_count; i++)
        free(s->lyrics_srcs[i]);
    free(s->lyrics_srcs);
    for (i = 0; i < s->extras_count; i++)
        free(s->extras_srcs[i]);
    free(s->extras_srcs);
    memset(s, 0, sizeof *s);
}

/* Walks, classifies, sorts and (where numbers are missing) numbers the
   audio files under `src`. Shared by `import` (which additionally renumbers
   duplicates) and `inspect` (which preserves tag-derived duplicate track
   numbers so the GUI can surface them). Returns 0 when no audio was found. */
static int
scan_source_dir(const char *src, scan_result *out)
{
    char **files = 0;
    size_t file_count = 0, file_cap = 0;
    size_t i;
    char srcpath[MUSICPACK_PATH_MAX + 2];

    memset(out, 0, sizeof *out);
    walk_dir(src, "", &files, &file_count, &file_cap);

    for (i = 0; i < file_count; i++) {
        const char *rel = files[i];
        const char *first, *rest, *dot;
        int disc = 1;
        int from_dir = 0;

        split_segments(rel, &first, &rest);
        if (rest != 0) {
            disc = disc_from_dirname(first);
            if (disc == 0)
                continue; /* ignore files under non-disc subdirectories */
            from_dir = 1;
            rel = rest;
        }
        if (strcmp(rel, "cover.jpg") == 0 || strcmp(rel, "cover.png") == 0 ||
            strcmp(rel, "front.jpg") == 0 || strcmp(rel, "front.png") == 0 ||
            strcmp(rel, "folder.jpg") == 0) {
            free(out->artwork_src);
            out->artwork_src = strdup(files[i]);
            continue;
        }
        if (strcmp(rel, "booklet.pdf") == 0) {
            free(out->booklet_src);
            out->booklet_src = strdup(files[i]);
            continue;
        }
        dot = strrchr(rel, '.');
        if (dot != 0 && strcmp(dot, ".lrc") == 0) {
            out->lyrics_srcs = (char **) realloc(
                out->lyrics_srcs, (out->lyrics_count + 1) * sizeof *out->lyrics_srcs);
            if (out->lyrics_srcs == 0) {
                free(files);
                return 0;
            }
            out->lyrics_srcs[out->lyrics_count++] = strdup(files[i]);
            continue;
        }
        if (dot != 0 && (strcmp(dot, ".txt") == 0 || strcmp(dot, ".md") == 0)) {
            out->extras_srcs = (char **) realloc(
                out->extras_srcs, (out->extras_count + 1) * sizeof *out->extras_srcs);
            if (out->extras_srcs == 0) {
                free(files);
                return 0;
            }
            out->extras_srcs[out->extras_count++] = strdup(files[i]);
            continue;
        }
        if (is_audio_ext(rel)) {
            import_track *it;
            out->tracks = (import_track *) realloc(
                out->tracks, (out->track_count + 1) * sizeof *out->tracks);
            if (out->tracks == 0) {
                free(files);
                return 0;
            }
            it = &out->tracks[out->track_count++];
            memset(it, 0, sizeof *it);
            it->src_rel = strdup(files[i]);
            it->disc = disc;
            {
                const char *base = rest != 0 ? rest : first;
                int n = 0, digits = 0;
                while (base[n] >= '0' && base[n] <= '9') {
                    n++;
                    digits++;
                }
                it->number = digits > 0 ? atoi(base) : 0;
                {
                    const char *t = base;
                    size_t stem_len;
                    char *stem;
                    if (digits > 0) {
                        t = base + n;
                        while (*t == ' ' || *t == '-' || *t == '.')
                            t++;
                    }
                    dot = strrchr(t, '.');
                    stem_len = dot != 0 ? (size_t) (dot - t) : strlen(t);
                    stem = (char *) malloc(stem_len + 1);
                    if (stem == 0) {
                        free(files);
                        return 0;
                    }
                    memcpy(stem, t, stem_len);
                    stem[stem_len] = '\0';
                    it->title = stem;
                }
                {
                    const char *dot2 = strrchr(base, '.');
                    it->ext = strdup(dot2 != 0 ? dot2 : "");
                }
            }
            snprintf(srcpath, sizeof srcpath, "%s/%s", src, files[i]);
            read_track_tags(it, srcpath);
            if (it->has_tags) {
                const musicpack_tag *tv;
                int num;
                if (tag_int_field(&it->tags, "TRACKNUMBER", "Track", &num))
                    it->number = num;
                tv = musicpack_tag_set_get(&it->tags, "TITLE");
                if (tv != 0 && !tv->is_binary && tv->value != 0 && *tv->value != '\0') {
                    free(it->title);
                    it->title = strdup(tv->value);
                }
                if (!from_dir && tag_int_field(&it->tags, "DISCNUMBER", "Disc", &num))
                    it->disc = num;
            }
        }
    }
    free(files);

    if (out->track_count == 0)
        return 0;

    qsort(out->tracks, out->track_count, sizeof *out->tracks, cmp_import_tracks);
    {
        size_t i2 = 0;
        while (i2 < out->track_count) {
            int disc = out->tracks[i2].disc;
            size_t j = i2;
            int all_have = 1;
            while (j < out->track_count && out->tracks[j].disc == disc) {
                if (out->tracks[j].number <= 0)
                    all_have = 0;
                j++;
            }
            if (!all_have) {
                int seq = 1;
                size_t k;
                for (k = i2; k < j; k++)
                    out->tracks[k].number = seq++;
            }
            i2 = j;
        }
    }
    if (out->track_count > 1 && out->tracks[0].has_tags) {
        const musicpack_tag *a0 = musicpack_tag_set_get(&out->tracks[0].tags, "ALBUM");
        if (a0 != 0 && !a0->is_binary) {
            for (i = 1; i < out->track_count; i++) {
                const musicpack_tag *ai;
                if (!out->tracks[i].has_tags)
                    continue;
                ai = musicpack_tag_set_get(&out->tracks[i].tags, "ALBUM");
                if (ai != 0 && !ai->is_binary && strcmp(a0->value, ai->value) != 0)
                    fprintf(stderr,
                            "warning: conflicting album names ('%s' vs '%s')\n",
                            a0->value, ai->value);
            }
        }
    }
    return 1;
}

static int
cmd_import(int argc, char **argv)
{
    const char *src = 0, *out_dir = 0, *title = 0;
    const char *final_dir;
    char stage_dir[MUSICPACK_PATH_MAX + 2];
    const char *release_date = 0, *release_type = 0, *orig_release_date = 0;
    const char *edition = 0, *label = 0, *catalogue = 0, *country = 0;
    const char *medium_format = 0, *notes = 0;
    char *artists[64];
    size_t artist_count = 0;
    int no_loudness = 0;
    int c;
    scan_result scan;
    musicpack_manifest m;
    musicpack_meter *album_meter = 0;
    char audio_dir[MUSICPACK_PATH_MAX + 2];
    char art_dir[MUSICPACK_PATH_MAX + 2], lyr_dir[MUSICPACK_PATH_MAX + 2];
    char hex[MUSICPACK_SHA256_HEX_SIZE];
    char srcpath[MUSICPACK_PATH_MAX + 2];
    size_t i;
    int bad = 0;

    while ((c = getopt(argc, argv, "o:t:a:Ld:R:O:e:l:c:C:m:N:")) != -1) {
        switch (c) {
        case 'o': out_dir = optarg; break;
        case 't': title = optarg; break;
        case 'a':
            if (artist_count >= sizeof artists / sizeof *artists)
                return usage_error("too many artists");
            artists[artist_count++] = optarg;
            break;
        case 'L': no_loudness = 1; break;
        case 'd': release_date = optarg; break;
        case 'R': release_type = optarg; break;
        case 'O': orig_release_date = optarg; break;
        case 'e': edition = optarg; break;
        case 'l': label = optarg; break;
        case 'c': catalogue = optarg; break;
        case 'C': country = optarg; break;
        case 'm': medium_format = optarg; break;
        case 'N': notes = optarg; break;
        default: usage_import(); return 2;
        }
    }
    if (optind < argc)
        src = argv[optind];
    if (src == 0 || out_dir == 0) {
        usage_import();
        return 2;
    }
    final_dir = out_dir;
    if (!prepare_stage(final_dir, "import", stage_dir, sizeof stage_dir)) {
        fprintf(stderr, "import: output or staging destination already exists\n");
        return 1;
    }
    out_dir = stage_dir;

    if (!scan_source_dir(src, &scan)) {
        fprintf(stderr, "no audio files found under '%s'\n", src);
        rm_rf(out_dir);
        return 1;
    }

    /* consistency: duplicate (disc, track) falls back to renumbering. This is
       an import convenience; `inspect` preserves tag-derived duplicate track
       numbers so the GUI can surface them as validation errors instead. */
    {
        size_t i2 = 0;
        while (i2 < scan.track_count) {
            int disc = scan.tracks[i2].disc;
            size_t j = i2, k, l;
            int dup = 0;
            while (j < scan.track_count && scan.tracks[j].disc == disc)
                j++;
            for (k = i2; k < j; k++)
                for (l = k + 1; l < j; l++)
                    if (scan.tracks[k].number > 0 &&
                        scan.tracks[k].number == scan.tracks[l].number)
                        dup = 1;
            if (dup) {
                int seq = 1;
                fprintf(stderr,
                        "warning: duplicate track numbers on disc %d; renumbering\n",
                        disc);
                for (k = i2; k < j; k++)
                    scan.tracks[k].number = seq++;
            }
            i2 = j;
        }
    }

    /* build package: explicit flags first, then embedded metadata fills the
       gaps (first-wins), then the folder name is the title fallback. */
    memset(&m, 0, sizeof m);
    if (title != 0)
        m.album_title = strdup(title);
    m.album_artists = (musicpack_artist *) calloc(artist_count, sizeof *m.album_artists);
    for (i = 0; i < artist_count; i++)
        m.album_artists[i].name = strdup(artists[i]);
    m.album_artist_count = artist_count;
    if (release_type != 0)
        m.release_type = strdup(release_type);
    if (orig_release_date != 0)
        m.original_release_date = strdup(orig_release_date);
    if (release_date != 0) {
        m.release.present = 1;
        m.release.release_date = strdup(release_date);
    }
    if (edition != 0) { m.release.present = 1; m.release.edition = strdup(edition); }
    if (label != 0) { m.release.present = 1; m.release.label = strdup(label); }
    if (catalogue != 0) { m.release.present = 1; m.release.catalogue_number = strdup(catalogue); }
    if (country != 0) { m.release.present = 1; m.release.country = strdup(country); }
    if (notes != 0) { m.release.present = 1; m.release.notes = strdup(notes); }

    if (scan.track_count > 0 && scan.tracks[0].has_tags) {
        musicpack_status st = musicpack_tag_map_album(&scan.tracks[0].tags, &m);
        if (st != MUSICPACK_OK) {
            fprintf(stderr, "cannot read album metadata\n");
            bad = 1;
            goto cleanup;
        }
    }
    if (m.album_title == 0)
        m.album_title = strdup(src);

    snprintf(audio_dir, sizeof audio_dir, "%s/audio", out_dir);
    snprintf(art_dir, sizeof art_dir, "%s/artwork", out_dir);
    snprintf(lyr_dir, sizeof lyr_dir, "%s/lyrics", out_dir);
    if (mkdir_p(out_dir) != 0 || mkdir_p(audio_dir) != 0 ||
        mkdir_p(art_dir) != 0 || mkdir_p(lyr_dir) != 0) {
        fprintf(stderr, "cannot create package directory '%s'\n", out_dir);
        bad = 1;
        goto cleanup;
    }

    /* count discs */
    {
        size_t d, ndiscs = 0;
        for (d = 0; d < scan.track_count; d++)
            if (d == 0 || scan.tracks[d].disc != scan.tracks[d - 1].disc)
                ndiscs++;
        m.discs = (musicpack_disc *) calloc(ndiscs, sizeof *m.discs);
        m.disc_count = ndiscs;
        ndiscs = 0;
        for (d = 0; d < scan.track_count; d++)
            if (d == 0 || scan.tracks[d].disc != scan.tracks[d - 1].disc)
                m.discs[ndiscs++].disc = scan.tracks[d].disc;
    }

    for (i = 0; i < scan.track_count; i++) {
        import_track *it = &scan.tracks[i];
        musicpack_disc *disc;
        musicpack_track *t;
        char target[MUSICPACK_PATH_MAX + 2];

        size_t d;
        for (d = 0; d < m.disc_count && m.discs[d].disc != it->disc; d++)
            ;
        if (d == m.disc_count) {
            bad = 1;
            break;
        }
        disc = &m.discs[d];
        if (disc->format == 0 && medium_format != 0)
            disc->format = strdup(medium_format);
        disc->tracks = (musicpack_track *) realloc(disc->tracks,
                                                   (disc->track_count + 1) * sizeof *disc->tracks);
        t = &disc->tracks[disc->track_count];
        memset(t, 0, sizeof *t);
        if (it->has_tags) {
            musicpack_status st = musicpack_tag_map_track(&it->tags, t);
            if (st != MUSICPACK_OK) {
                fprintf(stderr, "cannot read track metadata\n");
                bad = 1;
                break;
            }
        }
        if (t->number == 0)
            t->number = it->number;
        if (t->title == 0)
            t->title = strdup(it->title);
        {
            char fname[MUSICPACK_PATH_MAX + 2];
            snprintf(fname, sizeof fname, "%s", t->title != 0 ? t->title : "");
            sanitize_component(fname);
            if (m.disc_count > 1)
                snprintf(target, sizeof target, "%s/%d-%02d - %s%s", audio_dir,
                         disc->disc, t->number, fname, it->ext != 0 ? it->ext : "");
            else
                snprintf(target, sizeof target, "%s/%02d - %s%s", audio_dir, t->number,
                         fname, it->ext != 0 ? it->ext : "");
        }
        snprintf(srcpath, sizeof srcpath, "%s/%s", src, it->src_rel);

        if (copy_file(srcpath, target) != 0) {
            fprintf(stderr, "cannot copy '%s'\n", srcpath);
            bad = 1;
            break;
        }
        t->audio.path = strdup(target + strlen(out_dir) + 1);
        if (musicpack_sha256_file(target, hex, sizeof hex) != MUSICPACK_OK) {
            fprintf(stderr, "cannot hash '%s'\n", target);
            bad = 1;
            break;
        }
        t->audio.sha256 = strdup(hex);
        if (!no_loudness) {
            int has_l;
            double lufs, peak, dur = 0;
            if (measure_loudness(target, &has_l, &lufs, &peak, &dur,
                                 &album_meter) == 0 && has_l) {
                t->loudness.present = 1;
                t->loudness.lufs = lufs;
                t->loudness.true_peak_db = peak;
                if (dur > 0) {
                    t->has_duration = 1;
                    t->duration = dur;
                }
            } else {
                fprintf(stderr, "warning: could not measure loudness of '%s'\n", it->src_rel);
            }
        }
        /* unsynchronized lyrics tag -> first-class lyrics asset */
        if (it->has_tags) {
            const musicpack_tag *ly = musicpack_tag_set_get(&it->tags, "LYRICS");
            if (ly == 0 || ly->is_binary)
                ly = musicpack_tag_set_get(&it->tags, "UNSYNCEDLYRICS");
            if (ly != 0 && !ly->is_binary && ly->value != 0 && *ly->value != '\0') {
                char lpath[MUSICPACK_PATH_MAX + 2];
                char fname2[MUSICPACK_PATH_MAX + 2];
                snprintf(fname2, sizeof fname2, "%s", t->title != 0 ? t->title : "");
                sanitize_component(fname2);
                snprintf(lpath, sizeof lpath, "%s/%02d - %s.txt", lyr_dir, t->number,
                         fname2);
                if (unique_target(lpath, sizeof lpath) && write_all(lpath, ly->value) == 0) {
                    it->lyric_path = strdup(lpath + strlen(out_dir) + 1);
                    if (musicpack_sha256_file(lpath, hex, sizeof hex) == MUSICPACK_OK)
                        it->lyric_sha = strdup(hex);
                }
            }
        }
        disc->track_count++;
    }

    /* embedded artwork: local cover files (below) win; otherwise FLAC
       pictures and APEv2 cover art fill missing roles, first per role. */
    if (!bad) {
        size_t t_i;
        char target[MUSICPACK_PATH_MAX + 2];
        for (t_i = 0; t_i < scan.track_count; t_i++) {
            import_track *it = &scan.tracks[t_i];
            size_t k;
            for (k = 0; k < it->pics.count; k++) {
                const musicpack_picture *pic = &it->pics.items[k];
                const char *role = musicpack_meta_picture_role(pic->type);
                const char *ext;
                char rel[MUSICPACK_PATH_MAX + 2];
                if (artwork_role_taken(&m, role))
                    continue;
                ext = ext_for_mime(pic->mime);
                snprintf(target, sizeof target, "%s/%s%s", art_dir, role, ext);
                if (write_bytes(target, pic->data, pic->data_len) != 0) {
                    fprintf(stderr, "cannot write artwork\n");
                    bad = 1;
                    break;
                }
                snprintf(rel, sizeof rel, "artwork/%s%s", role, ext);
                if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                    manifest_add_artwork(&m, role, rel, hex);
                else
                    manifest_add_artwork(&m, role, rel, 0);
            }
            if (it->has_tags) {
                const musicpack_tag *cov =
                    musicpack_tag_set_get(&it->tags, "Cover Art (Front)");
                if (cov != 0 && cov->is_binary && cov->binary_len > 0 &&
                    !artwork_role_taken(&m, "front")) {
                    const unsigned char *nul =
                        (const unsigned char *) memchr(cov->binary, '\0',
                                                       cov->binary_len);
                    const unsigned char *img = nul != 0 ? nul + 1 : cov->binary;
                    size_t img_len = nul != 0
                        ? cov->binary_len - (size_t) (nul - cov->binary) - 1
                        : cov->binary_len;
                    const char *fname = (const char *) cov->binary;
                    const char *dot = strrchr(fname, '.');
                    const char *ext = dot != 0 ? dot : ".img";
                    char rel[MUSICPACK_PATH_MAX + 2];
                    if (img_len == 0)
                        continue;
                    snprintf(target, sizeof target, "%s/front%s", art_dir, ext);
                    if (write_bytes(target, img, img_len) != 0) {
                        fprintf(stderr, "cannot write artwork\n");
                        bad = 1;
                        break;
                    }
                    snprintf(rel, sizeof rel, "artwork/front%s", ext);
                    if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                        manifest_add_artwork(&m, "front", rel, hex);
                    else
                        manifest_add_artwork(&m, "front", rel, 0);
                }
            }
        }
    }

    if (!bad && scan.artwork_src != 0) {
        char target[MUSICPACK_PATH_MAX + 2];
        const char *ext = strrchr(scan.artwork_src, '.');
        snprintf(target, sizeof target, "%s/front%s", art_dir, ext != 0 ? ext : ".jpg");
        if (snprintf(srcpath, sizeof srcpath, "%s/%s", src, scan.artwork_src) >= (int) sizeof srcpath) bad = 1;
        if (!bad && copy_file(srcpath, target) != 0) {
            fprintf(stderr, "cannot copy artwork\n");
            bad = 1;
        } else {
            m.artwork = (musicpack_artwork *) calloc(1, sizeof *m.artwork);
            m.artwork_count = 1;
            m.artwork[0].role = strdup("front");
            m.artwork[0].asset.path = strdup(target + strlen(out_dir) + 1);
            if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                m.artwork[0].asset.sha256 = strdup(hex);
        }
    }
    if (!bad && scan.booklet_src != 0) {
        char target[MUSICPACK_PATH_MAX + 2];
        snprintf(target, sizeof target, "%s/booklet", out_dir);
        if (mkdir_p(target) != 0)
            bad = 1;
        else {
            snprintf(target, sizeof target, "%s/booklet/booklet.pdf", out_dir);
            if (snprintf(srcpath, sizeof srcpath, "%s/%s", src, scan.booklet_src) >= (int) sizeof srcpath) bad = 1;
            if (!bad && copy_file(srcpath, target) != 0) {
                fprintf(stderr, "cannot copy booklet\n");
                bad = 1;
            } else {
                m.booklet = (musicpack_asset *) calloc(1, sizeof *m.booklet);
                m.booklet_count = 1;
                m.booklet[0].path = strdup(target + strlen(out_dir) + 1);
                if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                    m.booklet[0].sha256 = strdup(hex);
            }
        }
    }
    if (!bad && scan.lyrics_count > 0) {
        size_t k;
        m.lyrics = (musicpack_asset *) calloc(scan.lyrics_count, sizeof *m.lyrics);
        m.lyrics_count = scan.lyrics_count;
        for (k = 0; k < scan.lyrics_count && !bad; k++) {
            char target[MUSICPACK_PATH_MAX + 2];
            const char *base = strrchr(scan.lyrics_srcs[k], '/');
            const char *name = base != 0 ? base + 1 : scan.lyrics_srcs[k];
            snprintf(target, sizeof target, "%s/%s", lyr_dir, name);
            if (!unique_target(target, sizeof target)) { bad = 1; break; }
            if (snprintf(srcpath, sizeof srcpath, "%s/%s", src, scan.lyrics_srcs[k]) >= (int) sizeof srcpath) { bad = 1; break; }
            if (copy_file(srcpath, target) != 0) {
                fprintf(stderr, "cannot copy lyrics '%s'\n", name);
                bad = 1;
                break;
            }
            m.lyrics[k].path = strdup(target + strlen(out_dir) + 1);
            if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                m.lyrics[k].sha256 = strdup(hex);
        }
    }
    /* merge tag-derived lyrics assets into the manifest (ownership moves);
       lyrics_count keeps counting only the source .lrc files so cleanup stays
       correct */
    if (!bad) {
        size_t tag_count = 0, src_n = scan.lyrics_count, mcount;
        for (i = 0; i < scan.track_count; i++)
            if (scan.tracks[i].lyric_path != 0)
                tag_count++;
        if (tag_count > 0) {
            musicpack_asset *na = (musicpack_asset *) realloc(
                m.lyrics, (src_n + tag_count) * sizeof *na);
            if (na == 0)
                bad = 1;
            else {
                m.lyrics = na;
                mcount = src_n;
                for (i = 0; i < scan.track_count; i++) {
                    if (scan.tracks[i].lyric_path != 0) {
                        musicpack_asset *a = &m.lyrics[mcount++];
                        a->path = scan.tracks[i].lyric_path;
                        a->sha256 = scan.tracks[i].lyric_sha;
                        scan.tracks[i].lyric_path = 0;
                        scan.tracks[i].lyric_sha = 0;
                    }
                }
                m.lyrics_count = mcount;
            }
        }
    }
    if (!bad && scan.extras_count > 0) {
        size_t k;
        char ex_dir[MUSICPACK_PATH_MAX + 2];
        snprintf(ex_dir, sizeof ex_dir, "%s/extras", out_dir);
        if (mkdir_p(ex_dir) != 0)
            bad = 1;
        else {
            m.extras = (musicpack_asset *) calloc(scan.extras_count, sizeof *m.extras);
            m.extras_count = scan.extras_count;
            for (k = 0; k < scan.extras_count && !bad; k++) {
                char target[MUSICPACK_PATH_MAX + 2];
                const char *base = strrchr(scan.extras_srcs[k], '/');
                const char *name = base != 0 ? base + 1 : scan.extras_srcs[k];
                snprintf(target, sizeof target, "%s/%s", ex_dir, name);
                if (!unique_target(target, sizeof target)) { bad = 1; break; }
                if (snprintf(srcpath, sizeof srcpath, "%s/%s", src, scan.extras_srcs[k]) >= (int) sizeof srcpath) { bad = 1; break; }
                if (copy_file(srcpath, target) != 0) {
                    fprintf(stderr, "cannot copy extra '%s'\n", name);
                    bad = 1;
                    break;
                }
                m.extras[k].path = strdup(target + strlen(out_dir) + 1);
                if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                    m.extras[k].sha256 = strdup(hex);
            }
        }
    }

    if (!no_loudness && album_meter != 0) {
        double alufs, apeak;
        if (musicpack_meter_result(album_meter, &alufs, &apeak) == MUSICPACK_OK) {
            m.has_album_loudness = 1;
            m.album_loudness.lufs = alufs;
            m.album_loudness.true_peak_db = apeak;
            m.loudness_algorithm = strdup(MUSICPACK_LOUDNESS_STANDARD);
        }
    }

    if (!bad) {
        char *json = 0;
        if (musicpack_manifest_write(&m, &json) == MUSICPACK_OK) {
            char mpath[MUSICPACK_PATH_MAX + 2];
            snprintf(mpath, sizeof mpath, "%s/manifest.json", out_dir);
            if (write_all(mpath, json) != 0)
                bad = 1;
            free(json);
        } else {
            bad = 1;
        }
    }

cleanup:
    /* free model */
    musicpack_manifest_clear(&m);
    musicpack_meter_free(album_meter);
    {
        size_t imported = scan.track_count;
        scan_result_clear(&scan);

        if (!bad && !verify_staged_package(out_dir))
            bad = 1;
        if (!bad && rename(out_dir, final_dir) != 0)
            bad = 1;
        if (bad) {
            rm_rf(out_dir);
            fprintf(stderr, "import failed\n");
            return 1;
        }
        printf("imported %zu track(s) into '%s'\n", imported, final_dir);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* ------------------------------------------------------------------ */
/* authoring draft: inspect / validate-draft / build-draft /           */
/* identify-draft. These power the MusicPack Author GUI; they speak     */
/* JSON to stdout and keep every .mpack semantic inside libmusicpack.   */
/* ------------------------------------------------------------------ */

static const char *
djstr(cJSON *o, const char *key)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    return (v != 0 && cJSON_IsString(v)) ? v->valuestring : 0;
}

static int
djnum(cJSON *o, const char *key, int *out)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(o, key);
    if (v == 0 || !cJSON_IsNumber(v))
        return 0;
    *out = (int) v->valuedouble;
    return 1;
}

static void
json_error_out(const char *code, const char *msg)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *e = cJSON_AddObjectToObject(root, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg);
    draft_print(root);
    cJSON_Delete(root);
}

static void
add_string_array(cJSON *root, const char *key, char *const *items, size_t n)
{
    cJSON *arr = cJSON_AddArrayToObject(root, key);
    size_t i;
    for (i = 0; i < n; i++)
        cJSON_AddItemToArray(arr, cJSON_CreateString(items[i]));
}

/* ---- draft validation --------------------------------------------- */

/* Targeted checks below produce readable authoring messages. They are NOT
   the authority: the synthesized-manifest parse through
   musicpack_manifest_parse() remains the authoritative gate, so these lists
   only mirror the v1 closed enums for message quality. */
static const char *const DRAFT_RELEASE_TYPES[] = {
    "album", "ep", "single", "maxi-single", "compilation", "soundtrack",
    "live-album", "remix-album", "box-set", "other"
};
static const char *const DRAFT_MEDIUM_FORMATS[] = {
    "CD", "SACD", "Vinyl", "Cassette", "Digital", "Blu-ray Audio",
    "DVD-Audio", "Other"
};

static int
str_in_list(const char *const *list, size_t n, const char *v)
{
    size_t i;
    if (v == 0)
        return 0;
    for (i = 0; i < n; i++)
        if (strcmp(list[i], v) == 0)
            return 1;
    return 0;
}

static int
path_is_valid(const char *p)
{
    return musicpack_path_validate(p) == MUSICPACK_OK;
}

static int
is_dir_path(const char *p)
{
#ifdef _WIN32
    struct _stat st;
    return _stat(p, &st) == 0 && (st.st_mode & _S_IFDIR) != 0;
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
#endif
}

static int
is_regular_path(const char *p)
{
#ifdef _WIN32
    struct _stat st;
    return _stat(p, &st) == 0 && (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
#endif
}

/* Resolve an existing path, or a prospective child of an existing parent.
   This makes overlap checks work before the output directory is created. */
static int
canonical_path(const char *path, char *out, size_t cap)
{
    char parent[MUSICPACK_PATH_MAX + 2], *slash;
#ifdef _WIN32
    if (_fullpath(out, path, cap) != 0)
        return 1;
#else
    if (realpath(path, out) != 0)
        return 1;
#endif
    if (strlen(path) >= sizeof parent)
        return 0;
    snprintf(parent, sizeof parent, "%s", path);
    slash = strrchr(parent, '/');
    if (slash == 0)
        return 0;
    *slash = '\0';
#ifdef _WIN32
    if (_fullpath(out, parent[0] != '\0' ? parent : ".", cap) == 0)
        return 0;
#else
    if (realpath(parent[0] != '\0' ? parent : ".", out) == 0)
        return 0;
#endif
    if (strlen(out) + 1 + strlen(slash + 1) >= cap)
        return 0;
    strcat(out, "/");
    strcat(out, slash + 1);
    return 1;
}

static int
paths_overlap(const char *a, const char *b)
{
    size_t n = strlen(a);
    return strcmp(a, b) == 0 ||
           (strncmp(a, b, n) == 0 && b[n] == '/') ||
           (strncmp(b, a, strlen(b)) == 0 && a[strlen(b)] == '/');
}

static int
reject_path_overlap(const char *source, const char *output, const char *what)
{
    char src[MUSICPACK_PATH_MAX + 2], dst[MUSICPACK_PATH_MAX + 2];
    if (!canonical_path(source, src, sizeof src) ||
        !canonical_path(output, dst, sizeof dst) || paths_overlap(src, dst)) {
        fprintf(stderr, "%s: source and output directories overlap\n", what);
        return 1;
    }
    return 0;
}

/* Preserve every asset even when source directories contain equal basenames. */
static int
unique_target(char *path, size_t cap)
{
    char original[MUSICPACK_PATH_MAX + 2], *dot, *slash;
    unsigned n;
    if (!is_regular_path(path))
        return 1;
    snprintf(original, sizeof original, "%s", path);
    slash = strrchr(original, '/');
    dot = strrchr(slash != 0 ? slash + 1 : original, '.');
    for (n = 2; n < 1000000; n++) {
        int written;
        if (dot != 0) {
            *dot = '\0';
            written = snprintf(path, cap, "%s-%u%s", original, n, dot + 1);
            *dot = '.';
        } else {
            written = snprintf(path, cap, "%s-%u", original, n);
        }
        if (written < 0 || (size_t) written >= cap)
            return 0;
        if (!is_regular_path(path))
            return 1;
    }
    return 0;
}

static int
file_is_regular_under(const char *root, const char *rel)
{
    char p[MUSICPACK_PATH_MAX + 2];
    if (snprintf(p, sizeof p, "%s/%s", root, rel) >= (int) sizeof p)
        return 0;
    return is_regular_path(p);
}

static void
vec_push(char ***arr, size_t *n, const char *msg)
{
    char **na = (char **) realloc(*arr, (*n + 1) * sizeof **arr);
    if (na == 0)
        return;
    *arr = na;
    (*arr)[*n] = strdup(msg);
    if ((*arr)[*n] != 0)
        (*n)++;
}

/* Validates a parsed draft. Fills \p errors/\p warnings (heap arrays; the
   caller frees with free_vec). Returns 1 when there are no errors. */
static int
draft_validate(cJSON *draft, char ***errors, size_t *ecount,
               char ***warnings, size_t *wcount)
{
    cJSON *album, *media, *artwork;
    const char *sr, *title, *rt;
    int i, ndiscs = 0;
    int seen_disc[64];
    int ok = 1;

    *errors = 0;
    *warnings = 0;
    *ecount = 0;
    *wcount = 0;
    memset(seen_disc, 0, sizeof seen_disc);

    sr = djstr(draft, "sourceRoot");
    if (sr == 0 || *sr == '\0') {
        vec_push(errors, ecount, "missing source root");
        ok = 0;
    }

    album = cJSON_GetObjectItemCaseSensitive(draft, "album");
    title = djstr(album, "title");
    if (title == 0 || *title == '\0') {
        vec_push(errors, ecount, "missing required album title");
        ok = 0;
    }
    {
        cJSON *artists = cJSON_GetObjectItemCaseSensitive(album, "artists");
        if (!cJSON_IsArray(artists) || cJSON_GetArraySize(artists) == 0) {
            vec_push(errors, ecount, "no artist");
            ok = 0;
        }
    }
    rt = djstr(album, "releaseType");
    if (rt != 0 && *rt != '\0' &&
        !str_in_list(DRAFT_RELEASE_TYPES,
                     sizeof DRAFT_RELEASE_TYPES / sizeof *DRAFT_RELEASE_TYPES, rt)) {
        char msg[256];
        snprintf(msg, sizeof msg, "unsupported release type '%s'", rt);
        vec_push(errors, ecount, msg);
        ok = 0;
    }

    media = cJSON_GetObjectItemCaseSensitive(draft, "media");
    if (!cJSON_IsArray(media) || cJSON_GetArraySize(media) == 0) {
        vec_push(errors, ecount, "no media");
        ok = 0;
    } else {
        {
            cJSON *mi;
            cJSON_ArrayForEach(mi, media) {
            int disc = 0;
            const char *fmt;
            cJSON *tracks;
            int seen_track[1024];

            memset(seen_track, 0, sizeof seen_track);
            if (!cJSON_IsObject(mi))
                continue;
            djnum(mi, "disc", &disc);
            if (disc < 1) {
                vec_push(errors, ecount, "invalid disc number");
                ok = 0;
            } else if (disc <= 63) {
                if (seen_disc[disc]) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "duplicate disc number %d", disc);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                }
                seen_disc[disc] = 1;
            }
            if (disc > ndiscs)
                ndiscs = disc;
            fmt = djstr(mi, "format");
            if (fmt != 0 && *fmt != '\0' &&
                !str_in_list(DRAFT_MEDIUM_FORMATS,
                             sizeof DRAFT_MEDIUM_FORMATS / sizeof *DRAFT_MEDIUM_FORMATS,
                             fmt)) {
                char msg[256];
                snprintf(msg, sizeof msg, "invalid medium format '%s'", fmt);
                vec_push(errors, ecount, msg);
                ok = 0;
            }
            tracks = cJSON_GetObjectItemCaseSensitive(mi, "tracks");
            if (!cJSON_IsArray(tracks) || cJSON_GetArraySize(tracks) == 0) {
                char msg[128];
                snprintf(msg, sizeof msg, "disc %d has no tracks", disc);
                vec_push(errors, ecount, msg);
                ok = 0;
                continue;
            }
            {
                cJSON *tr;
                cJSON_ArrayForEach(tr, tracks) {
                int num = 0;
                const char *ttl, *ap;
                if (!cJSON_IsObject(tr))
                    continue;
                djnum(tr, "track", &num);
                if (num < 1) {
                    char msg[128];
                    snprintf(msg, sizeof msg, "invalid track number on disc %d", disc);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                } else if (num < 1024) {
                    if (seen_track[num]) {
                        char msg[160];
                        snprintf(msg, sizeof msg,
                                 "duplicate track number %d on disc %d", num, disc);
                        vec_push(errors, ecount, msg);
                        ok = 0;
                    }
                    seen_track[num] = 1;
                }
                ttl = djstr(tr, "title");
                if (ttl == 0 || *ttl == '\0') {
                    char msg[160];
                    snprintf(msg, sizeof msg, "track %d on disc %d has no title",
                             num, disc);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                }
                ap = djstr(tr, "audioPath");
                if (ap == 0 || *ap == '\0') {
                    char msg[160];
                    snprintf(msg, sizeof msg, "track %d on disc %d has no audio file",
                             num, disc);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                } else {
                    if (!path_is_valid(ap)) {
                        char msg[320];
                        snprintf(msg, sizeof msg, "invalid path '%s'", ap);
                        vec_push(errors, ecount, msg);
                        ok = 0;
                    } else if (sr != 0 && *sr != '\0' &&
                               !file_is_regular_under(sr, ap)) {
                        char msg[320];
                        snprintf(msg, sizeof msg, "audio file not found: %s", ap);
                        vec_push(errors, ecount, msg);
                        ok = 0;
                    }
                }
                /* encodability hints (warnings only; the .mpack spec does not
                   require a codec, so these never block a build) */
                {
                    const char *cd = djstr(tr, "codec");
                    const char *dot = strrchr(ap != 0 ? ap : "", '.');
                    const char *ext = dot != 0 ? dot : cd;
                    int is_mpc = (dot != 0 && strcmp(dot, ".mpc") == 0) ||
                                 (cd != 0 && strncmp(cd, "musepack", 8) == 0);
                    int encodable = ext != 0 &&
                                    (strcmp(ext, ".flac") == 0 ||
                                     strcmp(ext, ".wav") == 0);
                    if (!is_mpc && !encodable && ext != 0) {
                        char msg[200];
                        snprintf(msg, sizeof msg,
                                 "track %d on disc %d (%s) cannot be encoded to "
                                 "Musepack; only FLAC and WAV sources are supported",
                                 num, disc, ext);
                        vec_push(warnings, wcount, msg);
                    } else if (!is_mpc && encodable) {
                        int rate = 0;
                        djnum(tr, "sampleRate", &rate);
                        if (rate > 0 && rate != 32000 && rate != 37800 &&
                            rate != 44100 && rate != 48000) {
                            char msg[200];
                            snprintf(msg, sizeof msg,
                                     "track %d on disc %d — sample rate %d Hz is not "
                                     "supported by Musepack encoding (supported: "
                                     "32/37.8/44.1/48 kHz)",
                                     num, disc, rate);
                            vec_push(warnings, wcount, msg);
                        }
                    }
                }
            }
            }
        }
    }
    }

    /* artwork + other asset paths */
    artwork = cJSON_GetObjectItemCaseSensitive(draft, "artwork");
    if (cJSON_IsArray(artwork)) {
        size_t k = 0;
        cJSON *a;
        cJSON_ArrayForEach(a, artwork) {
            const char *p = djstr(a, "path");
            const char *sa = djstr(a, "sourceAudio");
            if (p != 0 && *p != '\0') {
                k++;
                if (!path_is_valid(p)) {
                    char msg[320];
                    snprintf(msg, sizeof msg, "invalid artwork path '%s'", p);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                } else if (sr != 0 && *sr != '\0' && !file_is_regular_under(sr, p)) {
                    char msg[320];
                    snprintf(msg, sizeof msg, "artwork file not found: %s", p);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                }
            } else if (sa != 0 && *sa != '\0') {
                k++;
                if (!path_is_valid(sa)) {
                    char msg[320];
                    snprintf(msg, sizeof msg, "invalid artwork source '%s'", sa);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                } else if (sr != 0 && *sr != '\0' && !file_is_regular_under(sr, sa)) {
                    char msg[320];
                    snprintf(msg, sizeof msg, "embedded artwork source not found: %s", sa);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                }
            }
        }
        if (k == 0)
            vec_push(warnings, wcount, "no artwork");
    }
    {
        const char *cat = 0;
        cJSON *rel = cJSON_GetObjectItemCaseSensitive(draft, "release");
        if (cJSON_IsObject(rel))
            cat = djstr(rel, "catalogueNumber");
        if (cat == 0 || *cat == '\0')
            vec_push(warnings, wcount, "missing catalogue number");
    }
    {
        const char *mbid = 0, *conf = 0;
        cJSON *ids = cJSON_GetObjectItemCaseSensitive(draft, "identifiers");
        cJSON *idn = cJSON_GetObjectItemCaseSensitive(draft, "identity");
        if (cJSON_IsObject(ids))
            mbid = djstr(ids, "musicbrainzReleaseId");
        if (cJSON_IsObject(idn))
            conf = djstr(idn, "confidence");
        if ((mbid == 0 || *mbid == '\0') &&
            (conf == 0 || strcmp(conf, "exact") != 0))
            vec_push(warnings, wcount, "missing release identity");
    }

    /* unreferenced audio files under the source root */
    if (sr != 0 && *sr != '\0') {
        char **files = 0;
        size_t file_count = 0, file_cap = 0;
        size_t f;
        if (is_dir_path(sr)) {
            walk_dir(sr, "", &files, &file_count, &file_cap);
            for (f = 0; f < file_count; f++) {
                const char *dot = strrchr(files[f], '.');
                int referenced = 0;
                if (dot == 0 || !is_audio_ext(files[f]))
                    continue;
                if (cJSON_IsArray(media)) {
                    cJSON *mi;
                    cJSON_ArrayForEach(mi, media) {
                        cJSON *tracks = cJSON_GetObjectItemCaseSensitive(mi, "tracks");
                        cJSON *tr;
                        if (!cJSON_IsArray(tracks))
                            continue;
                        cJSON_ArrayForEach(tr, tracks) {
                            const char *ap = djstr(tr, "audioPath");
                            if (ap != 0 && strcmp(ap, files[f]) == 0)
                                referenced = 1;
                        }
                    }
                }
                if (!referenced) {
                    char msg[320];
                    snprintf(msg, sizeof msg, "audio file not included: %s", files[f]);
                    vec_push(warnings, wcount, msg);
                }
            }
            for (f = 0; f < file_count; f++)
                free(files[f]);
            free(files);
        }
    }

    /* authoritative gate: synthesize a parseable manifest and run the real
       .mpack validation */
    {
        musicpack_manifest m;
        char *json = 0;
        size_t si;
        if (!draft_to_manifest(draft, &m)) {
            vec_push(errors, ecount, "cannot interpret draft");
            ok = 0;
        } else {
            const char *zero =
                "0000000000000000000000000000000000000000000000000000000000000000";
            for (si = 0; si < m.disc_count; si++)
                for (i = 0; i < (int) m.discs[si].track_count; i++)
                    if (m.discs[si].tracks[i].audio.sha256 == 0)
                        m.discs[si].tracks[i].audio.sha256 = strdup(zero);
            for (si = 0; si < m.artwork_count; si++)
                if (m.artwork[si].asset.sha256 == 0)
                    m.artwork[si].asset.sha256 = strdup(zero);
            for (si = 0; si < m.booklet_count; si++)
                if (m.booklet[si].sha256 == 0)
                    m.booklet[si].sha256 = strdup(zero);
            for (si = 0; si < m.lyrics_count; si++)
                if (m.lyrics[si].sha256 == 0)
                    m.lyrics[si].sha256 = strdup(zero);
            for (si = 0; si < m.extras_count; si++)
                if (m.extras[si].sha256 == 0)
                    m.extras[si].sha256 = strdup(zero);
            if (musicpack_manifest_write(&m, &json) == MUSICPACK_OK) {
                musicpack_status st = MUSICPACK_OK;
                musicpack_manifest *pm = musicpack_manifest_parse(json, &st);
                if (pm == 0) {
                    char msg[256];
                    snprintf(msg, sizeof msg,
                             "draft does not satisfy .mpack v1 rules (%d)", (int) st);
                    vec_push(errors, ecount, msg);
                    ok = 0;
                } else {
                    musicpack_manifest_free(pm);
                }
                free(json);
            } else {
                vec_push(errors, ecount, "cannot serialize draft");
                ok = 0;
            }
            musicpack_manifest_clear(&m);
        }
    }

    return ok;
}

static void
free_vec(char **items, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++)
        free(items[i]);
    free(items);
}

/* ---- command: inspect --------------------------------------------- */

static void
usage_inspect(void)
{
    fprintf(stderr, "usage: musicpack inspect <dir> [--json]\n");
}

static void
inspect_fill_assets(cJSON *draft, const scan_result *scan)
{
    char used[16][32];
    size_t used_count = 0;
    cJSON *artwork = cJSON_GetObjectItemCaseSensitive(draft, "artwork");
    cJSON *booklet = cJSON_GetObjectItemCaseSensitive(draft, "booklet");
    cJSON *lyrics = cJSON_GetObjectItemCaseSensitive(draft, "lyrics");
    cJSON *extras = cJSON_GetObjectItemCaseSensitive(draft, "extras");
    size_t i, k;
    int have_front = 0;

    if (!cJSON_IsArray(artwork))
        return;
    if (scan->artwork_src != 0) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "role", "front");
        cJSON_AddStringToObject(a, "path", scan->artwork_src);
        cJSON_AddItemToArray(artwork, a);
        snprintf(used[used_count++], sizeof used[0], "front");
        have_front = 1;
    }
    for (i = 0; i < scan->track_count; i++) {
        const import_track *it = &scan->tracks[i];
        for (k = 0; k < it->pics.count; k++) {
            const char *role = musicpack_meta_picture_role(it->pics.items[k].type);
            size_t u;
            int taken = 0;
            for (u = 0; u < used_count; u++)
                if (strcmp(used[u], role) == 0)
                    taken = 1;
            if (taken)
                continue;
            {
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "role", role);
                cJSON_AddStringToObject(a, "embedded", "true");
                cJSON_AddStringToObject(a, "sourceAudio", it->src_rel);
                if (it->pics.items[k].mime != 0)
                    cJSON_AddStringToObject(a, "mime", it->pics.items[k].mime);
                cJSON_AddItemToArray(artwork, a);
                if (used_count < sizeof used / sizeof used[0])
                    snprintf(used[used_count++], sizeof used[0], "%s", role);
            }
        }
        if (it->has_tags && !have_front) {
            const musicpack_tag *cov =
                musicpack_tag_set_get(&it->tags, "Cover Art (Front)");
            if (cov != 0 && cov->is_binary && cov->binary_len > 0) {
                cJSON *a = cJSON_CreateObject();
                cJSON_AddStringToObject(a, "role", "front");
                cJSON_AddStringToObject(a, "embedded", "true");
                cJSON_AddStringToObject(a, "sourceAudio", it->src_rel);
                cJSON_AddItemToArray(artwork, a);
                have_front = 1;
            }
        }
    }
    if (scan->booklet_src != 0 && cJSON_IsArray(booklet)) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "path", scan->booklet_src);
        cJSON_AddItemToArray(booklet, a);
    }
    if (cJSON_IsArray(lyrics)) {
        for (i = 0; i < scan->lyrics_count; i++) {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "path", scan->lyrics_srcs[i]);
            cJSON_AddItemToArray(lyrics, a);
        }
    }
    if (cJSON_IsArray(extras)) {
        for (i = 0; i < scan->extras_count; i++) {
            cJSON *a = cJSON_CreateObject();
            cJSON_AddStringToObject(a, "path", scan->extras_srcs[i]);
            cJSON_AddItemToArray(extras, a);
        }
    }
}

static int
cmd_inspect(const char *dir, int json)
{
    scan_result scan;
    musicpack_manifest m;
    mpc_stream_info *streams = 0;
    cJSON *draft = 0;
    char srcpath[MUSICPACK_PATH_MAX + 2];
    size_t i, total = 0;
    int ndiscs = 0, bad = 0;

    memset(&scan, 0, sizeof scan);
    if (!scan_source_dir(dir, &scan)) {
        fprintf(stderr, "inspect: no audio files found under '%s'\n", dir);
        return 1;
    }

    memset(&m, 0, sizeof m);
    /* union album metadata across every tagged track (first-wins per field,
       which musicpack_tag_map_album already guarantees): a minimal tag on the
       first file must not shadow a rich tag on a later file */
    for (i = 0; i < scan.track_count; i++) {
        if (scan.tracks[i].has_tags &&
            musicpack_tag_map_album(&scan.tracks[i].tags, &m) != MUSICPACK_OK)
            bad = 1;
        if (bad)
            break;
    }
    if (!bad && m.album_title == 0) {
        const char *base = strrchr(dir, '/');
        m.album_title = strdup(base != 0 && base[1] != '\0' ? base + 1 : dir);
    }

    if (!bad) {
        size_t d;
        for (d = 0; d < scan.track_count; d++)
            if (scan.tracks[d].disc > ndiscs)
                ndiscs = scan.tracks[d].disc;
        m.discs = (musicpack_disc *) calloc((size_t) ndiscs, sizeof *m.discs);
        if (m.discs == 0)
            bad = 1;
        m.disc_count = (size_t) ndiscs;
    }

    streams = (mpc_stream_info *) calloc(scan.track_count, sizeof *streams);
    if (streams == 0)
        bad = 1;
    for (i = 0; i < scan.track_count && !bad; i++) {
        import_track *it = &scan.tracks[i];
        musicpack_disc *disc;
        musicpack_track *t;
        if ((size_t) it->disc - 1 >= m.disc_count) {
            bad = 1;
            break;
        }
        disc = &m.discs[it->disc - 1];
        disc->disc = it->disc;
        disc->tracks = (musicpack_track *) realloc(
            disc->tracks, (disc->track_count + 1) * sizeof *disc->tracks);
        if (disc->tracks == 0) {
            bad = 1;
            break;
        }
        t = &disc->tracks[disc->track_count];
        memset(t, 0, sizeof *t);
        if (it->has_tags && musicpack_tag_map_track(&it->tags, t) != MUSICPACK_OK) {
            bad = 1;
            break;
        }
        if (t->number == 0)
            t->number = it->number;
        if (t->title == 0)
            t->title = strdup(it->title);
        t->audio.path = strdup(it->src_rel);
        snprintf(srcpath, sizeof srcpath, "%s/%s", dir, it->src_rel);
        probe_stream(srcpath, &streams[i]);
        disc->track_count++;
    }

    if (!bad)
        draft = draft_from_manifest(&m, dir, streams);
    if (draft != 0)
        inspect_fill_assets(draft, &scan);

    if (json) {
        if (draft != 0)
            draft_print(draft);
        else
            json_error_out("inspect_failed", "cannot build authoring draft");
    } else {
        size_t d, t;
        if (draft == 0) {
            fprintf(stderr, "inspect: cannot build authoring draft\n");
            bad = 1;
        } else {
            printf("Album: %s\n", m.album_title != 0 ? m.album_title : "");
            for (i = 0; i < m.album_artist_count; i++)
                printf("Artist: %s%s%s\n", m.album_artists[i].name,
                       m.album_artists[i].role != 0 ? " (" : "",
                       m.album_artists[i].role != 0 ? m.album_artists[i].role : "");
            printf("Discs: %zu\n", m.disc_count);
            for (d = 0; d < m.disc_count; d++) {
                printf("Disc %d: %zu tracks\n", m.discs[d].disc,
                       m.discs[d].track_count);
                for (t = 0; t < m.discs[d].track_count; t++) {
                    const musicpack_track *tr = &m.discs[d].tracks[t];
                    printf("  Track %d: %s (%s", tr->number,
                           tr->title != 0 ? tr->title : "",
                           streams != 0 ? streams[total].codec : "?");
                    if (streams != 0 && streams[total].duration > 0)
                        printf(", %.1fs", streams[total].duration);
                    printf(")\n");
                    total++;
                }
            }
            printf("Total tracks: %zu\n", scan.track_count);
        }
    }

    cJSON_Delete(draft);
    free(streams);
    musicpack_manifest_clear(&m);
    scan_result_clear(&scan);
    return bad ? 1 : 0;
}

/* ---- command: validate-draft -------------------------------------- */

static void
usage_validate_draft(void)
{
    fprintf(stderr, "usage: musicpack validate-draft --draft FILE [--json]\n");
}

static int
cmd_validate_draft(const char *draft_path, int json)
{
    cJSON *draft;
    char **errors = 0;
    char **warnings = 0;
    size_t ecount = 0, wcount = 0;
    int ok;
    char err[512];

    draft = draft_read_json(draft_path, err, sizeof err);
    if (draft == 0) {
        if (json)
            json_error_out("invalid_draft", err);
        else
            fprintf(stderr, "validate-draft: %s\n", err);
        return 1;
    }
    ok = draft_validate(draft, &errors, &ecount, &warnings, &wcount);

    if (json) {
        cJSON *root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "ok", ok ? 1 : 0);
        add_string_array(root, "errors", errors, ecount);
        add_string_array(root, "warnings", warnings, wcount);
        draft_print(root);
        cJSON_Delete(root);
    } else {
        size_t i;
        for (i = 0; i < ecount; i++)
            printf("error: %s\n", errors[i]);
        for (i = 0; i < wcount; i++)
            printf("warning: %s\n", warnings[i]);
        printf("validate-draft: %zu error(s), %zu warning(s)\n", ecount, wcount);
    }

    free_vec(errors, ecount);
    free_vec(warnings, wcount);
    cJSON_Delete(draft);
    return ok ? 0 : 1;
}

/* ---- command: build-draft ----------------------------------------- */

static void
usage_build_draft(void)
{
    fprintf(stderr,
        "usage: musicpack build-draft --draft FILE -o DIR [--no-loudness] [--json]\n");
}

/* Extracts the first embedded picture matching `role` from a source audio
   file (FLAC PICTURE blocks, or the APEv2 front cover) into
   `artdir/<role><ext>` and writes the resulting absolute path into \p target.
   Returns 1 on success. */
static int
extract_embedded_image(const char *srcpath, const char *role, const char *artdir,
                        char *target, size_t target_cap)
{
    const char *dot = strrchr(srcpath, '.');
    const unsigned char *img = 0;
    unsigned char *owned = 0;
    size_t img_len = 0;
    char extbuf[8];
    const char *ext = extbuf;

    snprintf(extbuf, sizeof extbuf, ".jpg");
    if (dot != 0 && strcmp(dot, ".flac") == 0) {
        musicpack_pictures pics;
        size_t k;
        memset(&pics, 0, sizeof pics);
        if (musicpack_flac_read_metadata(srcpath, 0, &pics) == MUSICPACK_OK) {
            for (k = 0; k < pics.count; k++) {
                if (strcmp(musicpack_meta_picture_role(pics.items[k].type), role) == 0) {
                    owned = (unsigned char *) malloc(pics.items[k].data_len);
                    if (owned == 0)
                        break;
                    memcpy(owned, pics.items[k].data, pics.items[k].data_len);
                    img = owned;
                    img_len = pics.items[k].data_len;
                    snprintf(extbuf, sizeof extbuf, "%s",
                             ext_for_mime(pics.items[k].mime));
                    break;
                }
            }
            musicpack_pictures_free(&pics);
        }
    } else if (dot != 0 && strcmp(dot, ".mpc") == 0 && strcmp(role, "front") == 0) {
        musicpack_tag_set tags;
        const musicpack_tag *cov;
        memset(&tags, 0, sizeof tags);
        if (musicpack_ape_read(srcpath, &tags) == MUSICPACK_OK) {
            cov = musicpack_tag_set_get(&tags, "Cover Art (Front)");
            if (cov != 0 && cov->is_binary && cov->binary_len > 0) {
                const unsigned char *nul =
                    (const unsigned char *) memchr(cov->binary, '\0', cov->binary_len);
                const unsigned char *body = nul != 0 ? nul + 1 : cov->binary;
                size_t body_len = nul != 0
                    ? cov->binary_len - (size_t) (nul - cov->binary) - 1
                    : cov->binary_len;
                const char *fname = (const char *) cov->binary;
                const char *fe = strrchr(fname, '.');
                if (body_len > 0) {
                    owned = (unsigned char *) malloc(body_len);
                    if (owned != 0) {
                        memcpy(owned, body, body_len);
                        img = owned;
                        img_len = body_len;
                        /* the extension lives inside the APE tag buffer, which is
                           freed below; copy it into a local buffer */
                        if (fe != 0 && *fe != '\0')
                            snprintf(extbuf, sizeof extbuf, "%s", fe);
                    }
                }
            }
        }
        musicpack_tag_set_free(&tags);
    }
    if (img == 0 ||
        !((img_len >= 3 && img[0] == 0xff && img[1] == 0xd8 && img[2] == 0xff) ||
          (img_len >= 8 && memcmp(img, "\x89PNG\r\n\x1a\n", 8) == 0))) {
        free(owned);
        return 0;
    }
    if (img[0] == 0xff)
        snprintf(extbuf, sizeof extbuf, ".jpg");
    else
        snprintf(extbuf, sizeof extbuf, ".png");
    snprintf(target, target_cap, "%s/%s%s", artdir, role, ext);
    {
        int ok = write_bytes(target, img, img_len) == 0;
        free(owned);
        return ok;
    }
}

/* Extracts the first embedded picture matching `role` from a source audio
   file (FLAC PICTURE blocks, or the APEv2 front cover) into `artdir` and
   adds it to the manifest. Returns 1 on success. */
static int
extract_embedded_artwork(const char *srcpath, const char *role, const char *artdir,
                         musicpack_manifest *m, char *err, size_t err_cap)
{
    char target[MUSICPACK_PATH_MAX + 2];

    if (!extract_embedded_image(srcpath, role, artdir, target, sizeof target)) {
        snprintf(err, err_cap, "no embedded '%s' artwork in '%s'", role, srcpath);
        return 0;
    }
    {
        char hex[MUSICPACK_SHA256_HEX_SIZE];
        char rel[MUSICPACK_PATH_MAX + 2];
        snprintf(rel, sizeof rel, "artwork/%s", strrchr(target, '/') + 1);
        if (manifest_add_artwork(m, role, rel,
                                 musicpack_sha256_file(target, hex, sizeof hex) ==
                                         MUSICPACK_OK
                                     ? hex
                                     : 0) != 0) {
            snprintf(err, err_cap, "out of memory");
            return 0;
        }
    }
    return 1;
}

/* Attaches a completed sonic document (draft `sonicAnalysis.path`) into the
   package: validates it against the built manifest, copies it to
   analysis/sonic.json and adds the analysis[] reference. Never fails the
   build — an unreadable, malformed or mismatched document is dropped with a
   clear warning and the package is built without sonic (per the sonic
   spec). Returns 1 when sonic was included, 0 when skipped (with a reason
   in \p warn). */
static int
attach_sonic_document(cJSON *draft, musicpack_manifest *m, const char *out_dir,
                      char *warn, size_t warn_cap)
{
    cJSON *sa;
    const char *path;
    char *json;
    musicpack_status s;
    musicpack_sonic *sonic;
    char dst[MUSICPACK_PATH_MAX + 2];
    char adir[MUSICPACK_PATH_MAX + 2];
    char hex[MUSICPACK_SHA256_HEX_SIZE];
    musicpack_analysis *na;

    sa = cJSON_GetObjectItemCaseSensitive(draft, "sonicAnalysis");
    if (!cJSON_IsObject(sa))
        return 0;
    path = djstr(sa, "path");
    if (path == 0 || *path == '\0')
        return 0;

    json = read_file_bounded(path, MUSICPACK_SONIC_DOC_MAX, &s);
    if (json == 0) {
        snprintf(warn, warn_cap,
                 "sonic analysis file '%s' cannot be read; building without sonic",
                 path);
        return 0;
    }
    sonic = musicpack_sonic_parse(json, strlen(json), &s);
    free(json);
    if (sonic == 0) {
        snprintf(warn, warn_cap,
                 "sonic analysis document is malformed; building without sonic");
        return 0;
    }
    if (musicpack_sonic_validate(sonic, m, 0) != MUSICPACK_OK) {
        snprintf(warn, warn_cap,
                 "sonic analysis does not match the package tracks; building without sonic");
        musicpack_sonic_free(sonic);
        return 0;
    }

    /* reserve the manifest slot before touching disk */
    na = (musicpack_analysis *) realloc(m->analysis,
                                        (m->analysis_count + 1) * sizeof *na);
    if (na == 0) {
        snprintf(warn, warn_cap, "out of memory adding sonic reference");
        musicpack_sonic_free(sonic);
        return 0;
    }
    m->analysis = na;
    na = &m->analysis[m->analysis_count];
    memset(na, 0, sizeof *na);

    snprintf(adir, sizeof adir, "%s/analysis", out_dir);
    if (mkdir_p(adir) != 0) {
        snprintf(warn, warn_cap, "cannot create analysis directory; building without sonic");
        musicpack_sonic_free(sonic);
        return 0;
    }
    snprintf(dst, sizeof dst, "%s/analysis/sonic.json", out_dir);
    if (copy_file(path, dst) != 0) {
        snprintf(warn, warn_cap, "cannot copy sonic analysis; building without sonic");
        musicpack_sonic_free(sonic);
        return 0;
    }
    if (musicpack_sha256_file(dst, hex, sizeof hex) != MUSICPACK_OK) {
        remove(dst);
        snprintf(warn, warn_cap, "cannot hash sonic analysis; building without sonic");
        musicpack_sonic_free(sonic);
        return 0;
    }

    na->type = strdup("sonic");
    na->profile = strdup(sonic->profile_id);
    na->asset.path = strdup("analysis/sonic.json");
    na->asset.sha256 = strdup(hex);
    m->analysis_count++;
    musicpack_sonic_free(sonic);
    return 1;
}

static int
cmd_build_draft(const char *draft_path, const char *out_dir, int no_loudness, int json)
{
    cJSON *draft;
    char err[512];
    char **errors = 0, **warnings = 0;
    size_t ecount = 0, wcount = 0;
    musicpack_manifest m;
    musicpack_meter *album_meter = 0;
    const char *source_root;
    const char *final_out = out_dir;
    char stage_dir[MUSICPACK_PATH_MAX + 2];
    char audio_dir[MUSICPACK_PATH_MAX + 2];
    char art_dir[MUSICPACK_PATH_MAX + 2];
    char lyr_dir[MUSICPACK_PATH_MAX + 2];
    char bok_dir[MUSICPACK_PATH_MAX + 2];
    char ex_dir[MUSICPACK_PATH_MAX + 2];
    char hex[MUSICPACK_SHA256_HEX_SIZE];
    char srcpath[MUSICPACK_PATH_MAX + 2];
    size_t d, t, i;
    int bad = 0, verified = 0, sonic_included = 0;
    char *sonic_hint = 0;
    size_t verr = 0, vwarn = 0;

    draft = draft_read_json(draft_path, err, sizeof err);
    if (draft == 0) {
        if (json)
            json_error_out("invalid_draft", err);
        else
            fprintf(stderr, "build-draft: %s\n", err);
        return 1;
    }

    /* never assemble a package that fails its own validation */
    if (!draft_validate(draft, &errors, &ecount, &warnings, &wcount)) {
        if (json) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", 0);
            add_string_array(root, "errors", errors, ecount);
            add_string_array(root, "warnings", warnings, wcount);
            draft_print(root);
            cJSON_Delete(root);
        } else {
            for (i = 0; i < ecount; i++)
                fprintf(stderr, "error: %s\n", errors[i]);
        }
        free_vec(errors, ecount);
        free_vec(warnings, wcount);
        cJSON_Delete(draft);
        return 1;
    }
    free_vec(errors, ecount);
    free_vec(warnings, wcount);

    source_root = djstr(draft, "sourceRoot");
    if (source_root == 0 || *source_root == '\0') {
        if (json)
            json_error_out("invalid_draft", "draft has no sourceRoot");
        else
            fprintf(stderr, "build-draft: draft has no sourceRoot\n");
        cJSON_Delete(draft);
        return 1;
    }
    if (is_dir_path(final_out) || is_regular_path(final_out)) {
        if (json)
            json_error_out("destination_exists", "output destination already exists");
        else
            fprintf(stderr, "build-draft: output destination already exists\n");
        cJSON_Delete(draft);
        return 1;
    }
    if (reject_path_overlap(source_root, final_out, "build-draft")) {
        cJSON_Delete(draft);
        return 1;
    }
    if (snprintf(stage_dir, sizeof stage_dir, "%s.build-%ld", final_out,
                 (long) getpid()) >= (int) sizeof stage_dir ||
        is_dir_path(stage_dir) || is_regular_path(stage_dir)) {
        fprintf(stderr, "build-draft: cannot allocate staging directory\n");
        cJSON_Delete(draft);
        return 1;
    }
    out_dir = stage_dir;

    memset(&m, 0, sizeof m);
    if (!draft_to_manifest(draft, &m)) {
        if (json)
            json_error_out("invalid_draft", "cannot interpret draft");
        else
            fprintf(stderr, "build-draft: cannot interpret draft\n");
        cJSON_Delete(draft);
        return 1;
    }

    snprintf(audio_dir, sizeof audio_dir, "%s/audio", out_dir);
    snprintf(art_dir, sizeof art_dir, "%s/artwork", out_dir);
    snprintf(lyr_dir, sizeof lyr_dir, "%s/lyrics", out_dir);
    snprintf(bok_dir, sizeof bok_dir, "%s/booklet", out_dir);
    snprintf(ex_dir, sizeof ex_dir, "%s/extras", out_dir);
    if (mkdir_p(out_dir) != 0 || mkdir_p(audio_dir) != 0 ||
        mkdir_p(art_dir) != 0 || mkdir_p(lyr_dir) != 0 ||
        mkdir_p(bok_dir) != 0 || mkdir_p(ex_dir) != 0) {
        snprintf(err, sizeof err, "cannot create package directory '%s'", out_dir);
        bad = 1;
        goto done;
    }

    for (d = 0; d < m.disc_count && !bad; d++) {
        for (t = 0; t < m.discs[d].track_count && !bad; t++) {
            musicpack_track *tr = &m.discs[d].tracks[t];
            const char *rel = tr->audio.path != 0 ? tr->audio.path : "";
            const char *base, *dot;
            char fname[MUSICPACK_PATH_MAX + 2];
            char target[MUSICPACK_PATH_MAX + 2];
            if (*rel == '\0') {
                snprintf(err, sizeof err, "track %d on disc %d has no audio file",
                         tr->number, m.discs[d].disc);
                bad = 1;
                break;
            }
            snprintf(srcpath, sizeof srcpath, "%s/%s", source_root, rel);
            base = strrchr(rel, '/');
            base = base != 0 ? base + 1 : rel;
            dot = strrchr(base, '.');
            snprintf(fname, sizeof fname, "%s", tr->title != 0 ? tr->title : "");
            sanitize_component(fname);
            /* object paths must be unique across the package, so a multi-disc
               album prefixes the disc number; a single disc keeps the flat
               "NN - Title.ext" name. */
            if (m.disc_count > 1)
                snprintf(target, sizeof target, "%s/%d-%02d - %s%s", audio_dir,
                         m.discs[d].disc, tr->number, fname, dot != 0 ? dot : "");
            else
                snprintf(target, sizeof target, "%s/%02d - %s%s", audio_dir, tr->number,
                         fname, dot != 0 ? dot : "");
            if (copy_file(srcpath, target) != 0) {
                snprintf(err, sizeof err, "cannot copy '%s'", srcpath);
                bad = 1;
                break;
            }
            free(tr->audio.path);
            tr->audio.path = strdup(target + strlen(out_dir) + 1);
            if (musicpack_sha256_file(target, hex, sizeof hex) != MUSICPACK_OK) {
                snprintf(err, sizeof err, "cannot hash '%s'", target);
                bad = 1;
                break;
            }
            free(tr->audio.sha256);
            tr->audio.sha256 = strdup(hex);
            if (!no_loudness) {
                int has_l;
                double lufs, peak, dur = 0;
                if (measure_loudness(target, &has_l, &lufs, &peak, &dur,
                                     &album_meter) == 0 && has_l) {
                    tr->loudness.present = 1;
                    tr->loudness.lufs = lufs;
                    tr->loudness.true_peak_db = peak;
                    if (dur > 0) {
                        tr->has_duration = 1;
                        tr->duration = dur;
                    }
                }
            }
        }
    }

    /* artwork: copy file-based entries, extract embedded ones */
    for (i = 0; i < m.artwork_count && !bad; i++) {
        musicpack_artwork *a = &m.artwork[i];
        const char *ext = strrchr(a->asset.path, '.');
        char target[MUSICPACK_PATH_MAX + 2];
        snprintf(srcpath, sizeof srcpath, "%s/%s", source_root, a->asset.path);
        snprintf(target, sizeof target, "%s/%s%s", art_dir, a->role,
                 ext != 0 ? ext : ".jpg");
        if (!unique_target(target, sizeof target)) {
            snprintf(err, sizeof err, "cannot name artwork '%s'", a->asset.path);
            bad = 1;
            break;
        }
        if (copy_file(srcpath, target) != 0) {
            snprintf(err, sizeof err, "cannot copy artwork '%s'", a->asset.path);
            bad = 1;
            break;
        }
        free(a->asset.path);
        a->asset.path = strdup(target + strlen(out_dir) + 1);
        free(a->asset.sha256);
        if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
            a->asset.sha256 = strdup(hex);
    }
    if (!bad) {
        cJSON *artarr = cJSON_GetObjectItemCaseSensitive(draft, "artwork");
        if (cJSON_IsArray(artarr)) {
            cJSON *a;
            cJSON_ArrayForEach(a, artarr) {
                const char *rel = djstr(a, "path");
                const char *sa = djstr(a, "sourceAudio");
                const char *role = djstr(a, "role");
                if (rel != 0 && *rel != '\0')
                    continue; /* file-based entries handled above */
                if (sa == 0 || *sa == '\0' || role == 0 || *role == '\0')
                    continue;
                snprintf(srcpath, sizeof srcpath, "%s/%s", source_root, sa);
                if (!extract_embedded_artwork(srcpath, role, art_dir, &m,
                                              err, sizeof err)) {
                    bad = 1;
                    break;
                }
            }
        }
    }

    if (!bad) {
        for (i = 0; i < m.booklet_count; i++) {
            char target[MUSICPACK_PATH_MAX + 2];
            snprintf(srcpath, sizeof srcpath, "%s/%s", source_root, m.booklet[i].path);
            snprintf(target, sizeof target, "%s/%s", bok_dir,
                     strrchr(m.booklet[i].path, '/') != 0
                         ? strrchr(m.booklet[i].path, '/') + 1
                          : m.booklet[i].path);
            if (!unique_target(target, sizeof target)) { bad = 1; break; }
            if (copy_file(srcpath, target) != 0) {
                snprintf(err, sizeof err, "cannot copy booklet '%s'", m.booklet[i].path);
                bad = 1;
                break;
            }
            free(m.booklet[i].path);
            m.booklet[i].path = strdup(target + strlen(out_dir) + 1);
            free(m.booklet[i].sha256);
            if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                m.booklet[i].sha256 = strdup(hex);
        }
    }
    if (!bad) {
        for (i = 0; i < m.lyrics_count; i++) {
            char target[MUSICPACK_PATH_MAX + 2];
            snprintf(srcpath, sizeof srcpath, "%s/%s", source_root, m.lyrics[i].path);
            snprintf(target, sizeof target, "%s/%s", lyr_dir,
                     strrchr(m.lyrics[i].path, '/') != 0
                         ? strrchr(m.lyrics[i].path, '/') + 1
                          : m.lyrics[i].path);
            if (!unique_target(target, sizeof target)) { bad = 1; break; }
            if (copy_file(srcpath, target) != 0) {
                snprintf(err, sizeof err, "cannot copy lyrics '%s'", m.lyrics[i].path);
                bad = 1;
                break;
            }
            free(m.lyrics[i].path);
            m.lyrics[i].path = strdup(target + strlen(out_dir) + 1);
            free(m.lyrics[i].sha256);
            if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                m.lyrics[i].sha256 = strdup(hex);
        }
    }
    if (!bad) {
        for (i = 0; i < m.extras_count; i++) {
            char target[MUSICPACK_PATH_MAX + 2];
            snprintf(srcpath, sizeof srcpath, "%s/%s", source_root, m.extras[i].path);
            snprintf(target, sizeof target, "%s/%s", ex_dir,
                     strrchr(m.extras[i].path, '/') != 0
                         ? strrchr(m.extras[i].path, '/') + 1
                          : m.extras[i].path);
            if (!unique_target(target, sizeof target)) { bad = 1; break; }
            if (copy_file(srcpath, target) != 0) {
                snprintf(err, sizeof err, "cannot copy extra '%s'", m.extras[i].path);
                bad = 1;
                break;
            }
            free(m.extras[i].path);
            m.extras[i].path = strdup(target + strlen(out_dir) + 1);
            free(m.extras[i].sha256);
            if (musicpack_sha256_file(target, hex, sizeof hex) == MUSICPACK_OK)
                m.extras[i].sha256 = strdup(hex);
        }
    }

    if (!bad && album_meter != 0) {
        double alufs, apeak;
        if (musicpack_meter_result(album_meter, &alufs, &apeak) == MUSICPACK_OK) {
            m.has_album_loudness = 1;
            m.album_loudness.lufs = alufs;
            m.album_loudness.true_peak_db = apeak;
            m.loudness_algorithm = strdup(MUSICPACK_LOUDNESS_STANDARD);
        }
    }

    if (!bad) {
        char sonichint[512];
        sonichint[0] = '\0';
        if (attach_sonic_document(draft, &m, out_dir, sonichint, sizeof sonichint)) {
            sonic_included = 1;
        } else if (sonichint[0] != '\0') {
            sonic_hint = strdup(sonichint);
        }
    }

    if (!bad) {
        char *manifest_json = 0;
        char mpath[MUSICPACK_PATH_MAX + 2];
        if (musicpack_manifest_write(&m, &manifest_json) == MUSICPACK_OK) {
            snprintf(mpath, sizeof mpath, "%s/manifest.json", out_dir);
            if (write_all(mpath, manifest_json) != 0) {
                snprintf(err, sizeof err, "cannot write manifest.json");
                bad = 1;
            }
            free(manifest_json);
        } else {
            snprintf(err, sizeof err, "cannot serialize manifest");
            bad = 1;
        }
    }

    if (!bad) {
        musicpack_package *pkg = musicpack_package_open_dir(out_dir, 0);
        if (pkg == 0) {
            snprintf(err, sizeof err, "created package cannot be opened");
            bad = 1;
        } else {
            musicpack_report rep = { 0, 0 };
            musicpack_status s = musicpack_package_verify(pkg, &rep, 0, 0);
            verified = s == MUSICPACK_OK;
            verr = rep.errors;
            vwarn = rep.warnings;
            musicpack_package_close(pkg);
            if (!verified) {
                snprintf(err, sizeof err,
                         "created package failed verification (%zu error(s), %zu warning(s))",
                         verr, vwarn);
                bad = 1;
            }
        }
    }

    if (!bad && rename(out_dir, final_out) != 0) {
        snprintf(err, sizeof err, "cannot finalize package directory '%s'", final_out);
        bad = 1;
    }

done:
    musicpack_manifest_clear(&m);
    musicpack_meter_free(album_meter);
    cJSON_Delete(draft);
    if (bad)
        rm_rf(out_dir);

    if (json) {
        cJSON *root = cJSON_CreateObject();
        if (bad) {
            cJSON *e = cJSON_AddObjectToObject(root, "error");
            cJSON_AddStringToObject(e, "code", "build_failed");
            cJSON_AddStringToObject(e, "message", err[0] != '\0' ? err : "build failed");
        } else {
            cJSON *v = cJSON_AddObjectToObject(root, "verify");
            cJSON_AddBoolToObject(root, "ok", 1);
            cJSON_AddStringToObject(root, "outputPath", final_out);
            cJSON_AddNumberToObject(v, "errors", (double) verr);
            cJSON_AddNumberToObject(v, "warnings", (double) vwarn);
            cJSON_AddBoolToObject(root, "sonic", sonic_included ? 1 : 0);
            if (sonic_hint != 0)
                cJSON_AddStringToObject(root, "sonicWarning", sonic_hint);
        }
        draft_print(root);
        cJSON_Delete(root);
    } else {
        if (bad)
            fprintf(stderr, "build-draft: %s\n", err[0] != '\0' ? err : "build failed");
        else {
            if (sonic_included)
                printf("sonic analysis: included\n");
            if (sonic_hint != 0)
                fprintf(stderr, "build-draft: warning: %s\n", sonic_hint);
            printf("created package '%s' (%zu error(s), %zu warning(s))\n",
                   final_out, verr, vwarn);
        }
    }
    free(sonic_hint);
    return bad ? 1 : 0;
}

/* ---- command: identify-draft -------------------------------------- */

static void
usage_identify_draft(void)
{
    fprintf(stderr,
        "usage: musicpack identify-draft --draft FILE\n"
        "       (--mbid UUID | --barcode BC | --mb-json FILE) [--json]\n");
}

static int
cmd_identify_draft(const char *draft_path, const char *mbid, const char *barcode,
                   const char *mb_json_path, int json)
{
    cJSON *draft, *out = 0;
    char err[512];
    musicpack_manifest m;
    const char *conf = "none";
    int applied = 0;
    int rc = 0;

    draft = draft_read_json(draft_path, err, sizeof err);
    if (draft == 0) {
        if (json)
            json_error_out("invalid_draft", err);
        else
            fprintf(stderr, "identify-draft: %s\n", err);
        return 1;
    }
    memset(&m, 0, sizeof m);
    if (!draft_to_manifest(draft, &m)) {
        cJSON_Delete(draft);
        if (json)
            json_error_out("invalid_draft", "cannot interpret draft");
        else
            fprintf(stderr, "identify-draft: cannot interpret draft\n");
        return 1;
    }

    if (mb_json_path != 0) {
        musicpack_status st = MUSICPACK_OK;
        char *mbjson = read_file_bounded(mb_json_path, 8u * 1024u * 1024u, &st);
        if (mbjson == 0) {
            snprintf(err, sizeof err, "cannot read '%s'", mb_json_path);
            rc = 1;
            goto done;
        }
        conf = musicpack_mb_match_confidence(mbjson, &m);
        if (strcmp(conf, "none") != 0) {
            musicpack_mb_apply_release(mbjson, &m);
            applied = 1;
        }
        free(mbjson);
    } else if (mbid != 0) {
        char url[512];
        char *mbjson;
        snprintf(url, sizeof url,
                 "https://musicbrainz.org/ws/2/release/%s?inc=artist-credits+labels+recordings+media&fmt=json",
                 mbid);
        mbjson = curl_fetch(url, 1u * 1024u * 1024u);
        if (mbjson == 0) {
            snprintf(err, sizeof err, "network lookup failed; identity unchanged");
            rc = 1;
            goto done;
        }
        /* the user asserted this release id: record it so the match is exact */
        free(m.musicbrainz_release_id);
        m.musicbrainz_release_id = strdup(mbid);
        conf = musicpack_mb_match_confidence(mbjson, &m);
        if (strcmp(conf, "none") != 0) {
            musicpack_mb_apply_release(mbjson, &m);
            applied = 1;
        }
        free(mbjson);
    } else if (barcode != 0) {
        char url[512];
        char *mbjson;
        cJSON *candidates;
        snprintf(url, sizeof url,
                 "https://musicbrainz.org/ws/2/release/?query=barcode:%s&fmt=json&limit=5",
                 barcode);
        mbjson = curl_fetch(url, 1u * 1024u * 1024u);
        if (mbjson == 0) {
            snprintf(err, sizeof err, "network lookup failed");
            rc = 1;
            goto done;
        }
        candidates = mb_candidates(mbjson, &m);
        free(mbjson);
        if (candidates == 0) {
            snprintf(err, sizeof err, "cannot parse MusicBrainz response");
            rc = 1;
            goto done;
        }
        out = cJSON_CreateObject();
        cJSON_AddItemToObject(out, "candidates", candidates);
        draft_print(out);
        cJSON_Delete(out);
        cJSON_Delete(draft);
        musicpack_manifest_clear(&m);
        return 0;
    } else {
        usage_identify_draft();
        rc = 2;
        goto done;
    }

    if (applied) {
        /* a MusicBrainz match supersedes the draft's initial local/none
           identity: record source and the evidence-based confidence */
        free(m.identity_source);
        m.identity_source = strdup("musicbrainz");
        free(m.identity_confidence);
        m.identity_confidence = strdup(conf);
        draft_apply_manifest(draft, &m);
    }

    if (json) {
        out = cJSON_CreateObject();
        cJSON_AddItemToObject(out, "draft", draft);
        draft = 0; /* ownership moved into out */
        cJSON_AddStringToObject(out, "confidence", conf);
        cJSON_AddBoolToObject(out, "applied", applied ? 1 : 0);
        draft_print(out);
        cJSON_Delete(out);
        out = 0;
    } else {
        printf("identify-draft: %s\n", applied ? conf : "no match applied");
    }

done:
    if (json && out == 0 && rc != 0) {
        json_error_out("identify_failed", err);
    } else if (!json && rc != 0) {
        fprintf(stderr, "identify-draft: %s\n", err[0] != '\0' ? err : "failed");
    }
    cJSON_Delete(draft);
    cJSON_Delete(out);
    musicpack_manifest_clear(&m);
    return rc;
}

/* ------------------------------------------------------------------ */
/* command: encode-draft (FLAC/WAV -> Musepack q6 stage)               */
/* ------------------------------------------------------------------ */
/*                                                                      */
/* The encode stage turns lossless source tracks into tagged Musepack   */
/* SV8 files in a staging directory and returns a transformed draft     */
/* whose audioPath values point at the encoded files. `build-draft`     */
/* then assembles the package from that staging area untouched.         */
/*                                                                      */
/* Progress protocol (one JSON object per line on stdout):              */
/*   {"event":"stage","stage":"decoding|encoding|tagging","done":i,     */
/*    "total":n,"disc":d,"track":t,"title":"..."}                       */
/*   {"event":"track","done":i,"total":n,"disc":d,"track":t,            */
/*    "title":"...","status":"ok","sha256":"...","duration":...}        */
/*   {"event":"done","ok":true,"outputDir":"...","tracks":n,            */
/*    "draft":{...transformed draft...}}                                */
/*   {"event":"error","code":"...","message":"...","disc":d,"track":t}  */
/*   {"event":"cancelled"}                       // SIGTERM, exit 130   */
/*                                                                      */
/* Musepack is a fixed-rate subband codec: only 32/37.8/44.1/48 kHz     */
/* sources can be encoded. FLAC and WAV are the supported MVP sources.  */

#if defined(_WIN32)
static volatile int g_encode_cancelled = 0;
#else
static volatile sig_atomic_t g_encode_cancelled = 0;
static volatile pid_t g_encode_child = 0;

static void
on_encode_sigterm(int sig)
{
    (void) sig;
    g_encode_cancelled = 1;
    if (g_encode_child > 0)
        kill((pid_t) g_encode_child, SIGTERM);
}
#endif

static int
encode_supported_rate(long rate)
{
    return rate == 32000 || rate == 37800 || rate == 44100 || rate == 48000;
}

static void
emit_encode_json(cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    if (s != 0) {
        printf("%s\n", s);
        free(s);
        fflush(stdout);
    }
}

static void
emit_encode_stage(const char *stage, int done, int total, int disc, int track,
                  const char *title)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "stage");
    cJSON_AddStringToObject(o, "stage", stage);
    cJSON_AddNumberToObject(o, "done", done);
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddNumberToObject(o, "disc", disc);
    cJSON_AddNumberToObject(o, "track", track);
    cJSON_AddStringToObject(o, "title", title != 0 ? title : "");
    emit_encode_json(o);
    cJSON_Delete(o);
}

static void
emit_encode_track(int done, int total, int disc, int track, const char *title,
                  const char *status, const char *sha256, double duration)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "track");
    cJSON_AddNumberToObject(o, "done", done);
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddNumberToObject(o, "disc", disc);
    cJSON_AddNumberToObject(o, "track", track);
    cJSON_AddStringToObject(o, "title", title != 0 ? title : "");
    cJSON_AddStringToObject(o, "status", status);
    if (sha256 != 0)
        cJSON_AddStringToObject(o, "sha256", sha256);
    if (duration > 0)
        cJSON_AddNumberToObject(o, "duration", duration);
    emit_encode_json(o);
    cJSON_Delete(o);
}

static void
emit_encode_error(const char *code, const char *message, int disc, int track)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "event", "error");
    cJSON_AddStringToObject(o, "code", code);
    cJSON_AddStringToObject(o, "message", message);
    if (disc > 0)
        cJSON_AddNumberToObject(o, "disc", disc);
    if (track > 0)
        cJSON_AddNumberToObject(o, "track", track);
    emit_encode_json(o);
    cJSON_Delete(o);
}

/* Runs argv[0..] to completion and returns its exit status, or -1 when the
   process could not be spawned. On POSIX the child pid is tracked so SIGTERM
   can kill the active tool (which is what makes cancellation land fast). */
static int
run_command(char *const argv[])
{
#if defined(_WIN32)
    char cmd[16384];
    size_t n = 0, i, k;
    cmd[0] = '\0';
    for (i = 0; argv[i] != 0; i++) {
        const char *arg = argv[i];
        size_t len = strlen(arg);
        if (n + len + 8 >= sizeof cmd)
            return -1;
        if (i > 0)
            cmd[n++] = ' ';
        if (strchr(arg, ' ') != 0) {
            cmd[n++] = '"';
            for (k = 0; arg[k] != '\0'; k++) {
                if (arg[k] == '"')
                    cmd[n++] = '"';
                cmd[n++] = arg[k];
            }
            cmd[n++] = '"';
        } else {
            for (k = 0; arg[k] != '\0'; k++)
                cmd[n++] = arg[k];
        }
        cmd[n] = '\0';
    }
    {
        FILE *p = POPEN(cmd, "w");
        if (p == 0)
            return -1;
        return PCLOSE(p);
    }
#else
    pid_t pid = fork();
    int status = 0;
    if (pid < 0)
        return -1;
    if (pid == 0) {
        execvp(argv[0], argv);
        _exit(127);
    }
    g_encode_child = pid;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }
    g_encode_child = 0;
    if (WIFEXITED(status))
        return WEXITSTATUS(status);
    if (WIFSIGNALED(status))
        return 128 + WTERMSIG(status);
    return -1;
#endif
}

#ifndef X_OK
# define X_OK 0
#endif

/* Locates an executable on PATH (or returns it directly when it contains a
   path separator). Returns an owned string, or NULL. */
static char *
find_executable(const char *name)
{
    char buf[MUSICPACK_PATH_MAX + 2];
    const char *path;
    const char *p;

    if (name == 0 || *name == '\0')
        return 0;
    if (strchr(name, '/') != 0 || strchr(name, '\\') != 0)
        return access(name, X_OK) == 0 ? strdup(name) : 0;
    path = getenv("PATH");
    if (path == 0)
        return 0;
    p = path;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        size_t len = colon != 0 ? (size_t) (colon - p) : strlen(p);
        if (len > 0 && len + 2 < sizeof buf) {
            memcpy(buf, p, len);
            buf[len] = '\0';
            snprintf(buf + len, sizeof buf - len, "/%s", name);
            if (access(buf, X_OK) == 0)
                return strdup(buf);
        }
        if (colon == 0)
            break;
        p = colon + 1;
    }
    return 0;
}

/* Resolves an explicit binary path or a PATH lookup into \p buf. Returns the
   resolved string (either \p buf or the explicit argument). */
static const char *
resolve_bin(const char *explicit, const char *name, char *buf, size_t cap)
{
    if (explicit != 0 && *explicit != '\0') {
        snprintf(buf, cap, "%s", explicit);
        return buf;
    }
    {
        char *found = find_executable(name);
        if (found != 0) {
            snprintf(buf, cap, "%s", found);
            free(found);
            return buf;
        }
    }
    snprintf(buf, cap, "%s", name);
    return buf;
}

/* Returns 1 when \p resolved (from resolve_bin) is usable. */
static int
bin_available(const char *resolved)
{
    if (resolved == 0 || *resolved == '\0')
        return 0;
    if (strchr(resolved, '/') != 0 || strchr(resolved, '\\') != 0)
        return access(resolved, X_OK) == 0;
    return find_executable(resolved) != 0;
}

/* Sample rate from a minimal RIFF/WAVE header parse (0 when unknown). */
static long
wav_sample_rate(const char *path)
{
    unsigned char h[128];
    FILE *f = fopen(path, "rb");
    size_t got;
    long rate = 0;
    if (f == 0)
        return 0;
    got = fread(h, 1, sizeof h, f);
    fclose(f);
    if (got < 12 || memcmp(h, "RIFF", 4) != 0 || memcmp(h + 8, "WAVE", 4) != 0)
        return 0;
    {
        unsigned int pos = 12;
        while (pos + 8 <= got) {
            unsigned int clen = (unsigned int) h[pos + 4] |
                                ((unsigned int) h[pos + 5] << 8) |
                                ((unsigned int) h[pos + 6] << 16) |
                                ((unsigned int) h[pos + 7] << 24);
            if (memcmp(h + pos, "fmt ", 4) == 0 && pos + 16 <= got) {
                rate = (long) h[pos + 12] | ((long) h[pos + 13] << 8) |
                       ((long) h[pos + 14] << 16) | ((long) h[pos + 15] << 24);
                break;
            }
            pos += 8 + clen;
        }
    }
    return rate;
}

/* Recursively removes a directory tree (staging cleanup only). */
static void
rm_rf(const char *path)
{
#if defined(_WIN32)
    char pat[MUSICPACK_PATH_MAX + 2];
    finddata_t fd;
    intptr_t h;
    snprintf(pat, sizeof pat, "%s/*", path);
    h = _findfirst(pat, &fd);
    if (h != -1) {
        do {
            char next[MUSICPACK_PATH_MAX + 2];
            if (strcmp(fd.name, ".") == 0 || strcmp(fd.name, "..") == 0)
                continue;
            snprintf(next, sizeof next, "%s/%s", path, fd.name);
            if (fd.attrib & _A_SUBDIR)
                rm_rf(next);
            else
                remove(next);
        } while (_findnext(h, &fd) == 0);
        _findclose(h);
    }
    _rmdir(path);
#else
    DIR *d = opendir(path);
    struct dirent *e;
    if (d == 0)
        return;
    while ((e = readdir(d)) != 0) {
        struct stat st;
        char next[MUSICPACK_PATH_MAX + 2];
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0)
            continue;
        snprintf(next, sizeof next, "%s/%s", path, e->d_name);
        if (lstat(next, &st) != 0)
            continue;
        if (S_ISDIR(st.st_mode))
            rm_rf(next);
        else
            remove(next);
    }
    closedir(d);
    rmdir(path);
#endif
}

/* Canonical APE/Vorbis keys handled by the manifest projection or by the
   MusicPack loudness rules. Anything else in a source tag set is passed
   through verbatim so unknown metadata is preserved rather than dropped. */
static const char *const APE_CANONICAL_KEYS[] = {
    "ALBUM", "ALBUMARTIST", "ALBUM ARTIST", "TITLE", "ARTIST",
    "TRACKNUMBER", "TRACK", "TRACKTOTAL", "TOTALTRACKS",
    "DISCNUMBER", "DISC", "DISCTOTAL", "TOTALDISCS",
    "DATE", "YEAR", "ORIGINALDATE", "ORIGINAL YEAR",
    "GENRE", "PUBLISHER", "LABEL", "ORGANIZATION",
    "CATALOGNUMBER", "CATALOGUENUMBER", "CATALOG #", "BARCODE",
    "ISRC", "COMPOSER", "PERFORMER", "CONDUCTOR", "REMIXER", "AUTHOR",
    "MUSICBRAINZ_ALBUMID", "MUSICBRAINZ_RELEASEID", "MUSICBRAINZ ALBUM ID",
    "MUSICBRAINZ_RELEASEGROUPID", "RELEASEGROUPID",
    "MUSICBRAINZ RELEASE GROUP ID",
    "MUSICBRAINZ_RECORDINGID", "MUSICBRAINZ RECORDING ID",
    "MUSICBRAINZ_TRACKID", "MUSICBRAINZ TRACK ID",
    "MUSICBRAINZ_RELEASETRACKID", "MUSICBRAINZ RELEASE TRACK ID",
    "MUSICBRAINZ_ALBUMTYPE", "RELEASETYPE", "MUSICBRAINZ ALBUM TYPE",
    "MUSICBRAINZ_ALBUMCOUNTRY", "RELEASECOUNTRY", "MUSICBRAINZ ALBUM COUNTRY",
    "SOURCE", "SOURCEID",
    "REPLAYGAIN_TRACK_GAIN", "REPLAYGAIN_TRACK_PEAK", "REPLAYGAIN_ALBUM_GAIN",
    "REPLAYGAIN_ALBUM_PEAK", "REPLAYGAIN_REFERENCE_LOUDNESS",
    "R128_TRACK_GAIN", "R128_ALBUM_GAIN", "R128_TRACK_PEAK", "R128_ALBUM_PEAK",
    "COVER ART (FRONT)",
};

static int
ape_key_is_canonical(const char *key)
{
    char upper[256];
    size_t i, n = strlen(key), k;
    if (n == 0 || n >= sizeof upper)
        return 0;
    for (i = 0; i < n; i++) {
        unsigned char c = (unsigned char) key[i];
        upper[i] = (c >= 'a' && c <= 'z') ? (char) (c - 'a' + 'A') : (char) c;
    }
    upper[n] = '\0';
    for (k = 0; k < sizeof APE_CANONICAL_KEYS / sizeof *APE_CANONICAL_KEYS; k++)
        if (strcmp(upper, APE_CANONICAL_KEYS[k]) == 0)
            return 1;
    return 0;
}

/* Copies source text tags that the canonical mapping does not handle into the
   target APE set verbatim (semantic passthrough, mirroring flac2mpc). */
static void
merge_passthrough_tags(const musicpack_tag_set *src, musicpack_tag_set *ape)
{
    size_t i;
    if (src == 0)
        return;
    for (i = 0; i < src->count; i++) {
        const musicpack_tag *t = &src->items[i];
        if (t->is_binary || t->value == 0 || *t->value == '\0')
            continue;
        if (ape_key_is_canonical(t->key))
            continue;
        musicpack_tag_set_add(ape, t->key, t->value, t->value_len);
    }
}

/* Copies/extracts every artwork entry into the staging artwork directory and
   rewrites the draft's artwork entries to be file-based under it. Returns 1
   on success. */
static int
stage_artwork(cJSON *draft, const char *source_root, const char *art_dir)
{
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(draft, "artwork");
    cJSON *a;
    if (!cJSON_IsArray(arr))
        return 1;
    cJSON_ArrayForEach(a, arr) {
        const char *role = djstr(a, "role");
        const char *p = djstr(a, "path");
        const char *sa = djstr(a, "sourceAudio");
        char src[MUSICPACK_PATH_MAX + 2];
        char target[MUSICPACK_PATH_MAX + 2];
        char rel[MUSICPACK_PATH_MAX + 2];
        if (role == 0 || *role == '\0')
            continue;
        if (p != 0 && *p != '\0') {
            const char *ext = strrchr(p, '.');
            snprintf(src, sizeof src, "%s/%s", source_root, p);
            snprintf(target, sizeof target, "%s/%s%s", art_dir, role,
                     ext != 0 ? ext : ".img");
            if (!unique_target(target, sizeof target))
                return 0;
            if (copy_file(src, target) != 0)
                return 0;
        } else if (sa != 0 && *sa != '\0') {
            snprintf(src, sizeof src, "%s/%s", source_root, sa);
            if (!extract_embedded_image(src, role, art_dir, target, sizeof target))
                return 0;
        } else {
            continue;
        }
        snprintf(rel, sizeof rel, "artwork/%s", strrchr(target, '/') + 1);
        cJSON_DeleteItemFromObject(a, "path");
        cJSON_DeleteItemFromObject(a, "embedded");
        cJSON_DeleteItemFromObject(a, "sourceAudio");
        cJSON_AddStringToObject(a, "path", rel);
    }
    return 1;
}

/* Copies a booklet/lyrics/extras array into its staging directory and rewrites
   each entry's path to be staging-relative. */
static int
stage_asset_dir(cJSON *draft, const char *key, const char *prefix,
                const char *source_root, const char *dir)
{
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(draft, key);
    cJSON *it;
    if (!cJSON_IsArray(arr))
        return 1;
    cJSON_ArrayForEach(it, arr) {
        const char *p = djstr(it, "path");
        const char *base;
        char src[MUSICPACK_PATH_MAX + 2];
        char target[MUSICPACK_PATH_MAX + 2];
        char rel[MUSICPACK_PATH_MAX + 2];
        if (p == 0 || *p == '\0')
            continue;
        base = strrchr(p, '/');
        base = base != 0 ? base + 1 : p;
        snprintf(src, sizeof src, "%s/%s", source_root, p);
        snprintf(target, sizeof target, "%s/%s", dir, base);
        if (!unique_target(target, sizeof target))
            return 0;
        if (copy_file(src, target) != 0)
            return 0;
        snprintf(rel, sizeof rel, "%s/%s", prefix, strrchr(target, '/') + 1);
        cJSON_DeleteItemFromObject(it, "path");
        cJSON_AddStringToObject(it, "path", rel);
    }
    return 1;
}

static void
usage_encode_draft(void)
{
    fprintf(stderr,
        "usage: musicpack encode-draft --draft FILE -o STAGING_DIR\n"
        "       [--quality 6.0] [--ffmpeg BIN] [--mpcenc BIN] [--json]\n");
}

static int
cmd_encode_draft(const char *draft_path, const char *out_dir, const char *quality,
                 const char *ffmpeg_arg, const char *mpcenc_arg, int json)
{
    cJSON *draft;
    char err[512];
    char **errors = 0, **warnings = 0;
    size_t ecount = 0, wcount = 0;
    musicpack_manifest m;
    const char *source_root;
    char audio_dir[MUSICPACK_PATH_MAX + 2];
    char art_dir[MUSICPACK_PATH_MAX + 2];
    char bok_dir[MUSICPACK_PATH_MAX + 2];
    char lyr_dir[MUSICPACK_PATH_MAX + 2];
    char ex_dir[MUSICPACK_PATH_MAX + 2];
    char ffmpeg_buf[MUSICPACK_PATH_MAX + 2];
    char mpcenc_buf[MUSICPACK_PATH_MAX + 2];
    const char *ffmpeg_bin;
    const char *mpcenc_bin;
    char srcpath[MUSICPACK_PATH_MAX + 2];
    char wavpath[MUSICPACK_PATH_MAX + 2];
    char hex[MUSICPACK_SHA256_HEX_SIZE];
    size_t d, t, i, total = 0;
    int done_count = 0;
    int bad = 0;
    char qbuf[32];

    draft = draft_read_json(draft_path, err, sizeof err);
    if (draft == 0) {
        if (json)
            json_error_out("invalid_draft", err);
        else
            fprintf(stderr, "encode-draft: %s\n", err);
        return 1;
    }

    /* never encode a draft that fails its own validation */
    if (!draft_validate(draft, &errors, &ecount, &warnings, &wcount)) {
        if (json) {
            cJSON *root = cJSON_CreateObject();
            cJSON_AddBoolToObject(root, "ok", 0);
            add_string_array(root, "errors", errors, ecount);
            add_string_array(root, "warnings", warnings, wcount);
            draft_print(root);
            cJSON_Delete(root);
        } else {
            for (i = 0; i < ecount; i++)
                fprintf(stderr, "error: %s\n", errors[i]);
        }
        free_vec(errors, ecount);
        free_vec(warnings, wcount);
        cJSON_Delete(draft);
        return 1;
    }
    free_vec(errors, ecount);
    free_vec(warnings, wcount);

    source_root = djstr(draft, "sourceRoot");
    if (source_root == 0 || *source_root == '\0') {
        if (json)
            json_error_out("invalid_draft", "draft has no sourceRoot");
        else
            fprintf(stderr, "encode-draft: draft has no sourceRoot\n");
        cJSON_Delete(draft);
        return 1;
    }
    if (is_dir_path(out_dir) || is_regular_path(out_dir)) {
        if (json)
            json_error_out("destination_exists", "staging destination already exists");
        else
            fprintf(stderr, "encode-draft: staging destination already exists\n");
        cJSON_Delete(draft);
        return 1;
    }
    if (reject_path_overlap(source_root, out_dir, "encode-draft")) {
        cJSON_Delete(draft);
        return 1;
    }

    memset(&m, 0, sizeof m);
    if (!draft_to_manifest(draft, &m)) {
        if (json)
            json_error_out("invalid_draft", "cannot interpret draft");
        else
            fprintf(stderr, "encode-draft: cannot interpret draft\n");
        cJSON_Delete(draft);
        return 1;
    }

    for (d = 0; d < m.disc_count; d++)
        total += m.discs[d].track_count;
    if (total == 0) {
        if (json)
            json_error_out("invalid_draft", "the draft has no tracks");
        else
            fprintf(stderr, "encode-draft: the draft has no tracks\n");
        musicpack_manifest_clear(&m);
        cJSON_Delete(draft);
        return 1;
    }

    ffmpeg_bin = resolve_bin(ffmpeg_arg, "ffmpeg", ffmpeg_buf, sizeof ffmpeg_buf);
    mpcenc_bin = resolve_bin(mpcenc_arg, "mpcenc", mpcenc_buf, sizeof mpcenc_buf);

    /* pre-flight: every source must be encodable (FLAC/WAV at a supported
       rate) and the toolchain must be available before any encoding starts */
    for (d = 0; d < m.disc_count && !bad; d++) {
        for (t = 0; t < m.discs[d].track_count && !bad; t++) {
            const musicpack_track *tr = &m.discs[d].tracks[t];
            const char *rel = tr->audio.path != 0 ? tr->audio.path : "";
            const char *ext = strrchr(rel, '.');
            mpc_stream_info si;
            long rate = 0;
            snprintf(srcpath, sizeof srcpath, "%s/%s", source_root, rel);
            if (ext == 0 || (strcmp(ext, ".flac") != 0 && strcmp(ext, ".wav") != 0)) {
                char msg[512];
                snprintf(msg, sizeof msg,
                         "track %d on disc %d ('%s') cannot be encoded to Musepack: "
                         "only FLAC and WAV sources are supported",
                         tr->number, m.discs[d].disc, rel);
                emit_encode_error("UNSUPPORTED_SOURCE", msg, m.discs[d].disc, tr->number);
                bad = 1;
                break;
            }
            memset(&si, 0, sizeof si);
            probe_stream(srcpath, &si);
            rate = si.sample_rate;
            if (rate == 0 && strcmp(ext, ".wav") == 0)
                rate = wav_sample_rate(srcpath);
            if (rate > 0 && !encode_supported_rate(rate)) {
                char msg[512];
                snprintf(msg, sizeof msg,
                         "track %d on disc %d ('%s') — sample rate %ld Hz is not "
                         "supported by Musepack (supported: 32/37.8/44.1/48 kHz)",
                         tr->number, m.discs[d].disc, rel, rate);
                emit_encode_error("UNSUPPORTED_SAMPLE_RATE", msg, m.discs[d].disc,
                                  tr->number);
                bad = 1;
            }
        }
    }
    if (!bad && !bin_available(ffmpeg_bin)) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "ffmpeg is required to decode FLAC/WAV but was not found "
                 "('%s'); install ffmpeg or pass --ffmpeg", ffmpeg_bin);
        emit_encode_error("TOOL_MISSING", msg, 0, 0);
        bad = 1;
    }
    if (!bad && !bin_available(mpcenc_bin)) {
        char msg[512];
        snprintf(msg, sizeof msg,
                 "mpcenc is required to encode Musepack but was not found "
                 "('%s'); build it or pass --mpcenc", mpcenc_bin);
        emit_encode_error("TOOL_MISSING", msg, 0, 0);
        bad = 1;
    }
    if (bad) {
        musicpack_manifest_clear(&m);
        cJSON_Delete(draft);
        return 1;
    }

    /* the staging area mirrors the package layout; the GUI owns `out_dir` */
    snprintf(audio_dir, sizeof audio_dir, "%s/audio", out_dir);
    snprintf(art_dir, sizeof art_dir, "%s/artwork", out_dir);
    snprintf(bok_dir, sizeof bok_dir, "%s/booklet", out_dir);
    snprintf(lyr_dir, sizeof lyr_dir, "%s/lyrics", out_dir);
    snprintf(ex_dir, sizeof ex_dir, "%s/extras", out_dir);
    if (mkdir_p(audio_dir) != 0 || mkdir_p(art_dir) != 0 || mkdir_p(bok_dir) != 0 ||
        mkdir_p(lyr_dir) != 0 || mkdir_p(ex_dir) != 0) {
        if (json)
            json_error_out("encode_failed", "cannot create staging directory");
        else
            fprintf(stderr, "encode-draft: cannot create staging directory\n");
        musicpack_manifest_clear(&m);
        cJSON_Delete(draft);
        return 1;
    }

#if !defined(_WIN32)
    signal(SIGTERM, on_encode_sigterm);
# ifdef SIGINT
    signal(SIGINT, on_encode_sigterm);
# endif
#endif

    snprintf(qbuf, sizeof qbuf, "%s", quality != 0 && *quality != '\0' ? quality : "6.0");

    for (d = 0; d < m.disc_count && !bad && !g_encode_cancelled; d++) {
        for (t = 0; t < m.discs[d].track_count && !bad && !g_encode_cancelled; t++) {
            musicpack_track *tr = &m.discs[d].tracks[t];
            const char *rel = tr->audio.path != 0 ? tr->audio.path : "";
            const char *ext = strrchr(rel, '.');
            char fname[MUSICPACK_PATH_MAX + 2];
            char target[MUSICPACK_PATH_MAX + 2];
            char *wav = 0;
            char *argv_dec[16];
            char *argv_enc[16];
            int src_codec; /* 0 flac, 1 wav */
            mpc_stream_info si;
            musicpack_tag_set tags, ape;
            int rc;

            snprintf(fname, sizeof fname, "%s", tr->title != 0 ? tr->title : "");
            sanitize_component(fname);
            if (m.disc_count > 1)
                snprintf(target, sizeof target, "%s/%d-%02d - %s.mpc", audio_dir,
                         m.discs[d].disc, tr->number, fname);
            else
                snprintf(target, sizeof target, "%s/%02d - %s.mpc", audio_dir,
                         tr->number, fname);

            snprintf(srcpath, sizeof srcpath, "%s/%s", source_root, rel);
            src_codec = (ext != 0 && strcmp(ext, ".wav") == 0);
            snprintf(wavpath, sizeof wavpath, "%s/track-%zu.wav", out_dir, t + 1);
            wav = wavpath;

            emit_encode_stage("decoding", done_count + 1, (int) total,
                              m.discs[d].disc, tr->number, tr->title);
            argv_dec[0] = (char *) ffmpeg_bin;
            argv_dec[1] = "-v"; argv_dec[2] = "error";
            argv_dec[3] = "-y";
            argv_dec[4] = "-i"; argv_dec[5] = (char *) srcpath;
            argv_dec[6] = "-vn";
            argv_dec[7] = "-f"; argv_dec[8] = "wav";
            argv_dec[9] = wav;
            argv_dec[10] = 0;
            rc = run_command(argv_dec);
            if (rc != 0 || !is_regular_path(wav)) {
                char msg[640];
                if (rc == 127)
                    snprintf(msg, sizeof msg,
                             "track %d on disc %d — ffmpeg ('%s') could not be run",
                             tr->number, m.discs[d].disc, ffmpeg_bin);
                else
                    snprintf(msg, sizeof msg,
                             "track %d on disc %d — decoding '%s' failed (ffmpeg "
                             "exit %d)",
                             tr->number, m.discs[d].disc, rel, rc);
                emit_encode_error("DECODE_FAILED", msg, m.discs[d].disc, tr->number);
                bad = 1;
                break;
            }

            emit_encode_stage("encoding", done_count + 1, (int) total,
                              m.discs[d].disc, tr->number, tr->title);
            argv_enc[0] = (char *) mpcenc_bin;
            argv_enc[1] = "--quality"; argv_enc[2] = qbuf;
            argv_enc[3] = "--overwrite";
            argv_enc[4] = "--silent";
            argv_enc[5] = wav;
            argv_enc[6] = (char *) target;
            argv_enc[7] = 0;
            rc = run_command(argv_enc);
            remove(wav);
            if (rc != 0 || !is_regular_path(target)) {
                char msg[640];
                remove(target);
                if (rc == 127)
                    snprintf(msg, sizeof msg,
                             "track %d on disc %d — mpcenc ('%s') could not be run",
                             tr->number, m.discs[d].disc, mpcenc_bin);
                else
                    snprintf(msg, sizeof msg,
                             "track %d on disc %d — encoding '%s' failed (mpcenc "
                             "exit %d)",
                             tr->number, m.discs[d].disc, rel, rc);
                emit_encode_error("ENCODE_FAILED", msg, m.discs[d].disc, tr->number);
                bad = 1;
                break;
            }

            emit_encode_stage("tagging", done_count + 1, (int) total,
                              m.discs[d].disc, tr->number, tr->title);
            memset(&tags, 0, sizeof tags);
            if (src_codec == 0)
                musicpack_flac_read_metadata(srcpath, &tags, 0);
            memset(&ape, 0, sizeof ape);
            if (musicpack_manifest_to_ape_tags(&m, tr, m.discs[d].disc,
                                               (int) m.disc_count,
                                               (int) m.discs[d].track_count,
                                               &ape) == MUSICPACK_OK) {
                merge_passthrough_tags(&tags, &ape);
                if (musicpack_ape_write(target, &ape) != MUSICPACK_OK) {
                    char msg[640];
                    snprintf(msg, sizeof msg,
                             "track %d on disc %d — cannot write APEv2 tags on '%s'",
                             tr->number, m.discs[d].disc, tr->title);
                    emit_encode_error("TAG_WRITE_FAILED", msg, m.discs[d].disc,
                                      tr->number);
                    bad = 1;
                }
                musicpack_tag_set_free(&ape);
            }
            musicpack_tag_set_free(&tags);
            if (bad)
                break;

            if (musicpack_sha256_file(target, hex, sizeof hex) != MUSICPACK_OK) {
                char msg[512];
                snprintf(msg, sizeof msg,
                         "track %d on disc %d — cannot hash encoded file",
                         tr->number, m.discs[d].disc);
                emit_encode_error("HASH_FAILED", msg, m.discs[d].disc, tr->number);
                bad = 1;
                break;
            }

            /* display data for the transformed draft */
            memset(&si, 0, sizeof si);
            probe_stream(target, &si);

            /* rewrite the draft's track entry in place */
            {
                cJSON *media = cJSON_GetObjectItemCaseSensitive(draft, "media");
                cJSON *mi = cJSON_GetArrayItem(media, (int) d);
                cJSON *trk = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(mi, "tracks"), (int) t);
                cJSON *sa;
                char dur[64];
                const char *relpath = target + strlen(out_dir) + 1;
                cJSON_DeleteItemFromObject(trk, "audioPath");
                cJSON_AddStringToObject(trk, "audioPath", relpath);
                cJSON_DeleteItemFromObject(trk, "codec");
                cJSON_AddStringToObject(trk, "codec", "musepack-sv8");
                cJSON_DeleteItemFromObject(trk, "streamVersion");
                cJSON_AddNumberToObject(trk, "streamVersion", 8);
                if (si.sample_rate > 0) {
                    cJSON_DeleteItemFromObject(trk, "sampleRate");
                    cJSON_AddNumberToObject(trk, "sampleRate", (double) si.sample_rate);
                }
                if (si.channels > 0) {
                    cJSON_DeleteItemFromObject(trk, "channels");
                    cJSON_AddNumberToObject(trk, "channels", (double) si.channels);
                }
                if (si.duration > 0) {
                    snprintf(dur, sizeof dur, "%.3f", si.duration);
                    cJSON_DeleteItemFromObject(trk, "duration");
                    cJSON_AddRawToObject(trk, "duration", dur);
                }
                sa = cJSON_GetObjectItemCaseSensitive(trk, "sourceAudio");
                if (!cJSON_IsObject(sa))
                    sa = cJSON_AddObjectToObject(trk, "sourceAudio");
                cJSON_DeleteItemFromObject(sa, "codec");
                cJSON_AddStringToObject(sa, "codec", src_codec ? "wav" : "flac");
            }

            done_count++;
            emit_encode_track(done_count, (int) total, m.discs[d].disc, tr->number,
                              tr->title, "ok", hex, si.duration);
        }
    }

    if (!bad && !g_encode_cancelled) {
        if (!stage_artwork(draft, source_root, art_dir) ||
            !stage_asset_dir(draft, "booklet", "booklet", source_root, bok_dir) ||
            !stage_asset_dir(draft, "lyrics", "lyrics", source_root, lyr_dir) ||
            !stage_asset_dir(draft, "extras", "extras", source_root, ex_dir)) {
            emit_encode_error("STAGING_FAILED", "cannot stage artwork or assets",
                              0, 0);
            bad = 1;
        }
    }

    if (!bad && !g_encode_cancelled) {
        /* the transformed draft now points at the staging area: the GUI can
           build the package from it directly */
        cJSON_DeleteItemFromObject(draft, "sourceRoot");
        cJSON_AddStringToObject(draft, "sourceRoot", out_dir);
        if (json) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "event", "done");
            cJSON_AddBoolToObject(o, "ok", 1);
            cJSON_AddStringToObject(o, "outputDir", out_dir);
            cJSON_AddNumberToObject(o, "tracks", (double) total);
            cJSON_AddItemToObject(o, "draft", draft);
            draft = 0; /* ownership moved */
            emit_encode_json(o);
            cJSON_Delete(o);
        } else {
            printf("encoded %d track(s) into '%s'\n", done_count, out_dir);
        }
    } else if (g_encode_cancelled) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "event", "cancelled");
        emit_encode_json(o);
        cJSON_Delete(o);
        bad = 0; /* cancellation is a distinct, non-failing outcome */
        /* deterministic cleanup: a cancelled run never leaves a partial
           staging area behind */
        rm_rf(out_dir);
    } else {
        /* failure: remove the partial staging area */
        rm_rf(out_dir);
    }

    musicpack_manifest_clear(&m);
    cJSON_Delete(draft);
    return g_encode_cancelled ? 130 : bad ? 1 : 0;
}

/* main                                                                */
/* ------------------------------------------------------------------ */

int
main(int argc, char **argv)
{
    const char *cmd;

    fprintf(stderr, "%s", ABOUT);
    if (argc < 2) {
        fprintf(stderr, "usage: musicpack <info|verify|identify|create|import|update-metadata|inspect|validate-draft|build-draft|identify-draft|encode-draft|author-api-version> ...\n");
        return 2;
    }
    cmd = argv[1];
    if (strcmp(cmd, "info") == 0) {
        const char *dir = 0;
        int json = 0, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0)
                json = 1;
            else if (dir == 0)
                dir = argv[i];
            else
                return usage_error("too many arguments");
        }
        if (dir == 0)
            return usage_error("info requires a package");
        return cmd_info(dir, json);
    }
    if (strcmp(cmd, "verify") == 0) {
        int quiet = 0, json = 0, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "-q") == 0)
                quiet = 1;
            else if (strcmp(argv[i], "--json") == 0)
                json = 1;
        }
        return argc >= 3 ? cmd_verify(argv[2], quiet, json)
                         : usage_error("verify requires a package");
    }
    if (strcmp(cmd, "identify") == 0) {
        const char *dir = 0, *mbjson = 0;
        int i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--mb-json") == 0 && i + 1 < argc) {
                mbjson = argv[i + 1];
                i++;
            } else if (dir == 0) {
                dir = argv[i];
            } else {
                return usage_error("too many arguments");
            }
        }
        if (dir == 0) {
            usage_identify();
            return 2;
        }
        return cmd_identify(dir, mbjson);
    }
    if (strcmp(cmd, "create") == 0)
        return cmd_create(argc - 1, argv + 1);
    if (strcmp(cmd, "import") == 0)
        return cmd_import(argc - 1, argv + 1);
    if (strcmp(cmd, "update-metadata") == 0) {
        const char *dir = 0;
        int sync = 0, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--sync-tags") == 0)
                sync = 1;
            else if (dir == 0)
                dir = argv[i];
            else
                return usage_error("too many arguments");
        }
        if (dir == 0) {
            usage_update_metadata();
            return 2;
        }
        return cmd_update_metadata(dir, sync);
    }
    if (strcmp(cmd, "inspect") == 0) {
        const char *dir = 0;
        int json = 0, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--json") == 0)
                json = 1;
            else if (dir == 0)
                dir = argv[i];
            else
                return usage_error("too many arguments");
        }
        if (dir == 0) {
            usage_inspect();
            return 2;
        }
        return cmd_inspect(dir, json);
    }
    if (strcmp(cmd, "validate-draft") == 0) {
        const char *draft_path = 0;
        int json = 0, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--draft") == 0 && i + 1 < argc) {
                draft_path = argv[++i];
            } else if (strcmp(argv[i], "--json") == 0) {
                json = 1;
            } else {
                return usage_error("too many arguments");
            }
        }
        if (draft_path == 0) {
            usage_validate_draft();
            return 2;
        }
        return cmd_validate_draft(draft_path, json);
    }
    if (strcmp(cmd, "build-draft") == 0) {
        const char *draft_path = 0, *out_dir = 0;
        int no_loudness = 0, json = 0, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--draft") == 0 && i + 1 < argc) {
                draft_path = argv[++i];
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out_dir = argv[++i];
            } else if (strcmp(argv[i], "--no-loudness") == 0) {
                no_loudness = 1;
            } else if (strcmp(argv[i], "--json") == 0) {
                json = 1;
            } else {
                return usage_error("too many arguments");
            }
        }
        if (draft_path == 0 || out_dir == 0) {
            usage_build_draft();
            return 2;
        }
        return cmd_build_draft(draft_path, out_dir, no_loudness, json);
    }
    if (strcmp(cmd, "identify-draft") == 0) {
        const char *draft_path = 0, *mbid = 0, *barcode = 0, *mbjson = 0;
        int json = 0, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--draft") == 0 && i + 1 < argc) {
                draft_path = argv[++i];
            } else if (strcmp(argv[i], "--mbid") == 0 && i + 1 < argc) {
                mbid = argv[++i];
            } else if (strcmp(argv[i], "--barcode") == 0 && i + 1 < argc) {
                barcode = argv[++i];
            } else if (strcmp(argv[i], "--mb-json") == 0 && i + 1 < argc) {
                mbjson = argv[++i];
            } else if (strcmp(argv[i], "--json") == 0) {
                json = 1;
            } else {
                return usage_error("too many arguments");
            }
        }
        if (draft_path == 0 || (mbid == 0 && barcode == 0 && mbjson == 0)) {
            usage_identify_draft();
            return 2;
        }
        return cmd_identify_draft(draft_path, mbid, barcode, mbjson, json);
    }
    if (strcmp(cmd, "encode-draft") == 0) {
        const char *draft_path = 0, *out_dir = 0, *quality = 0;
        const char *ffmpeg_bin = 0, *mpcenc_bin = 0;
        int json = 0, i;
        for (i = 2; i < argc; i++) {
            if (strcmp(argv[i], "--draft") == 0 && i + 1 < argc) {
                draft_path = argv[++i];
            } else if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
                out_dir = argv[++i];
            } else if (strcmp(argv[i], "--quality") == 0 && i + 1 < argc) {
                quality = argv[++i];
            } else if (strcmp(argv[i], "--ffmpeg") == 0 && i + 1 < argc) {
                ffmpeg_bin = argv[++i];
            } else if (strcmp(argv[i], "--mpcenc") == 0 && i + 1 < argc) {
                mpcenc_bin = argv[++i];
            } else if (strcmp(argv[i], "--json") == 0) {
                json = 1;
            } else {
                return usage_error("too many arguments");
            }
        }
        if (draft_path == 0 || out_dir == 0) {
            usage_encode_draft();
            return 2;
        }
        return cmd_encode_draft(draft_path, out_dir, quality, ffmpeg_bin,
                                mpcenc_bin, json);
    }
    if (strcmp(cmd, "author-api-version") == 0) {
        /* Machine-readable capability handshake for MusicPack Author. */
        cJSON *root = cJSON_CreateObject();
        char *out = 0;
        if (root == 0)
            return 1;
        cJSON_AddStringToObject(root, "musicpackVersion", MUSICPACK_VERSION);
        cJSON_AddNumberToObject(root, "authorApi", MUSICPACK_AUTHOR_API);
        out = cJSON_PrintUnformatted(root);
        if (out == 0) { cJSON_Delete(root); return 1; }
        puts(out);
        free(out);
        cJSON_Delete(root);
        return 0;
    }
    return usage_error("unknown command");
}
