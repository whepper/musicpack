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

/* Native APEv2 tag dumper for the encode integration test: reads the tags
   written on an encoded .mpc through libmusicpack and prints `KEY=value`
   lines, replacing the old ffprobe-based check. */

#include <stdio.h>
#include <stdlib.h>

#include <musicpack/musicpack.h>

int
main(int argc, char **argv)
{
    musicpack_tag_set tags;
    musicpack_status st;
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <file.mpc>\n", argv[0]);
        return 2;
    }
    st = musicpack_ape_read(argv[1], &tags);
    if (st != MUSICPACK_OK)
        return 1;
    for (i = 0; i < tags.count; i++) {
        const musicpack_tag *t = &tags.items[i];
        if (t->is_binary)
            continue;
        printf("%s=%s\n", t->key, t->value != 0 ? t->value : "");
    }
    musicpack_tag_set_free(&tags);
    return 0;
}
