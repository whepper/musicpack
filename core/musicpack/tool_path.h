/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/
#ifndef MUSICPACK_TOOL_PATH_H_
#define MUSICPACK_TOOL_PATH_H_

char *musicpack_find_executable(const char *name);
char *musicpack_find_executable_in(const char *name, const char *path,
                                   const char *pathext);

#endif
