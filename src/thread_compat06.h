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

#ifndef __THREAD_COMPAT06_H__
#define __THREAD_COMPAT06_H__

#include <pthread.h>
#include <limits.h>

enum {OLDERR_FATAL=1, OLDERR_MINOR=2};

#define FSA_MAGIC_VOLH           "FsA0" // volume header (one per volume at the very beginning)
#define FSA_MAGIC_VOLF           "FsAE" // volume footer (one per volume at the very end)
#define FSA_MAGIC_MAIN           "ArCh" // archive header (one per archive at the beginning of the first volume)
#define FSA_MAGIC_FSIN           "FsIn" // filesys info (one per filesystem at the beginning of the archive)
#define FSA_MAGIC_FSYB           "FsYs" // filesys begin (one per filesystem when the filesys contents start)
#define FSA_MAGIC_DIRS           "DiRs" // dirs info (one per archive after mainhead before flat dirs/files)
#define FSA_MAGIC_OBJT           "ObJt" // object header (one per object: regfiles, dirs, symlinks, ...)
#define FSA_MAGIC_BLKH           "BlKh" // datablk header (one per data block, each regfile may have [0-n])
#define FSA_MAGIC_FILF           "FiLf" // filedat footer (one per regfile, after the list of data blocks)
#define FSA_MAGIC_DATF           "DaEn" // data footer (one per file system, at the end of its contents, or after the contents of the flatfiles)

struct s_blockinfo;
struct s_headinfo;
struct s_dico;

struct s_archreader;
typedef struct s_archreader carchreader;

struct s_archreader
{
    int    archfd; // file descriptor of the current volume (set to -1 when closed)
    u32    archid; // 32bit archive id for checking (random number generated at creation)
    u64    fscount; // how many filesystems in archive (valid only if archtype=filesystems)
    u32    archtype; // what has been saved in the archive: filesystems or directories
    u32    curvol; // current volume number, starts at 0, incremented when we change the volume
    u32    compalgo; // compression algorithm which has been used to create the archive
    u32    cryptalgo; // encryption algorithm which has been used to create the archive
    u32    complevel; // compression level which is specific to the compression algorithm
    u32    fsacomp; // fsa compression level given on the command line by the user
    u64    creattime; // archive create time (number of seconds since epoch)
    u64    minfsaver; // minimum fsarchiver version required to restore that archive
    u32    hasdirsinfohead; // true if the archive has a "DiRs" header (introduced in 0.6.7)
    int    filefmtver; // set to 1 for "FsArCh_001" or 2 for "FsArCh_002"
    char   filefmt[FSA_MAX_FILEFMTLEN]; // file format of that archive
    char   creatver[FSA_MAX_PROGVERLEN]; // fsa version used to create archive
    char   label[FSA_MAX_LABELLEN]; // archive label defined by the user
    char   basepath[PATH_MAX]; // path of the first volume of an archive
    char   volpath[PATH_MAX]; // path of the current volume of an archive
};

int archreader_init(carchreader *ai);
int archreader_destroy(carchreader *ai);
int archreader_open(carchreader *ai);
int archreader_close(carchreader *ai);
int archreader_incvolume(carchreader *ai, bool waitkeypress);
int archreader_volpath(carchreader *ai);
int archreader_read_data(carchreader *ai, void *data, u64 size);
int archreader_read_dico(carchreader *ai, struct s_dico *d);
int archreader_read_volheader(carchreader *ai);
int archreader_read_header(carchreader *ai, u32 *magic, struct s_dico **d, bool allowseek, u16 *fsid);
int archreader_read_block(carchreader *ai, struct s_dico *in_blkdico, int in_skipblock, int *out_sumok, struct s_blockinfo *out_blkinfo);

void *thread_compat06_fct(void *args);

#endif // __THREAD_COMPAT06_H__
