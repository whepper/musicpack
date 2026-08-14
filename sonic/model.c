/*
  Copyright (c) 2026, The MusicPack Development Team
  All rights reserved. (BSD-3-Clause; see LICENSES/BSD-3-Clause.txt for the full text.)
  SPDX-License-Identifier: BSD-3-Clause
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <onnxruntime_c_api.h>

#include "frontend.h"
#include "model.h"
#include "sonic_profile.h"

struct sonic_model {
    const OrtApi *api;
    OrtEnv *env;
    OrtSessionOptions *opts;
    OrtSession *sess;
    char *input_name;
    char *output_name;
    int64_t input_dims[4];
    int64_t output_dims[2];
};

static char *
ort_string(const OrtApi *api, OrtAllocator *alloc, char *s)
{
    size_t len = strnlen(s, 256);
    char *copy = (char *) malloc(len + 1);
    if (copy == 0)
        return 0;
    memcpy(copy, s, len);
    copy[len] = '\0';
    {
        OrtStatus *st = api->AllocatorFree(alloc, s);
        if (st != 0)
            api->ReleaseStatus(st);
    }
    return copy;
}

sonic_model *
sonic_model_open(const char *onnx_path)
{
    sonic_model *m;
    const OrtApi *api;
    OrtAllocator *alloc = 0;
    char *in = 0, *out = 0;
    size_t nin, nout;
    OrtStatus *st = 0;
    int rc = 0;

    if (onnx_path == 0)
        return 0;
    m = (sonic_model *) calloc(1, sizeof *m);
    if (m == 0)
        return 0;
    api = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    m->api = api;

    if (api->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "musicpack-sonic", &m->env) != 0)
        goto fail;
    {
        OrtStatus *st2 = api->CreateSessionOptions(&m->opts);
        if (st2 != 0) {
            api->ReleaseStatus(st2);
            goto fail;
        }
    }
    {
        OrtStatus *st2 = api->SetIntraOpNumThreads(m->opts, 1); /* determinism */
        if (st2 != 0) {
            api->ReleaseStatus(st2);
            goto fail;
        }
    }
    st = api->CreateSession(m->env, onnx_path, m->opts, &m->sess);
    if (st != 0) {
        api->ReleaseStatus(st);
        goto fail;
    }
    if (api->GetAllocatorWithDefaultOptions(&alloc) != 0)
        goto fail;
    if (api->SessionGetInputCount(m->sess, &nin) != 0 || nin != 1)
        goto fail;
    if (api->SessionGetOutputCount(m->sess, &nout) != 0 || nout != 1)
        goto fail;
    st = api->SessionGetInputName(m->sess, 0, alloc, &in);
    if (st != 0 || in == 0) {
        if (st) api->ReleaseStatus(st);
        goto fail;
    }
    st = api->SessionGetOutputName(m->sess, 0, alloc, &out);
    if (st != 0 || out == 0) {
        if (st) api->ReleaseStatus(st);
        goto fail;
    }
    m->input_name = ort_string(api, alloc, in);
    m->output_name = ort_string(api, alloc, out);
    if (m->input_name == 0 || m->output_name == 0)
        goto fail;

    /* expected shapes: input (N, 256, 199, 1), output (N, 512) */
    {
        OrtTypeInfo *ti = 0;
        const OrtTensorTypeAndShapeInfo *info;
        int64_t dims[4];
        size_t ndim;
        st = api->SessionGetInputTypeInfo(m->sess, 0, &ti);
        if (st != 0) {
            api->ReleaseStatus(st);
            goto fail;
        }
        if (api->CastTypeInfoToTensorInfo(ti, &info) != 0 || info == 0 ||
            api->GetDimensionsCount(info, &ndim) != 0 || ndim != 4 ||
            api->GetDimensions(info, dims, 4) != 0) {
            api->ReleaseTypeInfo(ti);
            goto fail;
        }
        api->ReleaseTypeInfo(ti);
        m->input_dims[0] = -1;
        m->input_dims[1] = dims[1];
        m->input_dims[2] = dims[2];
        m->input_dims[3] = dims[3];
        if (m->input_dims[1] != 256 || m->input_dims[2] != SONIC_MEL_FRAMES ||
            m->input_dims[3] != 1) {
            goto fail;
        }
    }
    {
        OrtTypeInfo *ti = 0;
        const OrtTensorTypeAndShapeInfo *info;
        int64_t dims[2];
        size_t ndim;
        st = api->SessionGetOutputTypeInfo(m->sess, 0, &ti);
        if (st != 0) {
            api->ReleaseStatus(st);
            goto fail;
        }
        if (api->CastTypeInfoToTensorInfo(ti, &info) != 0 || info == 0 ||
            api->GetDimensionsCount(info, &ndim) != 0 || ndim != 2 ||
            api->GetDimensions(info, dims, 2) != 0) {
            api->ReleaseTypeInfo(ti);
            goto fail;
        }
        api->ReleaseTypeInfo(ti);
        m->output_dims[0] = -1;
        m->output_dims[1] = dims[1];
        if (m->output_dims[1] != SONIC_PROFILE_DIMENSIONS)
            goto fail;
    }
    return m;

fail:
    (void) rc;
    sonic_model_close(m);
    return 0;
}

void
sonic_model_close(sonic_model *m)
{
    if (m == 0)
        return;
    if (m->api != 0) {
        if (m->sess)
            m->api->ReleaseSession(m->sess);
        if (m->opts)
            m->api->ReleaseSessionOptions(m->opts);
        if (m->env)
            m->api->ReleaseEnv(m->env);
    }
    free(m->input_name);
    free(m->output_name);
    free(m);
}

int
sonic_model_run(const sonic_model *m, const float *mel, size_t n_windows,
                float *out, size_t out_cap)
{
    OrtValue *input = 0, *output = 0;
    OrtMemoryInfo *mem = 0;
    int64_t in_shape[4], out_shape[2];
    OrtStatus *st;
    float *out_data = 0;
    int rc = 0;

    if (m == 0 || mel == 0 || out == 0 || n_windows == 0 ||
        n_windows * SONIC_PROFILE_DIMENSIONS > out_cap)
        return 0;

    in_shape[0] = (int64_t) n_windows;
    in_shape[1] = m->input_dims[1];
    in_shape[2] = m->input_dims[2];
    in_shape[3] = m->input_dims[3];
    out_shape[0] = (int64_t) n_windows;
    out_shape[1] = m->output_dims[1];

    if (m->api->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem) != 0)
        return 0;
    st = m->api->CreateTensorWithDataAsOrtValue(
        mem, (void *) mel,
        n_windows * (size_t) (256 * SONIC_MEL_FRAMES) * sizeof(float),
        in_shape, 4, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &input);
    m->api->ReleaseMemoryInfo(mem);
    if (st != 0) {
        m->api->ReleaseStatus(st);
        return 0;
    }
    st = m->api->Run(m->sess, 0, (const char *const *) &m->input_name,
                     (const OrtValue *const *) &input, 1,
                     (const char *const *) &m->output_name, 1, &output);
    m->api->ReleaseValue(input);
    if (st != 0) {
        m->api->ReleaseStatus(st);
        return 0;
    }
    st = m->api->GetTensorMutableData(output, (void **) &out_data);
    if (st == 0 && out_data != 0)
        memcpy(out, out_data, n_windows * SONIC_PROFILE_DIMENSIONS * sizeof(float));
    else
        rc = 0;
    if (st != 0)
        m->api->ReleaseStatus(st);
    m->api->ReleaseValue(output);
    return rc == 0 ? 1 : 0;
}
