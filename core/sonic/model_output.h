/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/
#ifndef SONIC_MODEL_OUTPUT_H_
#define SONIC_MODEL_OUTPUT_H_

#include <stddef.h>

typedef void *(*sonic_model_output_get_fn)(void *context, float **data);
typedef void (*sonic_model_status_release_fn)(void *context, void *status);

int sonic_model_read_output(sonic_model_output_get_fn get_output,
                            sonic_model_status_release_fn release_status,
                            void *context, float *out, size_t count);

#endif
