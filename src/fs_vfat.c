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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

#include <sys/types.h>
#include <sys/stat.h>

#include <uuid.h>

#include "fsarchiver.h"
#include "dico.h"
#include "common.h"
#include "strlist.h"
#include "filesys.h"
#include "fs_vfat.h"
#include "error.h"

int vfat_mount(char *partition, char *mntbuf, char *fsbuf, int flags, char *mntinfo)
{
	msgprintf(MSG_DEBUG1, "vfat_mount(partition=[%s], mnt=[%s], fsbuf=[%s])\n", partition, mntbuf, fsbuf);
	return generic_mount(partition, mntbuf, fsbuf, "", flags);;
}

int vfat_mkfs(cdico *d, char *partition, char *fsoptions)
{ 
	char command[2048];
    char buffer[2048];
    char options[2048];
    int exitst;
    unsigned int temp;
    
	msgprintf(MSG_DEBUG1, "vfat_mount(cdico=[???], partition=[%s], fsoptions=[%s])\n", partition, fsoptions);
    
    if (exec_command(command, sizeof(command), NULL, NULL, 0, NULL, 0, "mkdosfs")!=0)
    {   errprintf("mkdosfs not found. please install dosfstools on your system or check the PATH.\n");
        return -1;
    }
    
    memset(options, 0, sizeof(options));

    strlcatf(options, sizeof(options), " %s ", fsoptions);

	// ---- label
    if (dico_get_string(d, 0, FSYSHEADKEY_FSLABEL, buffer, sizeof(buffer))==0 && strlen(buffer)>0)
		strlcatf(options, sizeof(options), " -n '%s' ", buffer);
    
    // ---- uuid
    if (dico_get_string(d, 0, FSYSHEADKEY_FSUUID, buffer, sizeof(buffer))==0 && strlen(buffer)>0)
	{
		// FAT32 uses shorter UUID's, so truncate it.
		buffer[8] = '\0';
		
		strlcatf(options, sizeof(options), " -i '%s' ", buffer);
    }
    
    // ---- block size
    if ((dico_get_u16(d, 0, FSYSHEADKEY_FSVFATSECTORSPERCLUSTER, (u16 *)&temp) == 0) && (temp > 0))
		strlcatf(options, sizeof(options), " -s '%u' ", temp);
    
    // ---- FAT size
    if ((dico_get_u32(d, 0, FSYSHEADKEY_FSVFATFATSIZE, &temp) == 0) && (temp > 0))
		strlcatf(options, sizeof(options), " -F '%u' ", temp);
    
    if (exec_command(command, sizeof(command), &exitst, NULL, 0, NULL, 0, "mkdosfs %s %s", options, partition)!=0 || exitst!=0)
    {   errprintf("command [%s] failed\n", command);
        return -1;
    }
    
	return 0;
}

int vfat_umount(char *partition, char *mntbuf)
{
    msgprintf(MSG_DEBUG1, "vfat_umount(partition=[%s], mnt=[%s])\n", partition, mntbuf);
	return generic_umount(mntbuf);
}

