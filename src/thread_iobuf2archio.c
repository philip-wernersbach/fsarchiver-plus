/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2008-2011 Francois Dupoux.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * Homepage: http://www.fsarchiver.org
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <gcrypt.h>

#include "fsarchiver.h"
#include "archio.h"
#include "options.h"
#include "syncthread.h"
#include "iobuffer.h"
#include "common.h"
#include "error.h"
#include "queue.h"
#include "fec.h"

/*
 * This file implements the error-correction-code that allows to read the
 * archive and restore all data strictly at the original data, even when
 * there are corruptions in the archive file. This uses the FEC algorithm
 * which is implemented in fec.c.
 * 
 * This code is the interface between two levels:
 * - archio:   low-level access to the archive (open/read/write/close)
 * - bufferio: higher-level list of fixed-size blocks of data
 */

struct s_fecblockhead;
typedef struct s_fecblockhead cfecblockhead;

struct __attribute__ ((__packed__)) s_fecblockhead
{
    char md5sum[16];
};

void *thread_iobuf_to_archio_fct(void *args)
{
    char buffer_fec[FSA_FEC_MAXVAL_N * (FSA_FEC_PACKET_SIZE+sizeof(cfecblockhead))];
    char buffer_raw[FSA_FEC_VALUE_K * FSA_FEC_PACKET_SIZE];
    void *fec_src_pkt[FSA_FEC_VALUE_K];
    void *fec_handle = NULL;
    char curvolpath[PATH_MAX];
    cfecblockhead fecchksum;
    int fec_value_n;
    char archive[PATH_MAX];
    carchio *ai = NULL;
    u32 curoffset;
    u32 blocksize;
    u32 bytesused;
    u64 blocknum;
    int res;
    int i;

    // initializations
    fec_value_n = FSA_FEC_VALUE_K + g_options.ecclevel;
    blocknum = 0;
    inc_secthreads();
    blocksize = iobuffer_get_block_size(g_iobuffer);
    assert(blocksize == sizeof(buffer_raw));
    assert(sizeof(cfecblockhead) == 16);

    // initializes FEC
    msgprintf(MSG_DEBUG1, "fec_new(k=%d, n=%d)\n", (int)FSA_FEC_VALUE_K, (int)fec_value_n);
    if ((fec_handle = fec_new(FSA_FEC_VALUE_K, fec_value_n)) == NULL)
    {
        errprintf("fec_new(k=%d, n=%d) failed\n", (int)FSA_FEC_VALUE_K, (int)fec_value_n);
        set_status(STATUS_FAILED, "fec_new() failed");
        goto thread_iobuf_to_archio_cleanup;
    }
    for (i=0; i < FSA_FEC_VALUE_K; i++)
    {
        fec_src_pkt[i] = &buffer_raw[i * FSA_FEC_PACKET_SIZE];
    }

    // initializes archive
    path_force_extension(archive, sizeof(archive), g_archive, ".fsa");
    if ((ai = archio_alloc()) == NULL)
    {
        errprintf("archio_alloc() failed()\n");
        set_status(STATUS_FAILED, "archio_alloc() failed");
        goto thread_iobuf_to_archio_cleanup;
    }
    archio_init_write(ai, archive, g_options.ecclevel);

    // main loop
    while (((res = iobuffer_read_fec_block(g_iobuffer, buffer_raw, blocksize, &bytesused)) == FSAERR_SUCCESS) && (get_status() == STATUS_RUNNING))
    {
        /*
        char debugsum[16];
        memset(debugsum, 0, 16);
        gcry_md_hash_buffer(GCRY_MD_MD5, debugsum, buffer_raw, blocksize);
        u64 *temp1 = (u64 *)(debugsum+0);
        u64 *temp2 = (u64 *)(debugsum+8);
        printf("FDEBUG-SAVE: blocksize=%d blocknum=%d bytesused=%d md5=[%.16llx%.16llx]\n", (int)blocksize, (int)blocknum, (int)bytesused, (long long)*temp1, (long long)*temp2);
        */

        curoffset = 0;
        for (i=0; i < fec_value_n; i++)
        {
            fec_encode(fec_handle, fec_src_pkt, &buffer_fec[curoffset], i, FSA_FEC_PACKET_SIZE);
            gcry_md_hash_buffer(GCRY_MD_MD5, fecchksum.md5sum, &buffer_fec[curoffset], FSA_FEC_PACKET_SIZE);
            memcpy(&buffer_fec[curoffset + FSA_FEC_PACKET_SIZE], &fecchksum, sizeof(cfecblockhead));
            curoffset += FSA_FEC_PACKET_SIZE + sizeof(cfecblockhead);
        }

        if (archio_write_block(ai, buffer_fec, curoffset, bytesused, curvolpath, sizeof(curvolpath)) != 0)
        {
            msgprintf(MSG_STACK, "cannot write block to archive volume %s: archio_write_block() failed\n", curvolpath);
            set_status(STATUS_FAILED, "archio_write_block() failed");
            archio_close_write(ai, false);
            archio_delete_all(ai);
            goto thread_iobuf_to_archio_cleanup;
        }

        blocknum++;
    }

    archio_close_write(ai, true);

thread_iobuf_to_archio_cleanup:
    if (get_status() != STATUS_RUNNING)
        archio_delete_all(ai);
    fec_free(fec_handle);
    archio_destroy(ai);
    msgprintf(MSG_DEBUG1, "THREAD-IOBUF-WRITER: exit\n");
    dec_secthreads();
    return NULL;
}

