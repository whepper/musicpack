/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/

#include <string.h>

#include "model_output.h"

int
sonic_model_read_output(sonic_model_output_get_fn get_output,
                        sonic_model_status_release_fn release_status,
                        void *context, float *out, size_t count)
{
    float *data = 0;
    void *status;

    if (get_output == 0 || out == 0)
        return 0;
    status = get_output(context, &data);
    if (status != 0) {
        if (release_status != 0)
            release_status(context, status);
        return 0;
    }
    if (data == 0)
        return 0;
    memcpy(out, data, count * sizeof(float));
    return 1;
}
