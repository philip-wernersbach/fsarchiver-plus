/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2014 Philip Wernersbach & Jacobs Automation. All rights reserved.
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

#include "dico.h"
#include "strlist.h"
#include "limits.h"

#ifndef HAVE_FS_SWAP_H
#define HAVE_FS_SWAP_H

#define SWAP_VERSION1_VERSION_NUMBER 1

#define SWAP_PAGE_SIZE_BASE 2048
#define SWAP_PAGE_SIZE_MULTIPLE 2
#define SWAP_PAGE_SIZE_MAX   ULONG_MAX


#define SWAP_VERSION1_MAGIC  "SWAPSPACE2"
#define SWAP_MAGIC_OFFSET(a)       (a-10)

#define SWAP_UUID_OFFSET           0x40C
#define SWAP_LABEL_OFFSET          0x41C

struct swap_info {
	     __uint64_t          page_size;
         char                magic[11];
         char                 uuid[16];
         char                label[16];
};

int swap_mkfs(cdico *d, char *partition, char *fsoptions);
int swap_getinfo(cdico *d, char *devname);
int swap_test(char *devname);
int swap_get_reqmntopt(char *partition, cstrlist *reqopt, cstrlist *badopt);

#endif
