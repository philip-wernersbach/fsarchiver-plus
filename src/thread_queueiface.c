/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2008-2010 Francois Dupoux.  All rights reserved.
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

#include "fsarchiver.h"
#include "archio.h"
#include "iobuffer.h"
#include "dico.h"
#include "common.h"
#include "error.h"
#include "syncthread.h"
#include "queue.h"

void *thread_dequeue_fct(void *args)
{
    struct s_headinfo headinfo;
    struct s_blockinfo blkinfo;
    s64 blknum;
    int type;

    // init
    inc_secthreads();

    while ((queue_get_end_of_queue(g_queue) == false) && (get_status() == STATUS_RUNNING))
    {
        if (((blknum = queue_dequeue_first(g_queue, &type, &headinfo, &blkinfo)) < 0) && (blknum != FSAERR_ENDOFFILE)) // error
        {
            msgprintf(MSG_STACK, "queue_dequeue_first()=%ld=%s failed\n", (long)blknum, error_int_to_string(blknum));
            goto thread_dequeue_fct_error;
        }
        else if (blknum > 0) // block or header found
        {
            switch (type)
            {
                case QITEM_TYPE_BLOCK:
                    if (iobuffer_write_block(g_iobuffer, &blkinfo, blkinfo.blkfsid)!=0)
                    {
                        msgprintf(MSG_STACK, "archive_dowrite_block() failed\n");
                        goto thread_dequeue_fct_error;
                    }
                    free(blkinfo.blkdata);
                    break;
                case QITEM_TYPE_HEADER:
                    if (iobuffer_write_header(g_iobuffer, headinfo.dico, headinfo.magic, headinfo.fsid) != 0)
                    {
                        msgprintf(MSG_STACK, "archive_write_header() failed\n");
                        goto thread_dequeue_fct_error;
                    }
                    dico_destroy(headinfo.dico);
                    break;
                default:
                    errprintf("unexpected item type from queue: type=%d\n", type);
                    break;
            }
        }
    }

    if (get_status() != STATUS_RUNNING)
        goto thread_dequeue_fct_error;

    iobuffer_set_end_of_buffer(g_iobuffer, true);
    msgprintf(MSG_DEBUG1, "THREAD-DEQUEUE: exit success\n");
    dec_secthreads();
    return NULL;

thread_dequeue_fct_error:
    set_status(STATUS_FAILED);
    while (queue_get_end_of_queue(g_queue)==false) // wait until all the compression threads exit
        queue_destroy_first_item(g_queue); // empty queue
    iobuffer_set_end_of_buffer(g_iobuffer, true); // close iobuffer properly
    msgprintf(MSG_DEBUG1, "THREAD-DEQUEUE: exit failure\n");
    dec_secthreads();
    return NULL;
}

void *thread_enqueue_fct(void *args)
{
    char magic[FSA_SIZEOF_MAGIC];
    struct s_blockinfo blkinfo;
    cdico *dico = NULL;
    u16 fsid;
    int sumok;
    int status;
    u64 errors;
    s64 lres;
    int res;

    // init
    errors=0;
    inc_secthreads();    

    while ((iobuffer_get_end_of_buffer(g_iobuffer) == false) && (get_status() == STATUS_RUNNING))
    {
        //if ((res = archreader_read_header(ai, magic, &dico, true, &fsid))!=FSAERR_SUCCESS)
        if ((res = iobuffer_read_header(g_iobuffer, magic, &dico, &fsid)) != FSAERR_SUCCESS)
        {
            dico_destroy(dico);
            msgprintf(MSG_STACK, "archreader_read_header() failed to read next header\n");
            if (res == OLDERR_MINOR) // header is corrupt or not what we expected
            {
                errors++;
                msgprintf(MSG_DEBUG1, "OLDERR_MINOR\n");
                continue;
            }
            else // fatal error (eg: cannot read archive from disk)
            {
                msgprintf(MSG_DEBUG1, "!OLDERR_MINOR\n");
                goto thread_enqueue_fct_error;
            }
        }

        if (strncmp(magic, FSA_MAGIC_BLKH, FSA_SIZEOF_MAGIC) == 0) // header introduces a data block
        {
            //if (archreader_read_block(ai, dico, skipblock, &sumok, &blkinfo)!=0)
            if (iobuffer_read_block(g_iobuffer, dico, &sumok, &blkinfo) != 0)
            {
                msgprintf(MSG_STACK, "archreader_read_block() failed\n");
                goto thread_enqueue_fct_error;
            }

            if (g_fsbitmap[fsid] == 1)
            {
                status = ((sumok == true) ? QITEM_STATUS_TODO : QITEM_STATUS_DONE);
                if ((lres = queue_add_block(g_queue, &blkinfo, status)) != FSAERR_SUCCESS)
                {   if (lres != FSAERR_NOTOPEN)
                        errprintf("queue_add_block()=%ld=%s failed\n", (long)lres, error_int_to_string(lres));
                    goto thread_enqueue_fct_error;
                }
                if (sumok == false) errors++;
                dico_destroy(dico);
            }
        }
        else // current header does not introduce a block
        {
            // if it's a global header or a if this local header belongs to a filesystem that the main thread needs
            if ((fsid == FSA_FILESYSID_NULL) || (g_fsbitmap[fsid] == 1))
            {
                if ((lres = queue_add_header(g_queue, dico, magic, fsid)) != FSAERR_SUCCESS)
                {
                    msgprintf(MSG_STACK, "queue_add_header()=%ld=%s failed\n", (long)lres, error_int_to_string(lres));
                    goto thread_enqueue_fct_error;
                }
            }
            else // header not used: remove data strucutre in dynamic memory
            {
                dico_destroy(dico);
            }
        }
    }

    queue_set_end_of_queue(g_queue, true);
    msgprintf(MSG_DEBUG1, "THREAD-ENQUEUE: exit success\n");
    dec_secthreads();
    return NULL;

thread_enqueue_fct_error:
    set_status(STATUS_FAILED);
    queue_set_end_of_queue(g_queue, true);
    msgprintf(MSG_DEBUG1, "THREAD-ENQUEUE: exit failure\n");
    dec_secthreads();
    return NULL;
}
