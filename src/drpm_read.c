/*
    Authors:
        Pavel Tobias <ptobias@redhat.com>
        Matej Chalk <mchalk@redhat.com>

    Copyright (C) 2014 Red Hat

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Lesser General Public License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "drpm.h"
#include "drpm_private.h"

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <rpm/rpmlib.h>

#define MAGIC_DLT(x) (((x) >> 8) == 0x444C54)

int read_be32(int filedesc, uint32_t *buffer_ret)
{
    char buffer[4];

    switch (read(filedesc, buffer, 4)) {
    case 4:
        break;
    case -1:
        return DRPM_ERR_IO;
    default:
        return DRPM_ERR_FORMAT;
    }

    *buffer_ret = parse_be32(buffer);

    return DRPM_ERR_OK;
}

int readdelta_rest(int filedesc, struct drpm *delta)
{
    struct compstrm *stream;
    uint32_t src_nevr_len;
    uint32_t sequence_len;
    char *sequence = NULL;
    char md5[MD5_BYTES];
    int error = DRPM_ERR_OK;

    if ((error = compstrm_init(&stream, filedesc, &delta->comp)) != DRPM_ERR_OK)
        return error;

    if ((error = compstrm_read_be32(stream, &delta->version)) != DRPM_ERR_OK)
        goto cleanup;

    if (!MAGIC_DLT(delta->version)) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    delta->version = delta->version % 256 - '0';

    if (delta->version < 3 && delta->type == DRPM_TYPE_RPMONLY) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((error = compstrm_read_be32(stream, &src_nevr_len)) != DRPM_ERR_OK)
        goto cleanup;

    if ((delta->src_nevr = malloc(src_nevr_len + 1)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = compstrm_read(stream, src_nevr_len, delta->src_nevr)) != DRPM_ERR_OK)
        goto cleanup;

    delta->src_nevr[src_nevr_len] = '\0';

    if ((error = compstrm_read_be32(stream, &sequence_len)) != DRPM_ERR_OK)
        goto cleanup;

    if (sequence_len < MD5_BYTES) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((sequence = malloc(sequence_len)) == NULL ||
        (delta->sequence = malloc(sequence_len * 2 + 1)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = compstrm_read(stream, sequence_len, sequence)) != DRPM_ERR_OK)
        goto cleanup;

    dump_hex(delta->sequence, sequence, sequence_len);

    if ((error = compstrm_read(stream, MD5_BYTES, md5)) != DRPM_ERR_OK)
        goto cleanup;

    dump_hex(delta->tgt_md5, md5, MD5_BYTES);

    if (delta->version < 2) {
        delta->tgt_size = 0;
        goto cleanup;
    }

    error = compstrm_read_be32(stream, &delta->tgt_size);

cleanup:

    if (error == DRPM_ERR_OK)
        error = compstrm_destroy(&stream);
    else
        compstrm_destroy(&stream);

    free(sequence);

    return error;
}

int readdelta_rpmonly(int filedesc, struct drpm *delta)
{
    uint32_t version;
    uint32_t tgt_nevr_len;
    uint32_t add_data_size;
    int error;
    ssize_t bytes_read;

    if (read_be32(filedesc, &version) != DRPM_ERR_OK ||
        !MAGIC_DLT(version))
        return DRPM_ERR_FORMAT;

    version = (version & 0x000000FF) - '0';

    if (version < 1 || version > 3 ||
        read_be32(filedesc, &tgt_nevr_len) != DRPM_ERR_OK)
        return DRPM_ERR_FORMAT;

    if ((delta->tgt_nevr = malloc(tgt_nevr_len + 1)) == NULL)
        return DRPM_ERR_MEMORY;

    if ((bytes_read = read(filedesc, delta->tgt_nevr, tgt_nevr_len)) == -1)
        return DRPM_ERR_IO;

    if ((uint32_t) bytes_read != tgt_nevr_len)
        return DRPM_ERR_FORMAT;

    delta->tgt_nevr[tgt_nevr_len] = '\0';

    if ((error = read_be32(filedesc, &add_data_size)) != DRPM_ERR_OK)
        return error;

    lseek(filedesc, add_data_size, SEEK_CUR);

    return DRPM_ERR_OK;
}

int readdelta_standard(int filedesc, struct drpm *delta)
{
    FD_t file;
    Header header = NULL;
    Header signature = NULL;
    off_t remainder;
    int error = DRPM_ERR_OK;

    if ((file = Fopen(delta->filename, "rb")) == NULL)
        return DRPM_ERR_IO;

    if (Fseek(file, 96, SEEK_SET) == -1) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((signature = headerRead(file, HEADER_MAGIC_YES)) == NULL) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((remainder = Ftell(file) % 8) != 0) {
        if (Fseek(file, 8 - remainder, SEEK_CUR) == -1) {
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }
    }

    if ((header = headerRead(file, HEADER_MAGIC_YES)) == NULL) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((delta->tgt_nevr = headerGetAsString(header, RPMTAG_NEVR)) == NULL)
        error = DRPM_ERR_MEMORY;

    lseek(filedesc, Ftell(file), SEEK_SET);

cleanup:
    headerFree(header);
    headerFree(signature);
    Fclose(file);

    return error;
}
