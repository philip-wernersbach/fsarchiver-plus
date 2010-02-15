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

/*
 * - Test support for sectors_size != 512 with a recent libparted
 * - More cheks (max-primary-partitions, label-supports-logical)
 * - Move all the partutil functions to layout_util.c
 * - 
 */

#ifdef HAVE_CONFIG_H
#  include "config.h"
#endif

#include <parted/parted.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>

#include "fsarchiver.h"
#include "layout_read.h"
#include "syncthread.h"
#include "strdico.h"
#include "dico.h"
#include "common.h"
#include "error.h"

enum {PARTSTATUS_NULL=0, PARTSTATUS_CREATED, PARTSTATUS_RELOCATED};

#define MAX_PARTS   256

// =================================================================================

int partutil_partition_set_flags(PedPartition *part, char *flags)
{
    PedPartitionFlag flag=0;
    const char *flagname;
    char bufbak[2048];
    char *saveptr;
    char *result;
    char delims[]=",;\t\n";
    
    // init
    snprintf(bufbak, sizeof(bufbak), "%s", flags);
    
    result=strtok_r(bufbak, delims, &saveptr);
    while (result!=NULL)
    {
        while ((flag=ped_partition_flag_next(flag))!=0)
        {
            flagname=ped_partition_flag_get_name(flag);
            if (strcmp(flagname, result)==0 && ped_partition_is_flag_available(part, flag)==1)
                ped_partition_set_flag(part, flag, true);
        }
        
        result=strtok_r(NULL, delims, &saveptr);
    }
    
    return 0;
}

int partutil_partition_set_fstype(PedPartition *part, char *fstypename)
{
    const PedFileSystemType *fstype;
    
    if ((fstype=ped_file_system_type_get(fstypename))!=NULL)
    {
        ped_partition_set_system(part, fstype);	
    }
    return 0;
}

int partutil_is_primary(cpartspecs *spec)
{
    return ((strcmp(spec->type, "normal")==0) || (strcmp(spec->type, "extended")==0));
}

int partutil_is_extended(cpartspecs *spec)
{
    return (strcmp(spec->type, "extended")==0);
}

int partutil_is_logical(cpartspecs *spec)
{
    return (strcmp(spec->type, "logical")==0);
}

int partutil_set_status(cstrdico *dico, int partnum, s64 status)
{
    char keyname[512];
    char value[512];
    snprintf(keyname, sizeof(keyname), "part%d.status", partnum);
    snprintf(value, sizeof(value), "%ld", (long)status);
    return strdico_set_value(dico, keyname, value);
}

int partutil_get_status(cstrdico *dico, int partnum, s64 *status)
{
    char keyname[512];    
    *status=PARTSTATUS_NULL;
    snprintf(keyname, sizeof(keyname), "part%d.status", partnum);
    return strdico_get_s64(dico, status, keyname);
}

int partutil_show_disk_specs(char *devname)
{
    PedDevice *dev=NULL;
    PedDisk *disk=NULL;
    char fmtsize[512];
    int ret=0;
    
    if ((dev=ped_device_get(devname))==NULL)
    {   errprintf("ped_device_get() failed\n");
        ret=-1;
        goto partutil_show_disk_specs_cleanup;
    }
    
    if ((disk=ped_disk_new(dev))==NULL)
    {   errprintf("ped_disk_new() failed\n");
        ret=-1;
        goto partutil_show_disk_specs_cleanup;
    }
    
    format_size(((u64)dev->length)*((u64)dev->sector_size), fmtsize, sizeof(fmtsize), 'h');
    msgprintf(MSG_FORCE, "\nDestination disk: [%s] [%s] [%s] (disklabel: %s)\n\n",
        devname, fmtsize, dev->model, disk->type->name); 

partutil_show_disk_specs_cleanup:
    if (disk)
        ped_disk_destroy(disk);
    if (dev)
        ped_device_destroy(dev);
    
    return ret;
}