static int read_fat32_volume_info(int fd, struct fat32_volume_id *vi)
{
	int last_character;
	unsigned char temp;
	
	// Read the bytes per sector.
	if (lseek64(fd, VFAT_FAT32_BYTES_PER_SECTOR_OFFSET, SEEK_SET) != VFAT_FAT32_BYTES_PER_SECTOR_OFFSET)
        return false;
    
    if (read(fd, &vi->bytes_per_sector, sizeof(vi->bytes_per_sector))!=sizeof(vi->bytes_per_sector))
        return false;
    
    // Read the sectors per cluster.
	if (lseek64(fd, VFAT_FAT32_SECTORS_PER_CLUSTER_OFFSET, SEEK_SET) != VFAT_FAT32_SECTORS_PER_CLUSTER_OFFSET)
        return false;
    
    if (read(fd, &vi->sectors_per_cluster, sizeof(vi->sectors_per_cluster))!=sizeof(vi->sectors_per_cluster))
        return false;
    
    // Read the number of FATS.
	if (lseek64(fd, VFAT_FAT32_NUMBER_OF_FATS_OFFSET, SEEK_SET) != VFAT_FAT32_NUMBER_OF_FATS_OFFSET)
        return false;
    
    if (read(fd, &vi->number_of_fats, sizeof(vi->number_of_fats))!=sizeof(vi->number_of_fats))
        return false;
    
    // Read the FAT32 serial number.
	if (lseek64(fd, VFAT_FAT32_SERIAL_NUMBER_OFFSET, SEEK_SET) != VFAT_FAT32_SERIAL_NUMBER_OFFSET)
        return false;
    
    if (read(fd, &vi->serial_number, sizeof(vi->serial_number))!=sizeof(vi->serial_number))
        return false;
    
    // Read the FAT32 volume label.
	if (lseek64(fd, VFAT_FAT32_VOLUME_LABEL_OFFSET, SEEK_SET) != VFAT_FAT32_VOLUME_LABEL_OFFSET)
        return false;
    
    if (read(fd, &vi->volume_label, sizeof(vi->volume_label))!=sizeof(vi->volume_label))
        return false;
    
    // Read the FAT32 signature.
	if (lseek64(fd, VFAT_FAT32_SIGNATURE_OFFSET, SEEK_SET) != VFAT_FAT32_SIGNATURE_OFFSET)
        return false;
    
    if (read(fd, &vi->signature, sizeof(vi->signature))!=sizeof(vi->signature))
        return false;
    
    // Read the FAT32 filesystem type.
	if (lseek64(fd, VFAT_FAT32_FILESYSTEM_TYPE_OFFSET, SEEK_SET) != VFAT_FAT32_FILESYSTEM_TYPE_OFFSET)
        return false;
    
    if (read(fd, &vi->filesystem_type, sizeof(vi->filesystem_type))!=sizeof(vi->filesystem_type))
        return false;
    
    // volume_label is a string, so make sure that it has a null at the end
    vi->volume_label[11] = '\0';
    
    // so is filesystem_type
    vi->filesystem_type[8] = '\0';
    
    // Test if the volume label is the default FAT32 NULL volume label
    if (strcmp(vi->volume_label, VFAT_FAT32_NULL_LABEL) != 0)
    {		
		// volume_label is padded with spaces that we don't want,
		// so unpad it.
		for (last_character = sizeof(vi->volume_label)-2; vi->volume_label[last_character] == ' '; last_character--) 
		{
			// Need this so GCC doesn't optimize out our loop.
			asm("");
		}
		
		vi->volume_label[last_character+1] = '\0';
	} else {
		// Don't pass the NULL volume label through.
		vi->volume_label[0] = '\0';
	}
	
	// For some reason, the serial number in FAT32 is backwards on-disk,
	// so we have to reverse the serial number byte order.
	temp = vi->serial_number[0];
	vi->serial_number[0] = vi->serial_number[3];
	vi->serial_number[3] = temp;
	
	temp = vi->serial_number[1];
	vi->serial_number[1] = vi->serial_number[2];
	vi->serial_number[2] = temp;
    
    return true;
}

