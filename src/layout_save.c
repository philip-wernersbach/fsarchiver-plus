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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <libxml/encoding.h>
#include <libxml/xmlwriter.h>
#include <parted/parted.h>

#include "fsarchiver.h"
#include "strdico.h"
#include "devinfo.h"
#include "common.h"
#include "error.h"
#include "dico.h"

// ====================================================================================
typedef struct s_parttype
{   int num;
    char *name;
} cparttype;

cparttype parttype[]=
{
    {PED_PARTITION_NORMAL,      "normal"},
    {PED_PARTITION_LOGICAL,     "logical"},
    {PED_PARTITION_EXTENDED,    "extended"},
    {PED_PARTITION_FREESPACE,   "freespace"},
    {PED_PARTITION_METADATA,    "metadata"},
    {PED_PARTITION_PROTECTED,   "protected"},
    {-1,                         NULL}
};

char *parttype_int_to_string(int type)
{
    int i;
    for (i=0; parttype[i].name!=NULL; i++)
        if (parttype[i].num==type)
            return parttype[i].name;
    return NULL;    
}

int parttype_string_to_int(char *name)
{
    int i;
    for (i=0; parttype[i].name!=NULL; i++)
        if (strcmp(parttype[i].name, name)==0)
            return parttype[i].num;
    return -1;    
}

// ====================================================================================

typedef struct s_disklayoutwriter
{   xmlTextWriterPtr writer;
    xmlBufferPtr buffer;
} cdisklayoutwriter;

cdisklayoutwriter *disklayoutwriter_alloc()
{
    cdisklayoutwriter *dl;
    
    if ((dl=malloc(sizeof(cdisklayoutwriter)))==NULL)
    {   errprintf("malloc(%ld) failed\n", (long)sizeof(cdisklayoutwriter));
        return NULL;
    }
    
    memset(dl, 0, sizeof(cdisklayoutwriter));
    
    return dl;
}

int disklayoutwriter_destroy(cdisklayoutwriter *dl)
{
    assert(dl);
    free(dl);
    return 0;
}

int partutil_partition_get_flags(PedPartition *part, char *bufflags, int bufsize)
{
    PedPartitionFlag flag=0;
    int flagcount=0;
    const char *flagname;
    
    memset(bufflags, 0, bufsize);
    
    while ((flag=ped_partition_flag_next(flag))!=0)
    {
        flagname=ped_partition_flag_get_name(flag);
        if (ped_partition_get_flag(part, flag))
        {
            if (flagcount++ > 0)
                strlcatf(bufflags, bufsize, ",");
            strlcatf(bufflags, bufsize, "%s", flagname);
        }
    }
    
    return 0;
}

int disklayoutwriter_dump_partition(cdisklayoutwriter *dl, PedDisk *disk, PedPartition *part)
{
    char partflags[2048];
    char partname[512];
    char partfstype[512];
    const char *parttemp;
    char *parttype=NULL;
    
    assert(dl);
    assert(part);
        
    if ((parttype=parttype_int_to_string(part->type))==NULL)
    {   errprintf("disklayoutwriter_dump_device: invalid partition type: %d\n", part->type);
        return -1;
    }
    
    if (partutil_partition_get_flags(part, partflags, sizeof(partflags))!=0)
    {   errprintf("partutil_part_get_flags() failed\n");
        return -1;
    }
    
    memset(partname, 0, sizeof(partname));
    if ((ped_disk_type_check_feature(disk->type, PED_DISK_TYPE_PARTITION_NAME)==1) && 
        (parttemp=ped_partition_get_name(part))!=NULL)
        snprintf(partname, sizeof(partname), "%s", parttemp);
    
    memset(partfstype, 0, sizeof(partfstype));
    if ((part->fs_type!=NULL) && (part->fs_type->name!=NULL))
        snprintf(partfstype, sizeof(partfstype), "%s", part->fs_type->name);
    
    if (xmlTextWriterStartElement(dl->writer, BAD_CAST "partition")<0)
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterStartElement\n");
        return -1;
    }
    
    if ((xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "num", "%d", (int)part->num)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "type", "%s", parttype)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "start", "%lld", (long long)part->geom.start)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "length", "%lld", (long long)part->geom.length)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "end", "%lld", (long long)part->geom.end)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "flags", "%s", partflags)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "name", "%s", partname)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "fstype", "%s", partfstype)<0))
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterWriteFormatAttribute\n");
        return -1;
    }
    
    if (xmlTextWriterEndElement(dl->writer)<0)
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterEndElement\n");
        return -1;
    }
    
    return 0;
}

