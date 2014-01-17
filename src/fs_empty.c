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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mount.h>

#include <uuid.h>

#include "fsarchiver.h"
#include "dico.h"
#include "common.h"
#include "strlist.h"
#include "filesys.h"
#include "fs_empty.h"
#include "error.h"

int empty_mount(char *partition, char *mntbuf, char *fsbuf, int flags, char *mntinfo)
{
	msgprintf(MSG_DEBUG1, "empty_mount(partition=[%s], mnt=[%s], fsbuf=[%s])\n", partition, mntbuf, fsbuf);
	return generic_mount("none", mntbuf, "tmpfs", "size=4k", flags);
}

int empty_umount(char *partition, char *mntbuf)
{
    msgprintf(MSG_DEBUG1, "empty_umount(partition=[%s], mnt=[%s])\n", partition, mntbuf);
	return generic_umount(mntbuf);
}
