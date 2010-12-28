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

#ifndef __IOBUFFER_H__
#define __IOBUFFER_H__

#include <pthread.h>

struct s_iobuffer;
typedef struct s_iobuffer ciobuffer;
struct s_ioblock;
typedef struct s_ioblock cioblock;
struct s_blockinfo;
struct s_dico;

struct s_iobuffer
{
    pthread_mutex_t mutex; // pthread mutex for data protection
    pthread_cond_t cond; // condition for pthread synchronization
    cioblock *list_head; // linked list of FEC blocks
    u32 blocks_maxcnt; // maximum number of blocks in the linked list
    u32 blocks_curcnt; // current number of blocks in the linked list
    u32 blocks_size; // size of a block (FSA_FEC_VALUE_K * FSA_FEC_CHUNK_SIZE)
    bool endofbuffer; // true when no more data to put in queue (like eof)
};

struct s_ioblock
{
    char *data;
    int bytes_used;
    int bytes_ptr;
    bool eof;
    cioblock *next;
};

ciobuffer *iobuffer_alloc(u32 blocks_maxcnt, u32 blksize);
int iobuffer_destroy(ciobuffer *iob);
int iobuffer_set_end_of_buffer(ciobuffer *iob, bool state);
u32 iobuffer_get_block_size(ciobuffer *iob);
bool iobuffer_get_end_of_buffer_locked(ciobuffer *iob);
bool iobuffer_get_end_of_buffer(ciobuffer *iob);

int iobuffer_write_raw_data(ciobuffer *iob, char *buffer, u32 datasize);
int iobuffer_write_block(ciobuffer *iob, struct s_blockinfo *blkinfo, u16 fsid);
int iobuffer_write_header(ciobuffer *iob, struct s_dico *d, char *magic, u16 fsid);
int iobuffer_write_dico(ciobuffer *wb, struct s_dico *d, char *magic);
int iobuffer_read_fec_block(ciobuffer *iob, char *buffer, u32 bufsize, u32 *bytesused);

int iobuffer_read_raw_data(ciobuffer *iob, char *buffer, u32 datasize);
int iobuffer_read_block(ciobuffer *iob, struct s_dico *in_blkdico, int *out_sumok, struct s_blockinfo *out_blkinfo);
int iobuffer_read_header(ciobuffer *iob, char *magic, struct s_dico **d, u16 *fsid);
int iobuffer_read_dico(ciobuffer *iob, struct s_dico *d);
int iobuffer_write_fec_block(ciobuffer *iob, char *buffer, u32 bufsize, u32 bytesused);

#endif // __IOBUFFER_H__
