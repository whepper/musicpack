/*
  Copyright (c) 2005-2009, The Musepack Development Team
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

  * Neither the name of the The Musepack Development Team nor the
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
/// \file mpc_reader.c
/// Contains implementations for simple file-based mpc_reader
#include <mpc/reader.h>
#include <musepack/reader.h>
#include "internal.h"
#include <errno.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif
#include "../common/fileio.h"

#define STDIO_MAGIC ((mpc_int32_t)0xF34B963C) ///< Just a random safe-check value...
typedef struct mpc_reader_stdio_t {
    FILE       *p_file;
    mpc_seek_t  file_size;
    mpc_seek_t  position;
    mpc_bool_t  is_seekable;
    mpc_int32_t magic;
} mpc_reader_stdio;

/// mpc_reader callback implementations
static mpc_int32_t
read_stdio(mpc_reader *p_reader, void *ptr, mpc_int32_t size)
{
    mpc_reader_stdio *p_stdio = (mpc_reader_stdio*) p_reader->data;
    size_t n;
    if(p_stdio->magic != STDIO_MAGIC || size < 0 || (ptr == NULL && size != 0))
        return MPC_STATUS_FAIL;
    if (!p_stdio->is_seekable) {
        /* fread may wait to fill the complete request on a pipe. One raw read
           returns the bytes currently available so decoding can progress. */
        int result;
        do {
#ifdef _WIN32
            result = _read(_fileno(p_stdio->p_file), ptr, (unsigned int) size);
#else
            result = (int) read(fileno(p_stdio->p_file), ptr, (size_t) size);
#endif
        } while (result < 0 && errno == EINTR);
        if (result < 0)
            return MPC_STATUS_FAIL;
        p_stdio->position += (mpc_seek_t) result;
        return (mpc_int32_t) result;
    }
    n = fread(ptr, 1, (size_t) size, p_stdio->p_file);
    p_stdio->position += (mpc_seek_t) n;
    if (n == 0 && ferror(p_stdio->p_file))
        return MPC_STATUS_FAIL;
    return (mpc_int32_t) n;
}

static mpc_bool_t
seek_stdio(mpc_reader *p_reader, mpc_seek_t offset)
{
    mpc_reader_stdio *p_stdio = (mpc_reader_stdio*) p_reader->data;
    if(p_stdio->magic != STDIO_MAGIC) return MPC_FALSE;
    if (!p_stdio->is_seekable || mpc_file_seek(p_stdio->p_file, offset, SEEK_SET) != 0)
        return MPC_FALSE;
    p_stdio->position = offset;
    return MPC_TRUE;
}

static mpc_seek_t
tell_stdio(mpc_reader *p_reader)
{
    mpc_reader_stdio *p_stdio = (mpc_reader_stdio*) p_reader->data;
    if(p_stdio->magic != STDIO_MAGIC) return (mpc_seek_t) MPC_STATUS_FAIL;
    return p_stdio->is_seekable ? mpc_file_tell(p_stdio->p_file) : p_stdio->position;
}

static mpc_seek_t
get_size_stdio(mpc_reader *p_reader)
{
    mpc_reader_stdio *p_stdio = (mpc_reader_stdio*) p_reader->data;
    if(p_stdio->magic != STDIO_MAGIC) return (mpc_seek_t) MPC_STATUS_FAIL;
    return p_stdio->file_size;
}

static mpc_bool_t
canseek_stdio(mpc_reader *p_reader)
{
    mpc_reader_stdio *p_stdio = (mpc_reader_stdio*) p_reader->data;
    if(p_stdio->magic != STDIO_MAGIC) return MPC_FALSE;
    return p_stdio->is_seekable;
}

mpc_status
mpc_reader_init_stdio_stream(mpc_reader * p_reader, FILE * p_file)
{
    mpc_reader tmp_reader; mpc_reader_stdio *p_stdio; int err;

    if (p_reader == NULL || p_file == NULL)
        return MPC_STATUS_FAIL;

    p_stdio = NULL;
    memset(&tmp_reader, 0, sizeof tmp_reader);
    p_stdio = malloc(sizeof *p_stdio);
    if(!p_stdio) return MPC_STATUS_FAIL;
    memset(p_stdio, 0, sizeof *p_stdio);

    p_stdio->magic  = STDIO_MAGIC;
    p_stdio->p_file = p_file;
    p_stdio->is_seekable = MPC_TRUE;
    err = mpc_file_seek(p_stdio->p_file, 0, SEEK_END);
    if(err < 0) {
        clearerr(p_stdio->p_file);
        p_stdio->is_seekable = MPC_FALSE;
        p_stdio->file_size = 0;
    } else {
        p_stdio->file_size = mpc_file_tell(p_stdio->p_file);
        if(p_stdio->file_size == (mpc_seek_t) -1 ||
           mpc_file_seek(p_stdio->p_file, 0, SEEK_SET) != 0)
            goto clean;
    }

    tmp_reader.data     = p_stdio;
    tmp_reader.canseek  = canseek_stdio;
    tmp_reader.get_size = get_size_stdio;
    tmp_reader.read     = read_stdio;
    tmp_reader.seek     = seek_stdio;
    tmp_reader.tell     = tell_stdio;

    *p_reader = tmp_reader;
    return MPC_STATUS_OK;
clean:
    if(p_stdio && p_stdio->p_file)
        fclose(p_stdio->p_file);
    free(p_stdio);
    return MPC_STATUS_FAIL;
}