void *thread_archio_to_iobuf_fct(void *args)
{
    char buffer_fec[FSA_FEC_MAXVAL_N * (FSA_FEC_PACKET_SIZE + sizeof(cfecblockhead))];
    char buffer_raw[FSA_FEC_VALUE_K * FSA_FEC_PACKET_SIZE];
    char buffer_dec[FSA_FEC_VALUE_K * FSA_FEC_PACKET_SIZE];
    void *fec_src_pkt[FSA_FEC_VALUE_K];
    int fec_indexes[FSA_FEC_VALUE_K];
    void *fec_handle = NULL;
    char curvolpath[PATH_MAX];
    int fec_value_n = 0;
    carchio *ai = NULL;
    char md5sum[16];
    int curoffset;
    int goodpkts;
    int badpkts;
    u32 encodedsize;
    u32 blocksize;
    u32 bytesused;
    u64 blocknum;
    u32 ecclevel;
    int res;
    int i;

    // initializations
    blocknum = 0;
    inc_secthreads();
    blocksize = iobuffer_get_block_size(g_iobuffer);
    assert(blocksize == sizeof(buffer_raw));
    assert(sizeof(cfecblockhead) == 16);

    // initializes archive
    if ((ai = archio_alloc()) == NULL)
    {
        errprintf("archio_alloc() failed\n");
        set_status(STATUS_FAILED, "archio_alloc() failed");
        goto thread_archio_to_iobuf_cleanup;
    }
    if (archio_init_read(ai, g_archive, &ecclevel) != 0)
    {
        errprintf("archio_init_read() failed\n");
        set_status(STATUS_FAILED, "archio_init_read() failed");
        goto thread_archio_to_iobuf_cleanup;
    }

    // initializes FEC
    fec_value_n = FSA_FEC_VALUE_K + ecclevel;
    if ((fec_value_n < FSA_FEC_VALUE_K) || (fec_value_n > FSA_FEC_MAXVAL_N))
    {
        errprintf("invalid value for fec_value_n found in the main FEC header: %d\n", (int)fec_value_n);
        set_status(STATUS_FAILED, "fec_value_n is invalid");
        goto thread_archio_to_iobuf_cleanup;
    }

    msgprintf(MSG_DEBUG1, "fec_new(k=%d, n=%d)\n", (int)FSA_FEC_VALUE_K, (int)fec_value_n);
    if ((fec_handle = fec_new(FSA_FEC_VALUE_K, fec_value_n)) == NULL)
    {
        errprintf("fec_new(k=%d, n=%d) failed\n", (int)FSA_FEC_VALUE_K, (int)fec_value_n);
        set_status(STATUS_FAILED, "fec_new() failed");
        goto thread_archio_to_iobuf_cleanup;
    }

    // read all fec encoded blocks from archive (one fec encoded block = N packets)
    encodedsize = fec_value_n * (FSA_FEC_PACKET_SIZE + sizeof(cfecblockhead));
    while (((res = archio_read_block(ai, buffer_fec, encodedsize, &bytesused, curvolpath, sizeof(curvolpath))) == FSAERR_SUCCESS) && (get_status() == STATUS_RUNNING))
    {
        goodpkts = 0;
        badpkts = 0;
        curoffset = 0;

        // try to read K good packets from the list of N packets
        for (i=0; (goodpkts < FSA_FEC_VALUE_K) && (i < fec_value_n); i++)
        {
            // simulate corruptions in the low-level block from the archive
            /*struct timeval now;
            gettimeofday(&now, NULL);
            if ((now.tv_usec % 11) == 0)
            {
                 memset(&buffer_fec[curoffset], 0xFF, FSA_FEC_PACKET_SIZE);
            }*/

            gcry_md_hash_buffer(GCRY_MD_MD5, md5sum, &buffer_fec[curoffset], FSA_FEC_PACKET_SIZE);
            if (memcmp(md5sum, &buffer_fec[curoffset + FSA_FEC_PACKET_SIZE], sizeof(cfecblockhead)) == 0)
            {
                fec_src_pkt[goodpkts] = &buffer_raw[goodpkts * FSA_FEC_PACKET_SIZE];
                memcpy(fec_src_pkt[goodpkts], &buffer_fec[curoffset], FSA_FEC_PACKET_SIZE);
                fec_indexes[goodpkts] = i;
                goodpkts++;
            }
            else
            {
                badpkts++;
            }
            curoffset += FSA_FEC_PACKET_SIZE + sizeof(cfecblockhead);
        }

        if (goodpkts == FSA_FEC_VALUE_K) // enough good packets found
        {
            res = fec_decode(fec_handle, fec_src_pkt, fec_indexes, FSA_FEC_PACKET_SIZE);
            for (i=0; i < FSA_FEC_VALUE_K; i++)
            {
                memcpy(&buffer_dec[i * FSA_FEC_PACKET_SIZE], fec_src_pkt[i], FSA_FEC_PACKET_SIZE);
            }

            /*
            char debugsum[16]; 
            memset(debugsum, 0, 16);
            gcry_md_hash_buffer(GCRY_MD_MD5, debugsum, buffer_dec, blocksize);
            u64 *temp1 = (u64 *)(debugsum+0);
            u64 *temp2 = (u64 *)(debugsum+8);
            printf("FDEBUG-REST: blocksize=%d blocknum=%d md5=[%.16llx%.16llx] blocksize=%d, bytesused=%d\n", (int)blocksize, (int)blocknum, (long long)*temp1, (long long)*temp2, (int)blocksize, (int)bytesused);
            */

            if (iobuffer_write_fec_block(g_iobuffer, buffer_dec, blocksize, bytesused) != 0)
            {
                errprintf("iobuffer_write_fec_block() failed\n");
                set_status(STATUS_FAILED, "iobuffer_write_fec_block() failed");
                goto thread_archio_to_iobuf_cleanup;
            }

            if (badpkts > 0) // if errors have been found in the FEC packets
            {
                errprintf("the error-correction-code has fixed all corruptions in archive volume %s at block %lld: %d bad packets out of %d packets\n", curvolpath, (long long)blocknum, (int)badpkts, (int)fec_value_n);
            }
        }
        else if (goodpkts < FSA_FEC_VALUE_K) // too many bad packets
        {
            errprintf("cannot fix corruptions in archive volume %s at block %lld: too many bad packets (%d bad packets out of %d packets)\n", curvolpath, (long long)blocknum, (int)badpkts, (int)fec_value_n);
        }

        blocknum++;
    }

thread_archio_to_iobuf_cleanup:
    iobuffer_set_end_of_buffer(g_iobuffer, true);
    archio_close_read(ai);
    fec_free(fec_handle);
    archio_destroy(ai);
    msgprintf(MSG_DEBUG1, "THREAD-IOBUF-READER: exit\n");
    dec_secthreads();
    return NULL;
}
