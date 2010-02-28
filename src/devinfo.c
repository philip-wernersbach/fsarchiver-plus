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
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <ctype.h>
#include <fcntl.h>
#include <dirent.h>
#include <blkid.h>

#include "fsarchiver.h"
#include "devinfo.h"
#include "common.h"
#include "fs_ext2.h"
#include "error.h"

int get_partlist(struct s_devinfo *blkdev, int maxblkdev, int *diskcount, int *partcount)
{
    struct s_devinfo blkdev1[FSA_MAX_BLKDEVICES];
    struct s_devinfo tmpdev;
    int best; // index of the best item found in old array
    int pos=0; // pos of latest item in new array
    char devname[1024];
    char longname[1024];
    char delims[]=" \t\n";
    char line[1024];
    char *saveptr;
    char *result;
    char major[256];
    char minor[256];
    FILE *fpart;
    int count=0;
    int i, j;
    
    // init
    *diskcount=0;
    *partcount=0;
    
    // browse list in "/proc/partitions"
    if ((fpart=fopen("/proc/partitions","rb"))==NULL)
        return -1;
    while(!feof(fpart) && (count < FSA_MAX_BLKDEVICES) && (count < maxblkdev))
    {
        if (stream_readline(fpart, line, sizeof(line))>1)
        {
            minor[0]=major[0]=0;
            devname[0]=0;
            result=strtok_r(line, delims, &saveptr);
            
            for(i=0; result != NULL && i<=4; i++)
            {
                switch(i)
                {
                    case 0: // col0 = major
                        snprintf(major, sizeof(major), "%s", result);
                        break;
                    case 1: // col1 = minor
                        snprintf(minor, sizeof(minor), "%s", result);
                        break;
                    case 3: // col3 = devname
                        snprintf(devname, sizeof(devname), "%s", result);
                        break;
                }
                result = strtok_r(NULL, delims, &saveptr);
            }
            
            // ignore invalid entries
            if ((strlen(devname)==0) || (atoi(major)==0 && atoi(minor)==0))
                continue;
            snprintf(longname, sizeof(longname), "/dev/%s", devname);
            if (get_devinfo(&tmpdev, longname, atoi(minor), atoi(major))!=0)
               continue; // to to the next part
            
            // check that this device is not already in the list
            for (i=0; i < count; i++)
                if (blkdev1[i].rdev==tmpdev.rdev)
                    continue; // to to the next part
            
            // add the device to list if it is a real device and it's not already in the list
            blkdev1[count++]=tmpdev;
        }
    }
    
    fclose(fpart);
    
    // ---- 2. sort the devices
    for (pos=0, i=0; i<count; i++)
    {
        // set best to the first available item in the old array
        for (j=0, best=-1; (j<count) && (best==-1); j++)
            if (blkdev1[j].rdev!=0)
                best=j;
        // find the index of the best item in the old array
        for (j=0; j<count; j++)
            if ((blkdev1[j].rdev > 0) && (blkdev1[j].rdev < blkdev1[best].rdev))
                best=j;
        // update counters
        switch (blkdev1[best].devtype)
        {
            case BLKDEV_FILESYSDEV:
                (*partcount)++;
                break;
            case BLKDEV_PHYSDISK:
                (*diskcount)++;
                break;                
        }
        // move item to the new array
        blkdev[pos++]=blkdev1[best];
        blkdev1[best].rdev=0;
    }

    return count;
}

