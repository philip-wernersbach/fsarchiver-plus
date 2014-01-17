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

#include <uuid.h>

#include "fsarchiver.h"
#include "dico.h"
#include "common.h"
#include "strlist.h"
#include "filesys.h"
#include "fs_swap.h"
#include "error.h"

int swap_mkfs(cdico *d, char *partition, char *fsoptions)
{ 
	char command[2048];
    char buffer[2048];
    char options[2048];
    int exitst;
    u64 temp;
    
	msgprintf(MSG_DEBUG1, "swap_mkfs(cdico=[???], partition=[%s], fsoptions=[%s])\n", partition, fsoptions);
    
    if (exec_command(command, sizeof(command), NULL, NULL, 0, NULL, 0, "mkswap -V")!=0)
    {   errprintf("mkswap not found. please install util-linux-ng on your system or check the PATH.\n");
        return -1;
    }
    
    memset(options, 0, sizeof(options));

    strlcatf(options, sizeof(options), " %s ", fsoptions);

	// ---- label
    if (dico_get_string(d, 0, FSYSHEADKEY_FSLABEL, buffer, sizeof(buffer))==0 && strlen(buffer)>0)
		strlcatf(options, sizeof(options), " -L '%s' ", buffer);
    
    // ---- uuid
    if (dico_get_string(d, 0, FSYSHEADKEY_FSUUID, buffer, sizeof(buffer))==0 && strlen(buffer)>0)
		strlcatf(options, sizeof(options), " -U '%s' ", buffer);
    
    // ---- block size
    if ((dico_get_u64(d, 0, FSYSHEADKEY_FSSWAPPAGESIZE, (u64 *)&temp) == 0) && (temp > 0))
		strlcatf(options, sizeof(options), " -p '%llu' ", (unsigned long long)temp);
    
    // ---- swap version
    if ((dico_get_u32(d, 0, FSYSHEADKEY_FSSWAPVERSION, (u32 *)&temp) == 0) && (temp > 0))
		strlcatf(options, sizeof(options), " -v '%u' ", (unsigned int)temp);
    
    if (exec_command(command, sizeof(command), &exitst, NULL, 0, NULL, 0, "mkswap %s %s", options, partition)!=0 || exitst!=0)
    {   errprintf("command [%s] failed\n", command);
        return -1;
    }
    
	return 0;
}

static int read_swap_info(int fd, struct swap_info *sh)
{	
	sh->magic[sizeof(sh->magic)-1] = '\0';
	
	// Find the swap page size.
	for (sh->page_size = SWAP_PAGE_SIZE_BASE; sh->page_size <= SWAP_PAGE_SIZE_MAX; sh->page_size *= SWAP_PAGE_SIZE_MULTIPLE)
	{	
		// Read and test against the swap magic.
		if (lseek64(fd, SWAP_MAGIC_OFFSET(sh->page_size), SEEK_SET) != SWAP_MAGIC_OFFSET(sh->page_size))
			return false;
		
		if (read(fd, &sh->magic, sizeof(sh->magic)-1)!=sizeof(sh->magic)-1)
			return false;
		
		if (strcmp(sh->magic, SWAP_VERSION1_MAGIC) == 0)
			break;
	}
	
	if (strcmp(sh->magic, SWAP_VERSION1_MAGIC) != 0)
		return false;
        
    // Read the UUID.
    if (lseek64(fd, SWAP_UUID_OFFSET, SEEK_SET) != SWAP_UUID_OFFSET)
        return false;
    
    if (read(fd, &sh->uuid, sizeof(sh->uuid))!=sizeof(sh->uuid))
        return false;
        
    // Read the label.
    if (lseek64(fd, SWAP_LABEL_OFFSET, SEEK_SET) != SWAP_LABEL_OFFSET)
        return false;
    
    if (read(fd, &sh->label, sizeof(sh->label))!=sizeof(sh->label))
        return false;
    
    sh->label[15] = '\0';
    
    return true;
}

int swap_getinfo(cdico *d, char *devname)
{	
	struct swap_info sh;
	int fd;
	
	char uuid[512];
	
	msgprintf(MSG_DEBUG1, "swap_getinfo(cdico=[???], devname=[%s])\n", devname);
	
	// ---- check it's a swap file system
	if (swap_test(devname) != true)
	{
		msgprintf(MSG_DEBUG1, "%s does not contain a swap version 1 file system.\n", devname);
		return -1;
	}
	
	if ((fd=open64(devname, O_RDONLY|O_LARGEFILE))<0)
    {
        msgprintf(MSG_DEBUG1, "open64(%s) failed\n", devname);
        return -1;
    }
    
    memset(&sh, 0, sizeof(sh));
    if (read_swap_info(fd, &sh) != true)
    {
		close(fd);
        msgprintf(MSG_DEBUG1, "read swap info failed\n");
        return -1;
	}
	
	// ---- label
	msgprintf(MSG_DEBUG1, "swap_label=[%s]\n", sh.label);
    dico_add_string(d, 0, FSYSHEADKEY_FSLABEL, sh.label);
	
	// ---- uuid
	memset(uuid, 0, sizeof(uuid));
	uuid_unparse_lower((u8*)&sh.uuid, uuid);
    
    msgprintf(MSG_DEBUG1, "swap_uuid=[%s]\n", uuid);
    dico_add_string(d, 0, FSYSHEADKEY_FSUUID, uuid);
	
	// ---- block size
	msgprintf(MSG_DEBUG1, "swap_page_size=[%llu]\n", (unsigned long long)sh.page_size);
	dico_add_u64(d, 0, FSYSHEADKEY_FSSWAPPAGESIZE, sh.page_size);
	
	// ---- swap version
	msgprintf(MSG_DEBUG1, "swap_version=[%u]\n", SWAP_VERSION1_VERSION_NUMBER);
	dico_add_u32(d, 0, FSYSHEADKEY_FSSWAPVERSION, SWAP_VERSION1_VERSION_NUMBER);
	
	close(fd);
	return 0;
}

int swap_test(char *devname)
{
	struct swap_info sh;
	int fd;
    
    msgprintf(MSG_DEBUG1, "swap_test(devname=[%s])\n", devname);
    
    if ((fd=open64(devname, O_RDONLY|O_LARGEFILE))<0)
    {
        msgprintf(MSG_DEBUG1, "open64(%s) failed\n", devname);
        return false;
    }
    
    memset(&sh, 0, sizeof(sh));
    if (read_swap_info(fd, &sh) != true)
    {
		close(fd);
        msgprintf(MSG_DEBUG1, "read swap header failed\n");
        return false;
	}
    
    close(fd);
    return true;
}

int swap_get_reqmntopt(char *partition, cstrlist *reqopt, cstrlist *badopt)
{
	msgprintf(MSG_DEBUG1, "swap_get_reqmntopt(partition=[%s], reqopt=[???], badopt=[???])\n", partition);
	
	if (!reqopt || !badopt)
        return -1;

    return 0;
}
