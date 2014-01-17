/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2014 Philip Wernersbach & Jacobs Automation.  All rights reserved.
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

#ifndef HAVE_FS_VFAT_H
#define HAVE_FS_VFAT_H

#define VFAT_FAT32_BYTES_PER_SECTOR_OFFSET          0x0B
#define VFAT_FAT32_BYTES_PER_SECTOR                  512

#define VFAT_FAT32_SECTORS_PER_CLUSTER_OFFSET       0x0D

#define VFAT_FAT32_NUMBER_OF_FATS_OFFSET            0x10
#define VFAT_FAT32_NUMBER_OF_FATS                      2

#define VFAT_FAT32_SERIAL_NUMBER_OFFSET             0x43

#define VFAT_FAT32_VOLUME_LABEL_OFFSET              0x47

#define VFAT_FAT32_FILESYSTEM_TYPE_OFFSET           0x52     
#define VFAT_FAT32_FILESYSTEM_TYPE            "FAT32   "

#define VFAT_FAT32_SIGNATURE_OFFSET                0x1FE
#define VFAT_FAT32_SIGNATURE                      0xAA55

#define VFAT_FAT32_FAT_SIZE                           32
#define VFAT_FAT32_NULL_LABEL               "NO NAME    "

int vfat_mount(char *partition, char *mntbuf, char *fsbuf, int flags, char *mntinfo);
int vfat_mkfs(cdico *d, char *partition, char *fsoptions);
int vfat_umount(char *partition, char *mntbuf);
int vfat_getinfo(cdico *d, char *devname);
int vfat_test(char *devname);
int vfat_get_reqmntopt(char *partition, cstrlist *reqopt, cstrlist *badopt);

struct fat32_volume_id {
	// THIS CANNOT BE READ SEQUENTIALLY!
	//
	// FAT32 is aligned weird, so you have to seek to
	// the offset of the member you're trying to read, before
	// you read it.
	__uint16_t     bytes_per_sector;
	__uint8_t      sectors_per_cluster;
	__uint8_t      number_of_fats;
	unsigned char  serial_number[4];
	char           volume_label[12];
	char           filesystem_type[9];
	__uint16_t     signature;
};

#endif