s64 partutil_convert_sector(s64 oldsector, u32 oldsectsize_bytes, u32 newsectsize_bytes, bool roundway)
{
    s64 roundedsector=0;
    s64 newsector=0;
    u32 oldsectsize;
    u32 newsectsize;
    
    oldsectsize=oldsectsize_bytes/512;
    newsectsize=newsectsize_bytes/512;
    
    if (newsectsize==oldsectsize)
    {
        newsector=oldsector;
    }
    else if (newsectsize < oldsectsize)
    {
        newsector=(oldsector*((u64)newsectsize))/oldsectsize;
    }
    else // if (newsectsize > oldsectsize)
    {
        if (roundway==1) // round up
        {
            roundedsector=oldsector+(newsectsize-(oldsector%newsectsize));
            newsector=roundedsector*oldsectsize/newsectsize;
        }
        else
        {
            roundedsector=oldsector-(oldsector%newsectsize);
            newsector=roundedsector*oldsectsize/newsectsize;
        }
    }
    
    return newsector;
}

// =================================================================================

typedef struct s_disklayoutbuilder
{   char devname[PATH_MAX]; // device name of the disk (eg: "/dev/sda")
    cstrdico *dico; // dico that contains all the disk/part specs from the archive
    cdiskspecs disk_specs; // specs of the current disk drive
    PedDevice *dev; // libparted Device structure
    PedDisk *disk; // libparted Disk structure
    int maxprimary; // how many primary partitions the disklabel supports
    int primpart_highestid; // highest number of a primary partition
    int primpart_count; // how many primary partitions we really have
    int logicpart_highestid; // highest number of a logical partition
    int logicpart_count; // how many logical partitions we really have
    u64 geom_max_end; // end byte of the latest partition on the disk
} cdisklayoutbuilder;

// =================================================================================

cdisklayoutbuilder *disklayoutbuilder_alloc()
{
    cdisklayoutbuilder *dl;
    
    if ((dl=malloc(sizeof(cdisklayoutbuilder)))==NULL)
    {   errprintf("malloc(%ld) failed\n", (long)sizeof(cdisklayoutbuilder));
        return NULL;
    }
    
    memset(dl, 0, sizeof(cdisklayoutbuilder));
    
    return dl;
}

int disklayoutbuilder_destroy(cdisklayoutbuilder *dl)
{
    if (dl->disk)
        ped_disk_destroy(dl->disk);
    if (dl->dev)
        ped_device_destroy(dl->dev);
    memset(dl, 0, sizeof(cdisklayoutbuilder));
    free(dl);
    return 0;
}

int disklayoutbuilder_initialize(cdisklayoutbuilder *dl, cstrdico *dico, char *devname)
{
    dl->dico=dico;
    snprintf(dl->devname, sizeof(dl->devname), "%s", devname);
    return 0;
}

int disklayoutbuilder_create_disk(cdisklayoutbuilder *dl)
{
    PedDiskType *type;
    
    // get disk specs from dico
    if (convert_dico_to_device(dl->dico, &dl->disk_specs)!=0)
    {   errprintf("convert_dico_to_device() failed\n");
        return -1;
    }
    
    // create new device structure in memory
    if ((dl->dev=ped_device_get(dl->devname))==NULL)
    {   errprintf("ped_device_get(%s) failed", dl->devname);
        return -1;
    }
    
    // create new disk structure in memory
    if ((type=ped_disk_type_get(dl->disk_specs.disklabel))==NULL)
    {   errprintf("ped_disk_type_get(%s) failed: cannot get disk type\n", dl->disk_specs.disklabel);
        return -1;
    }
    
    if ((dl->disk=ped_disk_new_fresh(dl->dev, type))==NULL)
    {   errprintf("ped_disk_new_fresh() failed\n");
        return -1;
    }
    
    // disable cylinder alignments to respect the original disk layout
#ifndef OLD_PARTED
    if (ped_disk_set_flag(dl->disk, PED_DISK_CYLINDER_ALIGNMENT, false)!=1)
    {   errprintf("ped_disk_set_flag(disk, PED_DISK_CYLINDER_ALIGNMENT, false) failed\n");
        return -1;
    }
#endif // OLD_PARTED
    
    if ((dl->maxprimary=ped_disk_get_max_primary_partition_count(dl->disk)) < 1)
    {   errprintf("ped_disk_get_max_primary_partition_count() failed\n");
        return -1;
    }
    
    return 0;
}

