/*
 * Copyright (c) 2026, The MusicPack Development Team
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include <musepack/musepack.h>
#include <musicpack/musicpack.h>

int
main(void)
{
    musicpack_status status;
    musicpack_meter *meter = musicpack_meter_new(2, 44100, &status);
    const char *version = musepack_version();

    musicpack_meter_free(meter);
    return meter == NULL || status != MUSICPACK_OK || version == NULL;
}
