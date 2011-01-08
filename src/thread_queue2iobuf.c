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

#include "fsarchiver.h"
#include "archio.h"
#include "iobuffer.h"
#include "dico.h"
#include "common.h"
#include "error.h"
#include "syncthread.h"
#include "queue.h"

void *thread_queue_to_iobuf_fct(void *args)
{
    struct s_headinfo headinfo;
    struct s_blockinfo blkinfo;
    s64 blknum;
    int type;

    inc_secthreads();

    while (queue_get_end_of_queue(g_queue) == false)
    {
        if (((blknum = queue_dequeue_first(g_queue, &type, &headinfo, &blkinfo)) < 0) && (blknum != FSAERR_ENDOFFILE)) // error
        {
            msgprintf(MSG_STACK, "queue_dequeue_first()=%ld=%s failed\n", (long)blknum, error_int_to_string(blknum));
            set_status(STATUS_FAILED, "queue_dequeue_first() failed");
            goto thread_queue_to_iobuf_cleanup;
        }
        else if (blknum > 0) // block or header found
        {
            switch (type)
            {
                case QITEM_TYPE_BLOCK:
                    if (iobuffer_write_block(g_iobuffer, &blkinfo, blkinfo.blkfsid)!=0)
                    {
                        msgprintf(MSG_STACK, "iobuffer_write_block() failed\n");
                        set_status(STATUS_FAILED, "iobuffer_write_block() failed");
                        goto thread_queue_to_iobuf_cleanup;
                    }
                    free(blkinfo.blkdata);
                    break;

                case QITEM_TYPE_HEADER:
                    if (iobuffer_write_logichead(g_iobuffer, headinfo.dico, headinfo.headertype, headinfo.fsid) != 0)
                    {
                        msgprintf(MSG_STACK, "iobuffer_write_logichead() failed\n");
                        set_status(STATUS_FAILED, "iobuffer_write_logichead() failed");
                        goto thread_queue_to_iobuf_cleanup;
                    }
                    dico_destroy(headinfo.dico);
                    break;

                default:
                    errprintf("unexpected item type from queue: type=%d\n", type);
                    set_status(STATUS_FAILED, "unexpected item type from queue");
                    goto thread_queue_to_iobuf_cleanup;
                    break;
            }
        }
    }

thread_queue_to_iobuf_cleanup:
    iobuffer_set_end_of_buffer(g_iobuffer, true);
    msgprintf(MSG_DEBUG1, "THREAD-DEQUEUE: exit\n");
    dec_secthreads();
    return NULL;
}

void *thread_iobuf_to_queue_fct(void *args)
{
    struct s_blockinfo blkinfo;
    cdico *dico = NULL;
    u32 headertype;
    u16 fsindex;
    int sumok;
    int status;
    s64 lres;
    int res;

    inc_secthreads();    

    while ((iobuffer_get_end_of_buffer(g_iobuffer) == false))
    {
        if (((res = iobuffer_read_logichead(g_iobuffer, &headertype, &dico, &fsindex)) != FSAERR_SUCCESS) && (get_status() == STATUS_RUNNING))
        {
            dico_destroy(dico);
            msgprintf(MSG_STACK, "iobuffer_read_logichead() failed to read next logic-header\n");
            set_status(STATUS_FAILED, "iobuffer_read_logichead() failed");
            goto thread_iobuf_to_queue_cleanup;
        }

        if (headertype == FSA_HEADTYPE_BLKH) // header introduces a data block
        {
            if ((iobuffer_read_block(g_iobuffer, dico, &sumok, &blkinfo) != FSAERR_SUCCESS) && (get_status() == STATUS_RUNNING))
            {
                msgprintf(MSG_STACK, "iobuffer_read_block() failed\n");
                set_status(STATUS_FAILED, "iobuffer_read_block() failed");
                goto thread_iobuf_to_queue_cleanup;
            }

            if (g_fsbitmap[fsindex] == 1)
            {
                status = ((sumok == true) ? QITEM_STATUS_TODO : QITEM_STATUS_DONE);
                if ((lres = queue_add_block(g_queue, &blkinfo, status)) != FSAERR_SUCCESS)
                {
                    set_status(STATUS_FAILED, "queue_add_block() failed");
                    goto thread_iobuf_to_queue_cleanup;
                }
                dico_destroy(dico);
            }
        }
        else // current header does not introduce a block
        {
            // if it's a global header or a if this local header belongs to a filesystem that the main thread needs
            if ((fsindex == FSA_FILESYSID_NULL) || (g_fsbitmap[fsindex] == 1))
            {
                if ((lres = queue_add_header(g_queue, dico, headertype, fsindex)) != FSAERR_SUCCESS)
                {
                    msgprintf(MSG_STACK, "queue_add_header()=%ld=%s failed\n", (long)lres, error_int_to_string(lres));
                    set_status(STATUS_FAILED, "iobuffer_read_block() failed");
                    goto thread_iobuf_to_queue_cleanup;
                }
            }
            else // header not used: remove data strucutre in dynamic memory
            {
                dico_destroy(dico);
            }
        }
    }

thread_iobuf_to_queue_cleanup:
    queue_set_end_of_queue(g_queue, true);
    msgprintf(MSG_DEBUG1, "THREAD-ENQUEUE: exit\n");
    dec_secthreads();
    return NULL;
}