int disklayoutbuilder_analyse_partitions(cdisklayoutbuilder *dl)
{
    cpartspecs pspecs;
    u64 curendbyte;
    int i;
    
    dl->primpart_highestid=0; // SELECT max(id) FROM partition WHERE type='primary'
    dl->primpart_count=0;
    dl->logicpart_highestid=0; // SELECT max(id) FROM partition WHERE type='logical'
    dl->logicpart_count=0;
    dl->geom_max_end=0;
    
    for (i=1; i <= MAX_PARTS; i++)
    {
        if (convert_dico_to_partition(dl->dico, &pspecs, i)==0)
        {
            // analyse geometry
            curendbyte=((u64)pspecs.end)*((u64)dl->disk_specs.sectsize);
            dl->geom_max_end=max(dl->geom_max_end, curendbyte);
            
            // analyse primary partitions
            if (partutil_is_primary(&pspecs))
            {
                dl->primpart_count++;
                if (pspecs.num > dl->primpart_highestid)
                    dl->primpart_highestid=pspecs.num;
            }
            
            // analyse logical partitions
            if (partutil_is_logical(&pspecs))
            {
                dl->logicpart_count++;
                if (pspecs.num > dl->logicpart_highestid)
                    dl->logicpart_highestid=pspecs.num;
            }
        }
    }
    
    return 0;
}

int disklayoutbuilder_check_conditions(cdisklayoutbuilder *dl)
{
    char buffer1[512];
    char buffer2[512];
    u64 newdisksize;
    
    // check that parted >= 2.1 if sectors != 512
#ifdef OLD_PARTED
    if (dl->dev->sector_size!=512)
    {   errprintf("A newer version of libparted is required to work on devices with sectors bigger than 512 bytes\n");
        return -1;
    }
#endif // OLD_PARTED
    
    // check that the disklabel is supported
    if ((strcmp(dl->disk_specs.disklabel, "msdos")!=0) && (strcmp(dl->disk_specs.disklabel, "gpt")!=0))
    {   errprintf("Only \"msdos\" and \"gpt\" disklabels are currently supported, \"%s\" is not\n", dl->disk_specs.disklabel);
        return -1;
    }
    
    // check that the destination disk is big enough
    newdisksize=((u64)dl->dev->sector_size)*((u64)dl->dev->length);
    format_size(dl->geom_max_end, buffer1, sizeof(buffer1), 'h');
    format_size(newdisksize, buffer2, sizeof(buffer2), 'h');
    if (dl->geom_max_end > newdisksize)
    {   errprintf("Destination disk is too small: size_required: %s, disk_size: %s\n", buffer1, buffer2);
        return -1;
    }
    
    return 0;
}

