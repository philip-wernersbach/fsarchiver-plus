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

#ifndef __ARCHINFO_H__
#define __ARCHINFO_H__

struct s_dico;

struct s_archinfo;
typedef struct s_archinfo carchinfo;

struct s_archinfo
{
    u32    archtype; // what has been saved in the archive: filesystems or directories
    u64    fscount; // how many filesystems in archive (valid only if archtype=filesystems)
    u64    ptcount; // how many partition tables have been saved (valid only if archtype=filesystems)
    u32    compalgo; // compression algorithm which has been used to create the archive
    u32    cryptalgo; // encryption algorithm which has been used to create the archive
    u32    complevel; // compression level which is specific to the compression algorithm
    u32    fsacomp; // fsa compression level given on the command line by the user
    u64    creattime; // archive create time (number of seconds since epoch)
    u64    minfsaver; // minimum fsarchiver version required to restore that archive
    char   filefmt[FSA_MAX_FILEFMTLEN]; // file format of that archive
    char   creatver[FSA_MAX_PROGVERLEN]; // fsa version used to create archive
    char   label[FSA_MAX_LABELLEN]; // archive label defined by the user
};

int archinfo_show_mainhead(carchinfo *archinfo);
int archinfo_show_fshead(struct s_dico *dicofshead, int fsid);
char *compalgostr(int algo);
char *cryptalgostr(int algo);

#endif // __ARCHINFO_H__
