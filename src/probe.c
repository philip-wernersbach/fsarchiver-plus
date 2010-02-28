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

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <stdlib.h>
#include <string.h>

#include "fsarchiver.h"
#include "probe.h"
#include "devinfo.h"
#include "common.h"
#include "error.h"

struct s_diskinfo partinfo[]=
{
    {false,    "[%-16s] ",    "[=====DEVICE=====] "},
    {false,    "[%-11.11s] ", "[==FILESYS==] "},
    {false,    "[%-17.17s] ", "[======LABEL======] "},
    {false,    "[%12s] ",     "[====SIZE====] "},
    {false,    "[%3s] ",      "[MAJ] "},
    {false,    "[%3s] ",      "[MIN] "},
    {true,     "[%-36s] ",    "[==============LONGNAME==============] "},
    {true,     "[%-38s] ",    "[=================UUID=================] "},
    {false,    "",            ""}
};

char *partlist_getinfo(char *bufdat, int bufsize, struct s_devinfo *blkdev, int item)
{
    memset(bufdat, 0, bufsize);
    switch (item)
    {
        case 0:    snprintf(bufdat, bufsize, "%s", blkdev->devname); break;
        case 1:    snprintf(bufdat, bufsize, "%s", blkdev->fsname); break;
        case 2:    snprintf(bufdat, bufsize, "%s", blkdev->label); break;
        case 3:    snprintf(bufdat, bufsize, "%s", blkdev->txtsize); break;
        case 4:    snprintf(bufdat, bufsize, "%d", blkdev->major); break;
        case 5:    snprintf(bufdat, bufsize, "%d", blkdev->minor); break;
        case 6:    snprintf(bufdat, bufsize, "%s", blkdev->longname); break;
        case 7:    snprintf(bufdat, bufsize, "%s", blkdev->uuid); break;
    };
    return bufdat;
}

int oper_probe(bool details)
{
    struct s_devinfo blkdev[FSA_MAX_BLKDEVICES];
    int diskcount;
    int partcount;
    char temp[1024];
    int res;
    int i, j;
    
    // ---- 0. get info from /proc/partitions + libblkid
    if ((res=get_partlist(blkdev, FSA_MAX_BLKDEVICES, &diskcount, &partcount))<1)
    {   msgprintf(MSG_FORCE, "Failed to detect disks and filesystems\n");
        return -1;
    }
    
    // ---- 1. show physical disks
    if (diskcount>0)
    {
        msgprintf(MSG_FORCE, "[======DISK======] [=============NAME==============] [====SIZE====] [MAJ] [MIN]\n");
        for (i=0; i < res; i++)
            if (blkdev[i].devtype==BLKDEV_PHYSDISK)
                msgprintf(MSG_FORCE, "[%-16s] [%-31s] [%12s] [%3d] [%3d]\n", blkdev[i].devname, 
                    blkdev[i].name, blkdev[i].txtsize, blkdev[i].major, blkdev[i].minor);
        msgprintf(MSG_FORCE, "\n");
    }
    else
    {
        msgprintf(MSG_FORCE, "No physical disk found\n");
    }
    
    // ---- 2. show filesystem information
    if (partcount>0)
    {
        // show title for filesystems
        for (j=0; partinfo[j].title[0]; j++)
        {
            if (details==true || partinfo[j].detailed==false)
                msgprintf(MSG_FORCE, "%s", partinfo[j].title);
        }
        msgprintf(MSG_FORCE, "\n");
        
        // show filesystems data
        for (i=0; i < res; i++)
        {
            if (blkdev[i].devtype==BLKDEV_FILESYSDEV)
            {
                for (j=0; partinfo[j].title[0]; j++)
                {
                    if (details==true || partinfo[j].detailed==false)
                        msgprintf(MSG_FORCE, partinfo[j].format, partlist_getinfo(temp, sizeof(temp), &blkdev[i], j));
                }
                msgprintf(MSG_FORCE, "\n");
            }
        }
    }
    else
    {
        msgprintf(MSG_FORCE, "No filesystem found\n");
    }
    
    return 0;
}
