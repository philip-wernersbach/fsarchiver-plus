/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2008-2012 Francois Dupoux.  All rights reserved.
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

#include <signal.h>
#include <stdio.h>

#include "fsarchiver.h"
#include "fsarchiver.h"
#include "syncthread.h"
#include "iobuffer.h"
#include "queue.h"
#include "error.h"

// queue use to share data between the three sort of threads
cqueue *g_queue = NULL;
ciobuffer *g_iobuffer = NULL;

// filesystem bitmap used by do_extract() to say to threadio_readimg which filesystems to skip
// eg: "g_fsbitmap[0]=1,g_fsbitmap[1]=0" means that we want to read filesystem 0 and skip fs 1
u8 g_fsbitmap[FSA_MAX_FSPERARCH];

// g_stopfillqueue is set to true when the threads that reads the queue wants to stop
// either because there is an error or because it does not need the next data
atomic_t g_status = { (STATUS_RUNNING) };

// how many secondary threads are running (compression/decompression and archio threads)
atomic_t g_secthreads={ (0) };

char *get_status_text(int status)
{
    switch (status)
    {
        case STATUS_RUNNING:
            return "STATUS_RUNNING";
        case STATUS_FINISHED:
            return "STATUS_FINISHED";
        case STATUS_ABORTED:
            return "STATUS_ABORTED";
        case STATUS_FAILED:
            return "STATUS_FAILED";
        default:
            return "STATUS_??????";
    };
};

void inc_secthreads()
{
    (void)__sync_add_and_fetch(&g_secthreads.counter, 1);
}

void dec_secthreads()
{
    (void)__sync_sub_and_fetch(&g_secthreads.counter, 1);
}

int get_secthreads()
{
    return atomic_read(&g_secthreads);
}

void set_status(int status, char *context)
{
    atomic_set(&g_status, status);
    msgprintf(MSG_DEBUG1, "set_status(status=[%d]=[%s], context=[%s])\n", status, get_status_text(status), context);
}

int get_status()
{
    int mysigs[]={SIGINT, SIGTERM, -1};
    sigset_t mask_set;
    int old_status;
    int i;

    old_status = atomic_read(&g_status);

    // no need to worry about SIGINT/SIGTERM if already aborted / failed
    if (old_status != STATUS_RUNNING)
    {
        return old_status;
    }

    // if status is running then check signals SIGINT/SIGTERM
    if (sigpending(&mask_set) == 0)
    {
        for (i=0; mysigs[i] != -1; i++)
        {
            if (sigismember(&mask_set, mysigs[i]))
            {
                fprintf(stderr, "get_status(): received signal %d\n", mysigs[i]);
                atomic_set(&g_status, STATUS_ABORTED);
                return STATUS_ABORTED;
            }
        }
    }

    return old_status;
}