mpc_status
mpc_reader_init_stdio(mpc_reader *p_reader, const char *filename)
{
	FILE * stream;
	if (p_reader == NULL || filename == NULL)
		return MPC_STATUS_FAIL;
	stream = fopen(filename, "rb");
	if (stream == NULL) return MPC_STATUS_FAIL;
	return mpc_reader_init_stdio_stream(p_reader,stream);
}

void
mpc_reader_exit_stdio(mpc_reader *p_reader)
{
    mpc_reader_stdio *p_stdio;
    if (p_reader == NULL || p_reader->data == NULL) return;
    p_stdio = (mpc_reader_stdio*) p_reader->data;
    if(p_stdio->magic != STDIO_MAGIC) return;
    fclose(p_stdio->p_file);
    free(p_stdio);
    p_reader->data = NULL;
}

#define MEMORY_MAGIC ((mpc_int32_t)0x51A2C9D7) ///< Just a random safe-check value...
typedef struct mpc_reader_memory_t {
    const mpc_uint8_t *data;
    mpc_seek_t size;
    mpc_seek_t pos;
    mpc_int32_t magic;
} mpc_reader_memory;

static mpc_int32_t
read_memory(mpc_reader *p_reader, void *ptr, mpc_int32_t size)
{
    mpc_reader_memory *p_mem = (mpc_reader_memory*) p_reader->data;
    mpc_seek_t avail, n;

    if (p_mem->magic != MEMORY_MAGIC || size < 0 || (ptr == NULL && size != 0))
        return MPC_STATUS_FAIL;
    if (p_mem->pos >= p_mem->size)
        return 0;
    avail = p_mem->size - p_mem->pos;
    n = (mpc_seek_t) size < avail ? (mpc_seek_t) size : avail;
    memcpy(ptr, p_mem->data + p_mem->pos, (size_t) n);
    p_mem->pos += n;
    return (mpc_int32_t) n;
}

static mpc_bool_t
seek_memory(mpc_reader *p_reader, mpc_seek_t offset)
{
    mpc_reader_memory *p_mem = (mpc_reader_memory*) p_reader->data;
    if (p_mem->magic != MEMORY_MAGIC)
        return MPC_FALSE;
    if (offset > p_mem->size)
        return MPC_FALSE;
    p_mem->pos = offset;
    return MPC_TRUE;
}

static mpc_seek_t
tell_memory(mpc_reader *p_reader)
{
    mpc_reader_memory *p_mem = (mpc_reader_memory*) p_reader->data;
    if (p_mem->magic != MEMORY_MAGIC)
        return (mpc_seek_t) MPC_STATUS_FAIL;
    return p_mem->pos;
}

static mpc_seek_t
get_size_memory(mpc_reader *p_reader)
{
    mpc_reader_memory *p_mem = (mpc_reader_memory*) p_reader->data;
    if (p_mem->magic != MEMORY_MAGIC)
        return (mpc_seek_t) MPC_STATUS_FAIL;
    return p_mem->size;
}

static mpc_bool_t
canseek_memory(mpc_reader *p_reader)
{
    mpc_reader_memory *p_mem = (mpc_reader_memory*) p_reader->data;
    return p_mem->magic == MEMORY_MAGIC;
}

mpc_status
mpc_reader_init_memory(mpc_reader *reader, const void *data, mpc_seek_t size)
{
    mpc_reader tmp_reader;
    mpc_reader_memory *p_mem;

    if (reader == NULL || (data == NULL && size != 0) || size > SIZE_MAX)
        return MPC_STATUS_FAIL;
    memset(&tmp_reader, 0, sizeof tmp_reader);
    p_mem = malloc(sizeof *p_mem);
    if (p_mem == 0)
        return MPC_STATUS_FAIL;

    p_mem->magic = MEMORY_MAGIC;
    p_mem->data  = (const mpc_uint8_t*) data;
    p_mem->size  = size;
    p_mem->pos   = 0;

    tmp_reader.data     = p_mem;
    tmp_reader.canseek  = canseek_memory;
    tmp_reader.get_size = get_size_memory;
    tmp_reader.read     = read_memory;
    tmp_reader.seek     = seek_memory;
    tmp_reader.tell     = tell_memory;

    *reader = tmp_reader;
    return MPC_STATUS_OK;
}

void
mpc_reader_exit_memory(mpc_reader *reader)
{
    mpc_reader_memory *p_mem;
    if (reader == NULL || reader->data == NULL)
        return;
    p_mem = (mpc_reader_memory*) reader->data;
    if (p_mem->magic != MEMORY_MAGIC)
        return;
    free(p_mem);
    reader->data = NULL;
}