int disklayoutbuilder_recreate_primary_partitions(cdisklayoutbuilder *dl)
{
    const PedFileSystemType *fs_type=NULL;
    PedConstraint *constraint=NULL;
    PedPartitionType part_type=0;
    PedPartition *part=NULL;
    PedSector start, end;
    s64 maxpartstart_pos=-1;
    int maxpartstart_id=-1;
    int fakepartsize=512; 
    cpartspecs pspecs;
    s64 temp64;
    int i, j;
    
    // ---- create fake partitions from num=1 to num=primpart_highestid at the beginning of the disk
    for (i=1; i<=dl->primpart_highestid; i++)
    {
        part_type=0;
        if ((convert_dico_to_partition(dl->dico, &pspecs, i)==0) && partutil_is_extended(&pspecs))
            part_type=PED_PARTITION_EXTENDED;
        
        start=(i*fakepartsize);
        end=start+fakepartsize-1;
        if ((part=ped_partition_new(dl->disk, part_type, fs_type, start, end))==NULL)
        {   errprintf("ped_partition_new() failed: cannot create partition %d\n", i);
            return -1;
        }
        constraint=ped_constraint_exact(&part->geom);
        if (ped_disk_add_partition(dl->disk, part, constraint)!=1)
        {   errprintf("ped_disk_add_partition() failed: cannot add partition %d\n", i);
            return -1;
        }
        msgprintf(MSG_VERB2, "Created fake partition num=%d with type=%d\n", i, (int)part_type);
    }
    
    // ---- destroy all partitions which do not exist on the original disk
    for (i=1; i<=dl->primpart_highestid; i++)
    {
        if (convert_dico_to_partition(dl->dico, &pspecs, i)!=0)
        {
            if ((part=ped_disk_get_partition(dl->disk, i))==NULL)
            {   errprintf("ped_disk_get_partition(num=%d) failed\n", i);
                return -1;
            }
            
            if (ped_disk_delete_partition(dl->disk, part)!=1)
            {   errprintf("ped_disk_delete_partition() failed for part num=%d\n", i);
                return -1;
            }
            
            msgprintf(MSG_VERB2, "Deleted partition num=%d\n", i);
        }
    }
    
    // ---- relocate all primary partitions to the right place from the end of the disk to the beginning
    for (i=0; i<dl->primpart_count; i++)
    {
        // SELECT max(start_sector) FROM partition WHERE type='primary' AND status!='relocated'
        maxpartstart_pos=-1;
        maxpartstart_id=-1;
        for (j=1; j <= dl->maxprimary; j++)
        {
            if ((convert_dico_to_partition(dl->dico, &pspecs, j)==0) 
                && (partutil_is_primary(&pspecs)) && (pspecs.start > maxpartstart_pos))
            {
                if (partutil_get_status(dl->dico, j, &temp64)!=0 || temp64!=PARTSTATUS_RELOCATED)
                {
                    maxpartstart_pos=pspecs.start;
                    maxpartstart_id=j;
                }
            }
        }
        
        if (maxpartstart_id<1)
        {   errprintf("invalid value: maxpartstart_id=%ld\n", (long)maxpartstart_id);
            return -1;
        }
        
        if (convert_dico_to_partition(dl->dico, &pspecs, maxpartstart_id)!=0)
        {   errprintf("convert_dico_to_partition() failed\n");
            return -1;
        }
        msgprintf(MSG_VERB2, "primary partition with the biggest start has partid=%d\n", maxpartstart_id);
        msgprintf(MSG_VERB2, "moving part id=%d: start=%lld and end=%lld\n", maxpartstart_id, (long long)pspecs.start, (long long)pspecs.end);
        
        if ((part=ped_disk_get_partition(dl->disk, maxpartstart_id))==NULL)
        {   errprintf("ped_disk_get_partition(num=%d) failed\n", maxpartstart_id);
            return -1;
        }
        
        constraint=ped_constraint_any(dl->dev);
        start=partutil_convert_sector(pspecs.start, dl->disk_specs.sectsize, dl->dev->sector_size, 1);
        end=partutil_convert_sector(pspecs.end, dl->disk_specs.sectsize, dl->dev->sector_size, 0);
        msgprintf(MSG_VERB2, "start=partutil_convert_sector(oldsectnr=%ld, oldsectsize=%ld, newsectsize=%ld)=%ld\n", 
            (long)pspecs.start, (long)dl->disk_specs.sectsize, (long)dl->dev->sector_size, (long)start);
        msgprintf(MSG_VERB2, "end=partutil_convert_sector(oldsectnr=%ld, oldsectsize=%ld, newsectsize=%ld)=%ld\n", 
            (long)pspecs.end, (long)dl->disk_specs.sectsize, (long)dl->dev->sector_size, (long)end);
        if (ped_disk_set_partition_geom(dl->disk, part, constraint, start, end)!=1)
        {   errprintf("ped_disk_set_partition_geom() failed for part num=%d\n", maxpartstart_id);
            return -1;
        }
        partutil_set_status(dl->dico, maxpartstart_id, PARTSTATUS_RELOCATED);
    }
    
    return 0;
}

