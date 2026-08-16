/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
# include <direct.h>
# include <windows.h>
# define mkdir_one(path) _mkdir(path)
# define path_separator ";"
#else
# include <sys/stat.h>
# include <unistd.h>
# define mkdir_one(path) mkdir(path, 0755)
# define path_separator ":"
#endif

#include "tool_path.h"

static int failures;

#define CHECK(condition, message)                                             \
    do {                                                                      \
        if (!(condition)) {                                                   \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, message); \
            failures++;                                                       \
        }                                                                     \
    } while (0)

static int
make_temp_dir(char *path, size_t cap)
{
#ifdef _WIN32
    const char *base = getenv("TEMP");
    if (base == 0)
        base = ".";
    if (snprintf(path, cap, "%s\\musicpack_tool_path_%lu", base,
                 (unsigned long) GetCurrentProcessId()) >= (int) cap)
        return 0;
    return mkdir_one(path) == 0 || errno == EEXIST;
#else
    if (snprintf(path, cap, "/tmp/musicpack_tool_path_XXXXXX") >= (int) cap)
        return 0;
    return mkdtemp(path) != 0;
#endif
}

int
main(void)
{
    char root[512], dir[640], executable[768], search[1024];
#ifdef _WIN32
    char batch[768], command[768];
#endif
    char *found;
    FILE *f;

    if (!make_temp_dir(root, sizeof root)) {
        fprintf(stderr, "cannot create tool-path test directory\n");
        return 1;
    }
    snprintf(dir, sizeof dir, "%s/tool path's", root);
    CHECK(mkdir_one(dir) == 0, "create PATH entry with spaces");
#ifdef _WIN32
    snprintf(executable, sizeof executable, "%s/probe.XYZ", dir);
    snprintf(batch, sizeof batch, "%s/probe.BAT", dir);
    snprintf(command, sizeof command, "%s/probe.CMD", dir);
#else
    snprintf(executable, sizeof executable, "%s/probe", dir);
#endif
    f = fopen(executable, "wb");
    CHECK(f != 0, "create executable candidate");
    if (f != 0) {
        fputs("test", f);
        fclose(f);
    }
#ifdef _WIN32
    f = fopen(batch, "wb");
    CHECK(f != 0, "create BAT candidate");
    if (f != 0) {
        fputs("@exit /b 0\r\n", f);
        fclose(f);
    }
    f = fopen(command, "wb");
    CHECK(f != 0, "create CMD candidate");
    if (f != 0) {
        fputs("@exit /b 0\r\n", f);
        fclose(f);
    }
#endif
#ifndef _WIN32
    CHECK(chmod(executable, 0755) == 0, "mark executable candidate executable");
#endif
    snprintf(search, sizeof search, "%s%s%s", "/definitely/missing",
             path_separator, dir);
    found = musicpack_find_executable_in("probe", search,
#ifdef _WIN32
                                         ".BAT;.CMD;.XYZ"
#else
                                         0
#endif
    );
    CHECK(found != 0, "bare executable found on platform PATH");
    CHECK(found != 0 && strcmp(found, executable) == 0,
          "PATH/PATHEXT resolves the expected executable");
    free(found);

#ifdef _WIN32
    found = musicpack_find_executable_in(batch, 0, ".BAT;.CMD");
    CHECK(found == 0, "explicit BAT path rejected for direct spawning");
    free(found);
    found = musicpack_find_executable_in(command, 0, ".BAT;.CMD");
    CHECK(found == 0, "explicit CMD path rejected for direct spawning");
    free(found);
    found = musicpack_find_executable_in("probe", search, ".BAT;.CMD");
    CHECK(found == 0, "PATHEXT with only shell scripts is rejected");
    free(found);
#endif

    found = musicpack_find_executable_in(executable, 0,
#ifdef _WIN32
                                         ".XYZ"
#else
                                         0
#endif
    );
    CHECK(found != 0 && strcmp(found, executable) == 0,
          "explicit executable path resolves without a shell");
    free(found);

    remove(executable);
#ifdef _WIN32
    remove(batch);
    remove(command);
    _rmdir(dir);
    _rmdir(root);
#else
    rmdir(dir);
    rmdir(root);
#endif
    if (failures == 0) {
        printf("all musicpack tool-path tests passed\n");
        return 0;
    }
    return 1;
}
