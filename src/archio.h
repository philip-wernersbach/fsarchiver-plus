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

#ifndef __ARCHIO_H__
#define __ARCHIO_H__

#include <limits.h>
#include "strlist.h"

struct s_writebuf;
struct s_blockinfo;
struct s_headinfo;
struct s_strlist;

struct s_archio;
typedef struct s_archio carchio;

struct s_iohead;
typedef struct s_iohead ciohead;

struct s_archio
{
    int      archfd; // file descriptor of the current volume (set to -1 when closed)
    u32      archid; // 32bit archive id for checking (random number generated at creation)
    u32      curvol; // current volume number, starts at 0, incremented when we change the volume
    u64      curblock; // index of the last low-level block that has been written
    u32      ecclevel; // read in the volume header, required for higher layer
    bool     newarch; // true when the archive has been created by then current process
    bool     lastvol; // set to true when we open the last volume
    char     basepath[PATH_MAX]; // path of the first volume of an archive
    char     volpath[PATH_MAX]; // path of the current volume of an archive
    cstrlist vollist; // paths to all volumes of an archive
};

enum s_ioheadtype {IOHEAD_VOLHEAD=1, IOHEAD_VOLFOOT=2, IOHEAD_BLKHEAD=3, IOHEAD_BLKFOOT=4};

struct __attribute__ ((__packed__)) s_iohead
{
    u32 magic; // always set to FSA_MAGIC_IOPHYSHEADER
    u32 archid; // archive specific identifier
    u32 csum; // 32 bit checksum of the data
    u16 type; // (vol-head, blk-head)
    union
    {
        struct {u32 volnum; u64 minver; u32 ecclevel; u8 lastvol;} __attribute__ ((__packed__)) volhead;
        struct {u64 blknum; u32 blkid; u32 bytesused;} __attribute__ ((__packed__)) blkhead;
        char maxsize[18]; // reserve a fixed size for specific data
    }
    data;
};

carchio *archio_alloc();
int archio_destroy(carchio *ai);
int archio_delete_all(carchio *ai);
int archio_generate_id(carchio *ai);
int archio_open_read(carchio *ai);
int archio_open_write(carchio *ai);
int archio_init_read(carchio *ai, char *basepath, u32 *ecclevel);
int archio_init_write(carchio *ai, char *basepath, u32 ecclevel);
int archio_close_read(carchio *ai);
int archio_close_write(carchio *ai, bool lastvol);
int archio_read_low_level(carchio *ai, void *data, u32 bufsize);
int archio_write_low_level(carchio *ai, char *buffer, u32 bufsize);
s64 archio_get_currentpos(carchio *ai);
int archio_read_block(carchio *ai, char *buffer, u32 datsize, u32 *bytesused, char *volpathbuf, int volpathbufsize);
int archio_write_block(carchio *ai, char *buffer, u32 bufsize, u32 bytes_used, char *volpathbuf, int volpathbufsize);
int archio_incvolume(carchio *ai);
int archio_split_check(carchio *ai, u32 size);

#endif // __ARCHIO_H__