int disklayoutbuilder_recreate_logical_partitions(cdisklayoutbuilder *dl)
{
    PedFileSystemType *fs_type=NULL;
    PedPartitionType part_type=0;
    PedPartition *extpart=NULL;
    PedPartition *part=NULL;
    PedSector start, end;
    cpartspecs pspecs;
    int i;
    
    if ((extpart=ped_disk_extended_partition(dl->disk))==NULL)
    {   errprintf("Cannot find extended partition on the new disk\n");
        return -1;
    }
    
    for (i=5; i<=dl->logicpart_highestid; i++)
    {
        if ((convert_dico_to_partition(dl->dico, &pspecs, i)==0) && partutil_is_logical(&pspecs))
        {
            part_type=PED_PARTITION_LOGICAL;
            fs_type=NULL;
            start=pspecs.start;
            end=pspecs.end;
            
            if ((part=ped_partition_new(dl->disk, part_type, fs_type, start, end))==NULL)
            {   errprintf("ped_partition_new() failed: cannot create partition %d\n", i);
                return -1;
            }
            
            if (ped_disk_add_partition(dl->disk, part, ped_constraint_exact(&part->geom))!=1)
            {   errprintf("ped_disk_add_partition() failed: cannot add partition %d\n", i);
                return -1;
            }
            msgprintf(MSG_VERB2, "Created logical partition num=%d with type=%d\n", i, (int)part_type);
        }
    }
    
    return 0;
}

int disklayoutbuilder_set_partitions_attributes(cdisklayoutbuilder *dl)
{
    PedPartition *part;
    cpartspecs pspecs;
    int i;
    
    for (i=1; i<=MAX_PARTS; i++)
    {
        
        if ((convert_dico_to_partition(dl->dico, &pspecs, i)==0))
        {
            if ((part=ped_disk_get_partition(dl->disk, i))==NULL)
            {   errprintf("ped_disk_get_partition(num=%d) failed\n", i);
                return -1;
            }
            if (partutil_partition_set_flags(part, pspecs.flags)!=0)
            {   errprintf("partutil_partition_set_flags() failed\n");
                return -1;
            }
            if (partutil_partition_set_fstype(part, pspecs.fstype)!=0)
            {   errprintf("partutil_partition_set_fstype() failed\n");
                return -1;
            }
            if ((ped_disk_type_check_feature(dl->disk->type, PED_DISK_TYPE_PARTITION_NAME)==1) &&
                (ped_partition_set_name(part, pspecs.name)!=1))
            {   errprintf("ped_partition_set_name() failed\n");
                return -1;
            }
        }
    }
    
    return 0; 
}

int disklayoutbuilder_commit_changes(cdisklayoutbuilder *dl)
{
    // DEVELOPMENT: hard-coded protection to be sure we work on the disk dedicated to tests
    /*char *MY_DISK_DEDICATED_TO_TESTS="ATA SAMSUNG HD103UJ";
    if (strcmp(dl->dev->model, MY_DISK_DEDICATED_TO_TESTS)!=0)
    {   errprintf("Attempt to work on the wrong disk: expected=[%s], disk=[%s]\n", MY_DISK_DEDICATED_TO_TESTS, dl->dev->model);
        return -1;
    }*/
    msgprintf(MSG_VERB2, "disklayoutbuilder_rest_device(%s): model=[%s]\n", dl->devname, dl->dev->model);
    
    if (ped_disk_commit_to_dev(dl->disk)!=1)
    {   errprintf("ped_disk_commit_to_dev(%s) failed: cannot commit changes to disk\n", dl->devname);
        return -1;
    }
    
    if (ped_disk_commit_to_os(dl->disk)!=1)
    {   errprintf("ped_disk_commit_to_os(%s) failed: cannot commit changes to os\n", dl->devname);
        return -1;
    }
    
    return 0;
}

// =================================================================================

int restpt_show_device(cstrdico *dico, int id)
{
    cdiskspecs disk_specs;
    cpartspecs pspecs;
    int partcnt=0;
    int i;
    
    if (convert_dico_to_device(dico, &disk_specs)!=0)
    {   errprintf("convert_dico_to_device() failed\n");
        return -1;
    }
    
    msgprintf(MSG_FORCE, "\nid=%d: Original disk: [%s] [%s] [%s] (disklabel: %s)\n",
        id, disk_specs.path, disk_specs.size, disk_specs.model, disk_specs.disklabel);
    
    for (i=1; partcnt <= disk_specs.partcount && i<=MAX_PARTS; i++)
    {
        if (convert_dico_to_partition(dico, &pspecs, i)==0)
        {
            msgprintf(MSG_FORCE, "   -partition=%.2ld start=%.11ld len=%.11ld end=%.11ld (%s)\n", (long)pspecs.num, 
                (long)pspecs.start, (long)pspecs.length, (long)pspecs.end, pspecs.type);
            partcnt++;
        }
    }
    
    return 0;
}

