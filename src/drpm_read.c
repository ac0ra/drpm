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
#include <openssl/md5.h>

#define MAGIC_DLT(x) (((x) >> 8) == 0x444C54)
#define MAGIC_DLT3(x) ((x) == 0x444C5433)

#define RPM_LEADSIG_MIN_LEN 112 /* 96B rpmlead + 16B signature intro */

int read_be32(int filedesc, uint32_t *buffer_ret)
{
    unsigned char buffer[4];

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
    unsigned char buffer[8];

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
    struct decompstrm *stream;
    uint32_t src_nevr_len;
    uint32_t sequence_len;
    uint32_t deltarpm_comp;
    unsigned short tgt_comp;
    uint32_t comp_param_len;
    uint32_t offadjn;
    uint32_t leadlen;
    uint32_t inn;
    uint32_t outn;
    uint32_t ext_data_32;
    uint32_t add_data_size;
    uint32_t int_data_32;
    unsigned char *sequence = NULL;
    unsigned char md5[MD5_DIGEST_LENGTH];
    unsigned char *comp_param = NULL;
    unsigned char *lead = NULL;
    int error = DRPM_ERR_OK;

    if ((error = decompstrm_init(&stream, filedesc, &delta->comp, NULL)) != DRPM_ERR_OK)
        return error;

    if ((error = decompstrm_read_be32(stream, &delta->version)) != DRPM_ERR_OK)
        goto cleanup;

    if (!MAGIC_DLT(delta->version)) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    delta->version = delta->version % 256 - '0';

    printf("delta version: %u\n", delta->version);

    if (delta->version < 3 && delta->type == DRPM_TYPE_RPMONLY) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((error = decompstrm_read_be32(stream, &src_nevr_len)) != DRPM_ERR_OK)
        goto cleanup;

    if ((delta->src_nevr = malloc(src_nevr_len + 1)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = decompstrm_read(stream, src_nevr_len, delta->src_nevr)) != DRPM_ERR_OK)
        goto cleanup;

    delta->src_nevr[src_nevr_len] = '\0';

    printf("source NEVR: %s\n", delta->src_nevr);

    if ((error = decompstrm_read_be32(stream, &sequence_len)) != DRPM_ERR_OK)
        goto cleanup;

    if (sequence_len < MD5_DIGEST_LENGTH ||
        (sequence_len != MD5_DIGEST_LENGTH && delta->type == DRPM_TYPE_RPMONLY)) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((sequence = malloc(sequence_len)) == NULL ||
        (delta->sequence = malloc(sequence_len * 2 + 1)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = decompstrm_read(stream, sequence_len, sequence)) != DRPM_ERR_OK)
        goto cleanup;

    dump_hex(delta->sequence, sequence, sequence_len);

    printf("sequence: %s\n", delta->sequence);

    if ((error = decompstrm_read(stream, MD5_DIGEST_LENGTH, md5)) != DRPM_ERR_OK)
        goto cleanup;

    dump_hex(delta->tgt_md5, md5, MD5_DIGEST_LENGTH);

    printf("target MD5: %s\n", delta->tgt_md5);

    if (delta->version >= 2) {
        if ((error = decompstrm_read_be32(stream, &delta->tgt_size)) != DRPM_ERR_OK ||
            (error = decompstrm_read_be32(stream, &deltarpm_comp)) != DRPM_ERR_OK)
            goto cleanup;

        printf("target size: %u\n", delta->tgt_size);

        unsigned short level; //
        if (!deltarpm_decode_comp(deltarpm_comp, &tgt_comp, &level)) { // 3rd arg = NULL
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }

        delta->tgt_comp = tgt_comp;

        printf("target compression: %s (%u)\n", comp2str(tgt_comp), level);

        if ((error = decompstrm_read_be32(stream, &comp_param_len)) != DRPM_ERR_OK)
            goto cleanup;

        if (comp_param_len > 0) {
            if ((comp_param = malloc(comp_param_len)) == NULL ||
                (delta->tgt_comp_param = malloc(comp_param_len * 2 + 1)) == NULL) {
                error = DRPM_ERR_MEMORY;
                goto cleanup;
            }

            if ((error = decompstrm_read(stream, comp_param_len, comp_param)) != DRPM_ERR_OK)
                goto cleanup;

            dump_hex(delta->tgt_comp_param, comp_param, comp_param_len);
        }

        printf("target compression parameter block: %s\n", delta->tgt_comp_param);

        if (delta->version == 3) {
            if ((error = decompstrm_read_be32(stream, &delta->tgt_header_len)) != DRPM_ERR_OK ||
                (error = decompstrm_read_be32(stream, &offadjn)) != DRPM_ERR_OK)
                goto cleanup;

            printf("target header length: %u\n", delta->tgt_header_len);

            delta->adj_elems_size = 2 * offadjn;

            if (delta->adj_elems_size > 0) {
                if ((delta->adj_elems = malloc(delta->adj_elems_size * 4)) == NULL) {
                    error = DRPM_ERR_MEMORY;
                    goto cleanup;
                }
                for (uint32_t i = 0; i < delta->adj_elems_size; i += 2)
                    if ((error = decompstrm_read_be32(stream, delta->adj_elems + i)) != DRPM_ERR_OK)
                        goto cleanup;
                for (uint32_t j = 1; j < delta->adj_elems_size; j += 2)
                    if ((error = decompstrm_read_be32(stream, delta->adj_elems + j)) != DRPM_ERR_OK)
                        goto cleanup;
            }

            printf("offset adjustment elements:");
            for (uint32_t i = 0; i < delta->adj_elems_size; i++)
                printf(" %u", delta->adj_elems[i]);
            printf("\n");
        }
    }

    if (delta->tgt_header_len == 0 && delta->type == DRPM_TYPE_RPMONLY) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((error = decompstrm_read_be32(stream, &leadlen)) != DRPM_ERR_OK)
        goto cleanup;

    if (leadlen < RPM_LEADSIG_MIN_LEN) {
        error = DRPM_ERR_FORMAT;
        goto cleanup;
    }

    if ((lead = malloc(leadlen)) == NULL ||
        (delta->tgt_lead = malloc(leadlen * 2 + 1)) == NULL) {
        error = DRPM_ERR_MEMORY;
        goto cleanup;
    }

    if ((error = decompstrm_read(stream, leadlen, lead)) != DRPM_ERR_OK)
        goto cleanup;

    dump_hex(delta->tgt_lead, lead, leadlen);

    printf("target lead: %.50s... (length: %u)\n", delta->tgt_lead, leadlen);

    if ((error = decompstrm_read_be32(stream, &delta->payload_fmt_off)) != DRPM_ERR_OK ||
        (error = decompstrm_read_be32(stream, &inn)) != DRPM_ERR_OK ||
        (error = decompstrm_read_be32(stream, &outn)) != DRPM_ERR_OK)
        goto cleanup;

    printf("payload format offset: %u\n", delta->payload_fmt_off);

    delta->int_copies_size = 2 * inn;
    delta->ext_copies_size = 2 * outn;

    if (delta->int_copies_size > 0) {
        if ((delta->int_copies = malloc(delta->int_copies_size * 4)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        for (uint32_t i = 0; i < delta->int_copies_size; i += 2)
            if ((error = decompstrm_read_be32(stream, delta->int_copies + i)) != DRPM_ERR_OK)
                goto cleanup;
        for (uint32_t j = 1; j < delta->int_copies_size; j += 2)
            if ((error = decompstrm_read_be32(stream, delta->int_copies + j)) != DRPM_ERR_OK)
                goto cleanup;
    }

    printf("internal copies:");
    for (uint32_t i = 0; i < delta->int_copies_size; i++)
        printf(" %u", delta->int_copies[i]);
    printf("\n");

    if (delta->ext_copies_size > 0) {
        if ((delta->ext_copies = malloc(delta->ext_copies_size * 4)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        for (uint32_t i = 0; i < delta->ext_copies_size; i += 2)
            if ((error = decompstrm_read_be32(stream, delta->ext_copies + i)) != DRPM_ERR_OK)
                goto cleanup;
        for (uint32_t j = 1; j < delta->ext_copies_size; j += 2)
            if ((error = decompstrm_read_be32(stream, delta->ext_copies + j)) != DRPM_ERR_OK)
                goto cleanup;
    }

    printf("external copies:");
    for (uint32_t i = 0; i < delta->ext_copies_size; i++)
        printf(" %u", delta->ext_copies[i]);
    printf("\n");

    if (delta->version == 3) {
        if ((error = decompstrm_read_be64(stream, &delta->ext_data_len)) != DRPM_ERR_OK)
            goto cleanup;
    } else {
        if ((error = decompstrm_read_be32(stream, &ext_data_32)) != DRPM_ERR_OK)
            goto cleanup;
        delta->ext_data_len = ext_data_32;
    }

    printf("length of external data: %lu\n", delta->ext_data_len);

    if ((error = decompstrm_read_be32(stream, &add_data_size)) != DRPM_ERR_OK)
        goto cleanup;

    if (add_data_size > 0) {
        if (delta->type == DRPM_TYPE_RPMONLY) {
            error = DRPM_ERR_FORMAT;
            goto cleanup;
        }
        unsigned char *add_data;
        if ((add_data = malloc(add_data_size)) == NULL) {
            error = DRPM_ERR_MEMORY;
            goto cleanup;
        }
        if ((error = decompstrm_read(stream, add_data_size, add_data)) != DRPM_ERR_OK) // 3rd arg = NULL
            goto cleanup;
        printf("add data: ");
        for (uint32_t i = 0; i < add_data_size; i++)
            printf("%02x", add_data[i]);
        printf("\n");
    }

    if (delta->version == 3) {
        if ((error = decompstrm_read_be64(stream, &delta->int_data_len)) != DRPM_ERR_OK)
            goto cleanup;
    } else {
        if ((error = decompstrm_read_be32(stream, &int_data_32)) != DRPM_ERR_OK)
            goto cleanup;
        delta->int_data_len = int_data_32;
    }

    printf("length of internal data: %lu\n", delta->int_data_len);

cleanup:

    if (error == DRPM_ERR_OK)
        error = decompstrm_destroy(&stream);
    else
        decompstrm_destroy(&stream);

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

    unsigned char *add_data;

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

    printf("target NEVR: %s\n", delta->tgt_nevr);

    if ((error = read_be32(filedesc, &add_data_size)) != DRPM_ERR_OK)
        return error;

    //
    if ((add_data = malloc(add_data_size)) == NULL)
        return DRPM_ERR_MEMORY;
    if ((bytes_read = read(filedesc, add_data, add_data_size)) < 0)
        return DRPM_ERR_IO;
    if ((uint32_t) bytes_read != add_data_size)
        return DRPM_ERR_FORMAT;
    printf("add data: ");
    for (uint32_t i = 0; i < add_data_size; i++)
        printf("%02x", add_data[i]);
    printf("\n");
    //

    //if (lseek(filedesc, add_data_size, SEEK_CUR) == (off_t)-1)
        //return DRPM_ERR_IO;

    return DRPM_ERR_OK;
}

int readdelta_standard(int filedesc, struct drpm *delta)
{
    struct rpm *rpmst;
    int error;

    if ((error = rpm_read(&rpmst, delta->filename, RPM_ARCHIVE_DONT_READ, NULL, NULL, NULL)) != DRPM_ERR_OK)
        return error;

    if ((error = rpm_get_nevr(rpmst, &delta->tgt_nevr)) != DRPM_ERR_OK ||
        (error = rpm_get_comp(rpmst, &delta->tgt_comp)) != DRPM_ERR_OK)
        goto cleanup;

    printf("target NEVR: %s\n", delta->tgt_nevr);
    printf("target compression: %s\n", comp2str(delta->tgt_comp));

    if (lseek(filedesc, rpm_size_full(rpmst), SEEK_SET) == (off_t)-1)
        error = DRPM_ERR_IO;

cleanup:
    if (error == DRPM_ERR_OK)
        error = rpm_destroy(&rpmst);
    else
        rpm_destroy(&rpmst);

    return error;
}