int disklayoutwriter_dump_device(cdisklayoutwriter *dl, char *devname, cdico *dico, int dicsection, int dickey)
{
    PedDevice *dev=NULL;
    PedDisk *disk=NULL;
    PedPartition *part=NULL;
    int ret=0;
    
    assert(dl);

    if ((dev=ped_device_get(devname))==NULL)
    {   errprintf("ped_device_get() failed\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if ((disk=ped_disk_new(dev))==NULL)
    {   errprintf("ped_disk_new() failed\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if ((dl->buffer = xmlBufferCreate())==NULL)
    {   errprintf("disklayoutwriter_dump_device: Error creating the xml buffer\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if ((dl->writer=xmlNewTextWriterMemory(dl->buffer, 0))==NULL)
    {   errprintf("disklayoutwriter_dump_device: Error creating the xml writer\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if (xmlTextWriterStartDocument(dl->writer, NULL, "ISO-8859-1", NULL)!=0)
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterStartDocument\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if (xmlTextWriterStartElement(dl->writer, BAD_CAST "disklayout")<0)
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterStartElement\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if (xmlTextWriterStartElement(dl->writer, BAD_CAST "disk")<0)
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterStartElement\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if ((xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "phys_sector_size", "%ld", (long)dev->phys_sector_size)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "sector_size", "%ld", (long)dev->sector_size)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "model", "%s", (long)dev->model)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "path", "%s", (long)dev->path)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "length", "%lld", (long long)dev->length)<0) ||
        (xmlTextWriterWriteFormatAttribute(dl->writer, BAD_CAST "disklabel", "%s", disk->type->name)<0))
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterWriteFormatAttribute\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    while ((part=ped_disk_next_partition(disk, part)) != NULL)
    {
        switch (part->type)
        {
                case PED_PARTITION_NORMAL:
                case PED_PARTITION_LOGICAL:
                case PED_PARTITION_EXTENDED:
                case PED_PARTITION_PROTECTED:
                    if (disklayoutwriter_dump_partition(dl, disk, part)!=0)
                    {
                        ret=-1;
                        goto disklayoutwriter_dump_device_cleanup;
                    }
                    break;
                default:
                    break;
        }
    }
    
    if (xmlTextWriterEndElement(dl->writer)<0)
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterEndElement\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if (xmlTextWriterEndElement(dl->writer)<0)
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterEndElement\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    if (xmlTextWriterEndDocument(dl->writer)<0)
    {   errprintf("disklayoutwriter_dump_device: Error at xmlTextWriterEndDocument\n");
        ret=-1;
        goto disklayoutwriter_dump_device_cleanup;
    }
    
    xmlFreeTextWriter(dl->writer);
    
    dico_add_string(dico, dicsection, dickey, (const char *)dl->buffer->content);
    
    xmlBufferFree(dl->buffer);
    
disklayoutwriter_dump_device_cleanup:
    if (disk)
        ped_disk_destroy(disk);
    if (dev)
        ped_device_destroy(dev);
    return ret;
}

int disklayoutwriter_dump_all_devices(cdisklayoutwriter *dl, cdico *dico, int dicsection)
{
    struct s_devinfo blkdev[FSA_MAX_BLKDEVICES];
    int diskcount;
    int partcount;
    int diskid=0;
    int res;
    int i;
    
    // ---- 0. get info from /proc/partitions + libblkid
    if ((res=get_partlist(blkdev, FSA_MAX_BLKDEVICES, &diskcount, &partcount))<1)
    {   msgprintf(MSG_FORCE, "Failed to detect disks and filesystems\n");
        return -1;
    }
    
    // ---- 1. show physical disks
    if (diskcount>0)
    {
        for (i=0; i < res; i++)
        {
            if (blkdev[i].devtype==BLKDEV_PHYSDISK)
            {
                msgprintf(MSG_VERB1, "Saving description of the partition table of disk %s\n", blkdev[i].longname);
                disklayoutwriter_dump_device(dl, blkdev[i].longname, dico, dicsection, diskid++);
            }
        }
    }
    else
    {
        msgprintf(MSG_VERB1, "No physical disk found, no partition table saved\n");
    }
    
    return diskid;
}

// =================================================================================

int savept(cdico *dico, int dicsection)
{
    cdisklayoutwriter *dl;
    int ptcount;
    
    dl=disklayoutwriter_alloc();
    ptcount=disklayoutwriter_dump_all_devices(dl, dico, dicsection);
    disklayoutwriter_destroy(dl);
    
    return ptcount;
}
