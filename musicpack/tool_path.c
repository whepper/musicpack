/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/

#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <io.h>
# include <sys/stat.h>
# define PATH_LIST_SEPARATOR ';'
#else
# include <sys/stat.h>
# include <unistd.h>
# define PATH_LIST_SEPARATOR ':'
#endif

#include "tool_path.h"

static int
is_executable_file(const char *path)
{
#ifdef _WIN32
    struct _stat st;
    return _stat(path, &st) == 0 && (st.st_mode & _S_IFREG) != 0;
#else
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
#endif
}

static char *
join_candidate(const char *dir, size_t dir_len, const char *name,
               const char *extension, size_t extension_len)
{
    size_t name_len = strlen(name);
    int separator = dir_len > 0 && dir[dir_len - 1] != '/' && dir[dir_len - 1] != '\\';
    char *candidate = (char *) malloc(dir_len + (size_t) separator + name_len +
                                      extension_len + 1);
    size_t p = 0;
    if (candidate == 0)
        return 0;
    if (dir_len > 0) {
        memcpy(candidate, dir, dir_len);
        p = dir_len;
    }
    if (separator)
        candidate[p++] = '/';
    memcpy(candidate + p, name, name_len);
    p += name_len;
    if (extension_len > 0) {
        memcpy(candidate + p, extension, extension_len);
        p += extension_len;
    }
    candidate[p] = '\0';
    if (!is_executable_file(candidate)) {
        free(candidate);
        return 0;
    }
    return candidate;
}

#ifdef _WIN32
static int
name_has_extension(const char *name)
{
    const char *base = name, *p;
    for (p = name; *p != '\0'; p++)
        if (*p == '/' || *p == '\\')
            base = p + 1;
    return strchr(base, '.') != 0;
}

static int
extension_is_shell_only(const char *extension, size_t len)
{
    if (len > 0 && *extension == '.') {
        extension++;
        len--;
    }
    return len == 3 &&
           (extension[0] == 'b' || extension[0] == 'B' ||
            extension[0] == 'c' || extension[0] == 'C') &&
           ((extension[0] == 'b' || extension[0] == 'B')
                ? (extension[1] == 'a' || extension[1] == 'A') &&
                  (extension[2] == 't' || extension[2] == 'T')
                : (extension[1] == 'm' || extension[1] == 'M') &&
                  (extension[2] == 'd' || extension[2] == 'D'));
}

static int
name_has_shell_only_extension(const char *name)
{
    const char *base = name, *dot = 0, *p;
    for (p = name; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
            dot = 0;
        } else if (*p == '.') {
            dot = p;
        }
    }
    return dot != 0 && dot >= base && extension_is_shell_only(dot, strlen(dot));
}

static char *
try_windows_extensions(const char *dir, size_t dir_len, const char *name,
                       const char *pathext)
{
    const char *p;
    char *candidate;
    if (name_has_shell_only_extension(name))
        return 0;
    candidate = join_candidate(dir, dir_len, name, 0, 0);
    if (candidate != 0 || name_has_extension(name))
        return candidate;
    if (pathext == 0 || *pathext == '\0')
        pathext = ".COM;.EXE";
    p = pathext;
    while (*p != '\0') {
        const char *end = strchr(p, ';');
        size_t len = end != 0 ? (size_t) (end - p) : strlen(p);
        while (len > 0 && (*p == ' ' || *p == '\t')) {
            p++;
            len--;
        }
        while (len > 0 && (p[len - 1] == ' ' || p[len - 1] == '\t'))
            len--;
        if (len > 0 && !extension_is_shell_only(p, len)) {
            char dot = '.';
            if (*p == '.')
                candidate = join_candidate(dir, dir_len, name, p, len);
            else {
                char *extension = (char *) malloc(len + 1);
                if (extension == 0)
                    return 0;
                extension[0] = dot;
                memcpy(extension + 1, p, len);
                candidate = join_candidate(dir, dir_len, name, extension, len + 1);
                free(extension);
            }
            if (candidate != 0)
                return candidate;
        }
        if (end == 0)
            break;
        p = end + 1;
    }
    return 0;
}
#endif

char *
musicpack_find_executable_in(const char *name, const char *path,
                             const char *pathext)
{
    const char *p;
    if (name == 0 || *name == '\0')
        return 0;
    if (strchr(name, '/') != 0 || strchr(name, '\\') != 0) {
#ifdef _WIN32
        return try_windows_extensions(0, 0, name, pathext);
#else
        return join_candidate(0, 0, name, 0, 0);
#endif
    }
    if (path == 0)
        return 0;
    p = path;
    while (*p != '\0') {
        const char *end = strchr(p, PATH_LIST_SEPARATOR);
        const char *dir = p;
        size_t len = end != 0 ? (size_t) (end - p) : strlen(p);
        char *candidate;
        if (len >= 2 && dir[0] == '"' && dir[len - 1] == '"') {
            dir++;
            len -= 2;
        }
        if (len > 0) {
#ifdef _WIN32
            candidate = try_windows_extensions(dir, len, name, pathext);
#else
            (void) pathext;
            candidate = join_candidate(dir, len, name, 0, 0);
#endif
            if (candidate != 0)
                return candidate;
        }
        if (end == 0)
            break;
        p = end + 1;
    }
    return 0;
}

char *
musicpack_find_executable(const char *name)
{
    return musicpack_find_executable_in(name, getenv("PATH"),
#ifdef _WIN32
                                        getenv("PATHEXT")
#else
                                        0
#endif
    );
}
