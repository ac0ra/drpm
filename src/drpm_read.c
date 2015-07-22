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
#define MAGIC_DLT3(x) ((x) == 0x444C5433)

#define RPM_LEAD_SIG_MIN_LEN 112

#define DELTARPM_COMPALGO(comp) ((comp) % 256)

#define DELTARPM_COMP_UN 0
#define DELTARPM_COMP_GZ 1
#define DELTARPM_COMP_BZ_20 2
#define DELTARPM_COMP_GZ_RSYNC 3
#define DELTARPM_COMP_BZ_17 4
#define DELTARPM_COMP_LZMA 5
#define DELTARPM_COMP_XZ 6

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

int read_be64(int filedesc, uint64_t *buffer_ret)
{
    char buffer[8];

    switch (read(filedesc, buffer, 8)) {
    case 8:
        break;
    case -1:
        return DRPM_ERR_IO;
    default:
        return DRPM_ERR_FORMAT;
    }

    *buffer_ret = parse_be64(buffer);

    return DRPM_ERR_OK;
}

int readdelta_rest(int filedesc, struct drpm *delta)
{
    struct compstrm *stream;
    uint32_t src_nevr_len;
    uint32_t sequence_len;
    uint32_t tgt_comp;
    uint32_t comp_param_len;
    uint32_t offadjn;
    uint32_t leadlen;
    uint32_t inn;
    uint32_t outn;
    uint32_t ext_data_msb;
    uint32_t ext_data;
    uint32_t add_data_size;
    uint32_t int_data_msb;
    uint32_t int_data;
    char *sequence = NULL;
    char md5[MD5_BYTES];
    char *comp_param = NULL;
    char *lead = NULL;
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

    if (sequence_len < MD5_BYTES ||
        (sequence_len != MD5_BYTES && delta->type == DRPM_TYPE_RPMONLY)) {
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

    if (delta->version >= 2) {
        if ((error = compstrm_read_be32(stream, &delta->tgt_size)) != DRPM_ERR_OK ||
            (error = compstrm_read_be32(stream, &tgt_comp)) != DRPM_ERR_OK)
            goto cleanup;

        switch (DELTARPM_COMPALGO(tgt_comp)) {
        case DELTARPM_COMP_UN:
            delta->tgt_comp = DRPM_COMP_NONE;
            break;
        case DELTARPM_COMP_GZ:
        case DELTARPM_COMP_GZ_RSYNC:
            delta->tgt_comp = DRPM_COMP_GZIP;
            break;
        case DELTARPM_COMP_BZ_20:
        case DELTARPM_COMP_BZ_17:
            delta->tgt_comp = DRPM_COMP_BZIP2;
            break;
        case DELTARPM_COMP_LZMA:
            delta->tgt_comp = DRPM_COMP_LZMA;
            break;
        case DELTARPM_COMP_XZ:
            delta->tgt_comp = DRPM_COMP_XZ;
            break;
        default:
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }

        if ((error = compstrm_read_be32(stream, &comp_param_len)) != DRPM_ERR_OK)
            goto cleanup;

        if (comp_param_len > 0) {
            if ((comp_param = malloc(comp_param_len)) == NULL ||
                (delta->tgt_comp_param = malloc(comp_param_len * 2 + 1)) == NULL) {
                error = DRPM_ERR_MEMORY;
                goto cleanup;
            }

            if ((error = compstrm_read(stream, comp_param_len, comp_param)) != DRPM_ERR_OK)
                goto cleanup;

            dump_hex(delta->tgt_comp_param, comp_param, comp_param_len);
        }

        if (delta->version == 3) {
            if ((error = compstrm_read_be32(stream, &delta->tgt_header_len)) != DRPM_ERR_OK ||
                (error = compstrm_read_be32(stream, &offadjn)) != DRPM_ERR_OK)
                goto cleanup;

            delta->adj_elems_size = 2 * offadjn;

            if (delta->adj_elems_size > 0) {
                if ((delta->adj_elems = malloc(delta->adj_elems_size * 4)) == NULL) {
                    error = DRPM_ERR_MEMORY;
                    goto cleanup;
                }
                for (uint32_t i = 0; i < delta->adj_elems_size; i += 2)
                    if ((error = compstrm_read_be32(stream, delta->adj_elems + i)) != DRPM_ERR_OK)
                        goto cleanup;
                for (uint32_t j = 1; j < delta->adj_elems_size; j += 2)
                    if ((error = compstrm_read_be32(stream, delta->adj_elems + j)) != DRPM_ERR_OK)
                        goto cleanup;
            }
        }
    }

    if (delta->tgt_header_len == 0 && delta->type == DRPM_TYPE_RPMONLY) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((error = compstrm_read_be32(stream, &leadlen)) != DRPM_ERR_OK)
        goto cleanup;

    if (leadlen < RPM_LEAD_SIG_MIN_LEN) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((lead = malloc(leadlen)) == NULL ||
        (delta->tgt_lead = malloc(leadlen * 2 + 1)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = compstrm_read(stream, leadlen, lead)) != DRPM_ERR_OK)
        goto cleanup;

    dump_hex(delta->tgt_lead, lead, leadlen);

    if ((error = compstrm_read_be32(stream, &delta->payload_fmt_off)) != DRPM_ERR_OK ||
        (error = compstrm_read_be32(stream, &inn)) != DRPM_ERR_OK ||
        (error = compstrm_read_be32(stream, &outn)) != DRPM_ERR_OK)
        goto cleanup;

    delta->int_copies_size = 2 * inn;
    delta->ext_copies_size = 2 * outn;

    if (delta->int_copies_size > 0) {
        if ((delta->int_copies = malloc(delta->int_copies_size * 4)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        for (uint32_t i = 0; i < delta->int_copies_size; i += 2)
            if ((error = compstrm_read_be32(stream, delta->int_copies + i)) != DRPM_ERR_OK)
                goto cleanup;
        for (uint32_t j = 1; j < delta->int_copies_size; j += 2)
            if ((error = compstrm_read_be32(stream, delta->int_copies + j)) != DRPM_ERR_OK)
                goto cleanup;
    }

    if (delta->ext_copies_size > 0) {
        if ((delta->ext_copies = malloc(delta->ext_copies_size * 4)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        for (uint32_t i = 0; i < delta->ext_copies_size; i += 2)
            if ((error = compstrm_read_be32(stream, delta->ext_copies + i)) != DRPM_ERR_OK)
                goto cleanup;
        for (uint32_t j = 1; j < delta->ext_copies_size; j += 2)
            if ((error = compstrm_read_be32(stream, delta->ext_copies + j)) != DRPM_ERR_OK)
                goto cleanup;
    }

    if (delta->version == 3) {
        if ((error = compstrm_read_be32(stream, &ext_data_msb)) != DRPM_ERR_OK)
            goto cleanup;
        delta->ext_data_len = (uint64_t)ext_data_msb << 32;
    }

    if ((error = compstrm_read_be32(stream, &ext_data)) != DRPM_ERR_OK)
        goto cleanup;

    delta->ext_data_len += ext_data;

    if ((error = compstrm_read_be32(stream, &add_data_size)) != DRPM_ERR_OK)
        goto cleanup;

    if (add_data_size > 0) {
        if (delta->type == DRPM_TYPE_RPMONLY) {
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }
        if ((error = compstrm_skip(stream, add_data_size)) != DRPM_ERR_OK)
            goto cleanup;
    }

    if (delta->version == 3) {
        if ((error = compstrm_read_be32(stream, &int_data_msb)) != DRPM_ERR_OK)
            goto cleanup;
        delta->int_data_len = (uint64_t)int_data_msb << 32;
    }

    if ((error = compstrm_read_be32(stream, &int_data)) != DRPM_ERR_OK)
        goto cleanup;

    delta->int_data_len += int_data;

cleanup:

    if (error == DRPM_ERR_OK)
        error = compstrm_destroy(&stream);
    else
        compstrm_destroy(&stream);

    free(sequence);
    free(comp_param);
    free(lead);

    return error;
}

int readdelta_rpmonly(int filedesc, struct drpm *delta)
{
    uint32_t version;
    uint32_t tgt_nevr_len;
    uint32_t add_data_size;
    ssize_t bytes_read;
    int error;

    if ((error = read_be32(filedesc, &version)) != DRPM_ERR_OK)
        return error;

    if (!MAGIC_DLT3(version))
        return DRPM_ERR_FORMAT;

    if ((error = read_be32(filedesc, &tgt_nevr_len)) != DRPM_ERR_OK)
        return error;

    if ((delta->tgt_nevr = malloc(tgt_nevr_len + 1)) == NULL)
        return DRPM_ERR_MEMORY;

    if ((bytes_read = read(filedesc, delta->tgt_nevr, tgt_nevr_len)) < 0)
        return DRPM_ERR_IO;

    if ((uint32_t) bytes_read != tgt_nevr_len)
        return DRPM_ERR_FORMAT;

    delta->tgt_nevr[tgt_nevr_len] = '\0';

    if ((error = read_be32(filedesc, &add_data_size)) != DRPM_ERR_OK)
        return error;

    if (lseek(filedesc, add_data_size, SEEK_CUR) == (off_t)-1)
        return DRPM_ERR_IO;

    return DRPM_ERR_OK;
}

int readdelta_standard(int filedesc, struct drpm *delta)
{
    FD_t file;
    Header header = NULL;
    Header signature = NULL;
    off_t file_pos;
    const char *payload_comp;
    int error = DRPM_ERR_OK;

    if ((file = Fopen(delta->filename, "rb")) == NULL)
        return DRPM_ERR_IO;

    if (Fseek(file, 96, SEEK_SET) < 0 ||
        (signature = headerRead(file, HEADER_MAGIC_YES)) == NULL ||
        (file_pos = Ftell(file)) < 0 ||
        Fseek(file, (8 - (file_pos % 8)) % 8, SEEK_CUR) < 0 ||
        (header = headerRead(file, HEADER_MAGIC_YES)) == NULL ||
        (payload_comp = headerGetString(header, RPMTAG_PAYLOADCOMPRESSOR)) == NULL) {
        error = Ferror(file) ? DRPM_ERR_IO : DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if (strcmp(payload_comp, "gzip") == 0) {
        delta->tgt_comp = DRPM_COMP_GZIP;
    } else if (strcmp(payload_comp, "bzip2") == 0) {
        delta->tgt_comp = DRPM_COMP_BZIP2;
    } else if (strcmp(payload_comp, "lzip") == 0) {
        delta->tgt_comp = DRPM_COMP_LZIP;
    } else if (strcmp(payload_comp, "lzma") == 0) {
        delta->tgt_comp = DRPM_COMP_LZMA;
    } else if (strcmp(payload_comp, "xz") == 0) {
        delta->tgt_comp = DRPM_COMP_XZ;
    } else {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if (lseek(filedesc, Ftell(file), SEEK_SET) == (off_t)-1)
        error = DRPM_ERR_IO;

cleanup:
    headerFree(header);
    headerFree(signature);
    Fclose(file);

    return error;
}