// =================================================================================

int restpt(char *partdesc, int id, struct s_strdico *dicocmdline)
{
    cdisklayoutbuilder *dl=NULL;
    cstrdico *dicodiskinfo=NULL;
    char destdisk[1024];
    int delay=5;
    int ret=0;
    
    if (strdico_get_string(dicocmdline, destdisk, sizeof(destdisk), "dest")!=0)
    {   errprintf("strdico_get_string(dicocmdline, 'dest') failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    if ((dicodiskinfo=strdico_alloc())==NULL)
    {   errprintf("strdico_alloc() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    if (parttable_read_xml_dump(partdesc, dicodiskinfo)!=0)
    {   errprintf("parttable_read_xml_dump() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    restpt_show_device(dicodiskinfo, id);
    partutil_show_disk_specs(destdisk);
    
    if ((dl=disklayoutbuilder_alloc())==NULL)
    {   errprintf("disklayoutbuilder_alloc() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    if (disklayoutbuilder_initialize(dl, dicodiskinfo, destdisk)!=0)
    {   msgprintf(MSG_STACK, "disklayoutbuilder_initialize() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    if (disklayoutbuilder_create_disk(dl)!=0)
    {   msgprintf(MSG_STACK, "disklayoutbuilder_create_disk() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    msgprintf(MSG_DEBUG2, "ped_disk_get_max_primary_partition_count(%s)=%d\n", dl->devname, dl->maxprimary);
    
    if (disklayoutbuilder_analyse_partitions(dl)!=0)
    {   msgprintf(MSG_STACK, "disklayoutbuilder_analyse_partitions() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    if (disklayoutbuilder_check_conditions(dl)!=0)
    {   msgprintf(MSG_STACK, "disklayoutbuilder_check_conditions() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    msgprintf(MSG_DEBUG2, "primpart_count=%d\n", dl->primpart_count);
    msgprintf(MSG_DEBUG2, "primpart_highestid=%d\n", dl->primpart_highestid);
    msgprintf(MSG_DEBUG2, "logicpart_count=%d\n", dl->logicpart_count);
    msgprintf(MSG_DEBUG2, "logicpart_highestid=%d\n", dl->logicpart_highestid);
    
    if ((dl->primpart_count>0) && (disklayoutbuilder_recreate_primary_partitions(dl)!=0))
    {   errprintf("disklayoutbuilder_recreate_primary_partitions() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    if ((dl->logicpart_count>0) && (disklayoutbuilder_recreate_logical_partitions(dl)!=0))
    {   msgprintf(MSG_STACK, "disklayoutbuilder_recreate_logical_partitions() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    if (disklayoutbuilder_set_partitions_attributes(dl)!=0)
    {   msgprintf(MSG_STACK, "disklayoutbuilder_set_partitions_flags() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    while ((delay-- > 0) && (get_abort()==false))
    {   msgprintf(MSG_FORCE, "Partition table will be restored on %s in %d seconds, press Ctrl+C to abort\n", destdisk, delay);
        sleep(1);
    }
    
    if (get_abort()==true)
    {   msgprintf(MSG_FORCE, "operation aborted by user\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    } 
    
    msgprintf(MSG_FORCE, "Restoration of the partition table on disk=[%s]...\n", destdisk);
    
    if (disklayoutbuilder_commit_changes(dl)!=0)
    {   errprintf("disklayoutbuilder_commit_changes() failed\n");
        ret=-1;
        goto restpt_restore_device_cleanup;
    }
    
    msgprintf(MSG_FORCE, "Partition table has been written on disk=[%s]\n", destdisk);
       
restpt_restore_device_cleanup:
    if (dl)
        disklayoutbuilder_destroy(dl);
    if (dicodiskinfo)
        strdico_destroy(dicodiskinfo);
    return ret;
}

// =================================================================================

int showpt(char *partdesc, int id)
{
    cstrdico *dico;
    
    dico=strdico_alloc();
    parttable_read_xml_dump(partdesc, dico);
    restpt_show_device(dico, id);
    strdico_destroy(dico);
    
    return 0;
}
