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

#ifndef __LAYOUT_READ_H__
#define __LAYOUT_READ_H__

struct s_strdico;

struct s_diskspecs;
typedef struct s_diskspecs cdiskspecs;

struct s_partspecs;
typedef struct s_partspecs cpartspecs;

struct s_diskspecs
{   s64  partcount;
    char model[1024];
    char path[1024];
    s64  length;
    s64  sectsize;
    char size[1024];
    char disklabel[1024];
};

struct s_partspecs
{   s64  num;
    s64  start;
    s64  length;
    s64  end;
    char name[512];
    char type[2048];
    char flags[2048];
    char fstype[512];
};

int parttable_read_xml_dump(char *tmparchive, struct s_strdico *dico);
int convert_dico_to_device(struct s_strdico *dico, cdiskspecs *disk);
int convert_dico_to_partition(struct s_strdico *dico, cpartspecs *part, int partnum);

#endif // __LAYOUT_READ_H__