int vfat_getinfo(cdico *d, char *devname)
{	
	struct fat32_volume_id vi;
	int fd;
	
	unsigned char  uuid_elongated[16];
	char uuid[512];
	
	msgprintf(MSG_DEBUG1, "vfat_getinfo(cdico=[???], devname=[%s])\n", devname);
	
	// ---- check it's a FAT32 file system
	if (vfat_test(devname) != true)
	{
		msgprintf(MSG_DEBUG1, "%s does not contain a FAT32 file system.\n", devname);
		return -1;
	}
	
	if ((fd=open64(devname, O_RDONLY|O_LARGEFILE))<0)
    {
        msgprintf(MSG_DEBUG1, "open64(%s) failed\n", devname);
        return -1;
    }
    
    memset(&vi, 0, sizeof(vi));
    if (read_fat32_volume_info(fd, &vi) != true)
    {
		close(fd);
        msgprintf(MSG_DEBUG1, "read fat32 volume info failed\n");
        return -1;
	}
	
	// ---- label
	msgprintf(MSG_DEBUG1, "vfat_label=[%s]\n", vi.volume_label);
    dico_add_string(d, 0, FSYSHEADKEY_FSLABEL, vi.volume_label);
	
	// ---- uuid
	// FAT32 doesn't have standard UUID's, they're
	// shorter. We artificially elongate the UUID's so
	// that we can use the standard library functions.
	memset(uuid, 0, sizeof(uuid));
	memset(uuid_elongated, 0, sizeof(uuid_elongated));
	
	memcpy(uuid_elongated, vi.serial_number, sizeof(vi.serial_number));
	
	uuid_unparse_lower((u8*)&uuid_elongated, uuid);
    msgprintf(MSG_DEBUG1, "vfat_uuid=[%s]\n", uuid);
    dico_add_string(d, 0, FSYSHEADKEY_FSUUID, uuid);
	
	// ---- block size
	msgprintf(MSG_DEBUG1, "vfat_fat_sectors_per_cluster=[%X]\n", vi.sectors_per_cluster);
	dico_add_u16(d, 0, FSYSHEADKEY_FSVFATSECTORSPERCLUSTER, vi.sectors_per_cluster);
	
	// ---- FAT size
	msgprintf(MSG_DEBUG1, "vfat_fat_size=[%d]\n", VFAT_FAT32_FAT_SIZE);
	dico_add_u32(d, 0, FSYSHEADKEY_FSVFATFATSIZE, VFAT_FAT32_FAT_SIZE);
	
	close(fd);
	return 0;
}

int vfat_test(char *devname)
{
	struct fat32_volume_id vi;
	int fd;
    
    msgprintf(MSG_DEBUG1, "vfat_test(devname=[%s])\n", devname);
    
    if ((fd=open64(devname, O_RDONLY|O_LARGEFILE))<0)
    {
        msgprintf(MSG_DEBUG1, "open64(%s) failed\n", devname);
        return false;
    }
    
    memset(&vi, 0, sizeof(vi));
    if (read_fat32_volume_info(fd, &vi) != true)
    {
		close(fd);
        msgprintf(MSG_DEBUG1, "read fat32 volume info failed\n");
        return false;
	}
    
    // ---- check it's a FAT32 file system
    if (vi.bytes_per_sector != VFAT_FAT32_BYTES_PER_SECTOR)
    {   
		close(fd);
        msgprintf(MSG_DEBUG1, "vi.bytes_per_sector != VFAT_FAT32_BYTES_PER_SECTOR\n");
        return false;
    }
    
    if (vi.number_of_fats != VFAT_FAT32_NUMBER_OF_FATS)
    {   
		close(fd);
        msgprintf(MSG_DEBUG1, "vi.number_of_fats != VFAT_FAT32_NUMBER_OF_FATS\n");
        return false;
    }
    
    if (vi.signature != VFAT_FAT32_SIGNATURE)
    {   
		close(fd);
        msgprintf(MSG_DEBUG1, "vi.signature != VFAT_FAT32_SIGNATURE\n");
        return false;
    }
    
    if (strcmp(vi.filesystem_type, VFAT_FAT32_FILESYSTEM_TYPE) != 0)
    {   
		close(fd);
        msgprintf(MSG_DEBUG1, "strcmp(vi.filesystem_type, VFAT_FAT32_FILESYSTEM_TYPE) != 0\n");
        msgprintf(MSG_DEBUG1, "strcmp(\"%s\", \"%s\") != 0\n", vi.filesystem_type, VFAT_FAT32_FILESYSTEM_TYPE);
        return false;
    }
    
    close(fd);
    return true;
}

int vfat_get_reqmntopt(char *partition, cstrlist *reqopt, cstrlist *badopt)
{
	msgprintf(MSG_DEBUG1, "vfat_get_reqmntopt(partition=[%s], reqopt=[???], badopt=[???])\n", partition);
	
	if (!reqopt || !badopt)
        return -1;

    return 0;
}