int get_devinfo(struct s_devinfo *outdev, char *indevname, int min, int maj)
{
    char sysblkdevname[512];
    blkid_tag_iterate iter;
    char sysblkinfo[PATH_MAX];
    const char *type, *value;
    struct stat64 statbuf;
    struct dirent *dir;
    char temp[PATH_MAX];
    blkid_dev dev;
    DIR *dirdesc;
    FILE *finfo;
    int found;
    int fd;
    int i;
    
    // init
    memset(outdev, 0, sizeof(struct s_devinfo));
    
    // defaults values
    outdev->devtype=BLKDEV_INVALID;
    snprintf(outdev->label, sizeof(outdev->label), " ");
    snprintf(outdev->uuid, sizeof(outdev->uuid), "<unknown>");
    snprintf(outdev->fsname, sizeof(outdev->fsname), "<unknown>");
    
    // check the name starts with "/dev/"
    if ((strlen(indevname) < 5) || (memcmp(indevname, "/dev/", 5)!=0))
        return -1;
    
    // get short name ("/dev/sda1" -> "sda1")
    snprintf(outdev->devname, sizeof(outdev->devname), "%s", indevname+5); // skip "/dev/"
    
    // get long name if there is one (eg: LVM / devmapper)
    snprintf(outdev->longname, sizeof(outdev->longname), "%s", indevname);
    if ((dirdesc=opendir("/dev/mapper"))!=NULL)
    {
        found=false;
        while (((dir=readdir(dirdesc)) != NULL) && found==false)
        {
            snprintf(temp, sizeof(temp), "/dev/mapper/%s", dir->d_name);
            if ((stat64(temp, &statbuf)==0) && S_ISBLK(statbuf.st_mode) && 
                (major(statbuf.st_rdev)==maj) && (minor(statbuf.st_rdev)==min))
            {
                snprintf(outdev->longname, sizeof(outdev->longname), "%s", temp);
                found=true;
            }
        }
        closedir(dirdesc);
    }
    
    // get device basic info (size, major, minor)
    if (((fd=open64(outdev->longname, O_RDONLY|O_LARGEFILE))<0) ||
        ((outdev->devsize=lseek64(fd, 0, SEEK_END))<0) ||
        (fstat64(fd, &statbuf)!=0) ||
        (!S_ISBLK(statbuf.st_mode)) ||
        (close(fd)<0))
        return -1;
    outdev->rdev=statbuf.st_rdev;
    outdev->major=major(statbuf.st_rdev);
    outdev->minor=minor(statbuf.st_rdev);
    format_size(outdev->devsize, outdev->txtsize, sizeof(outdev->txtsize), 'h');
    if (outdev->devsize==1024) // ignore extended partitions
        return -1;
    
    // devname shown in /sys/block (eg for HP-cciss: "cciss/c0d0" -> "cciss!c0d0")
    snprintf(sysblkdevname, sizeof(sysblkdevname), "%s", outdev->devname);
    for (i=0; (sysblkdevname[i]!=0) && (i<sizeof(sysblkdevname)); i++)
        if (sysblkdevname[i]=='/')
            sysblkdevname[i]='!';
    
    // check if it's a physical disk (there is a "/sys/block/${devname}/device")
    snprintf(sysblkinfo, sizeof(sysblkinfo), "/sys/block/%s/device", sysblkdevname);
    if (stat64(sysblkinfo, &statbuf)==0)
    {
        outdev->devtype=BLKDEV_PHYSDISK;
        snprintf(sysblkinfo, sizeof(sysblkinfo), "/sys/block/%s/device/model", sysblkdevname);
        if ( ((finfo=fopen(sysblkinfo, "rb")) != NULL) && (fread(temp, 1, sizeof(temp), finfo)>0) && fclose(finfo)==0 )
            for (i=0; (temp[i]!=0) && (temp[i]!='\r') && (temp[i]!='\n'); i++)
                outdev->name[i]=temp[i];
    }
    else
    {
        outdev->devtype=BLKDEV_FILESYSDEV;
    }
    
    // get blkid infos about the device (label, uuid)
    blkid_cache cache = NULL;
    if (blkid_get_cache(&cache, NULL) < 0)
        return -1;
    if ((dev=blkid_get_dev(cache, outdev->longname, BLKID_DEV_NORMAL))!=NULL)
    {
        iter = blkid_tag_iterate_begin(dev);
        while (blkid_tag_next(iter, &type, &value)==0)
        {
            if (strcmp(type, "LABEL")==0)
                snprintf(outdev->label, sizeof(outdev->label), "%s", value);
            else if (strcmp(type, "UUID")==0)
                snprintf(outdev->uuid, sizeof(outdev->uuid), "%s", value);
            else if (strcmp(type, "TYPE")==0)
                snprintf(outdev->fsname, sizeof(outdev->fsname), "%s", value);
        }
        blkid_tag_iterate_end(iter);
        
        // workaround: blkid < 1.41 don't know ext4 and say it is ext3 instead
        if (strcmp(outdev->fsname, "ext3")==0)
        {
            if (ext3_test(outdev->longname)==true)
                snprintf(outdev->fsname, sizeof(outdev->fsname), "ext3");
            else // cannot run ext4_test(): it would fail on an ext4 when e2fsprogs < 1.41
                snprintf(outdev->fsname, sizeof(outdev->fsname), "ext4");
        }
    }
    blkid_put_cache(cache); // free memory allocated by blkid_get_cache
    
    return 0;
}
