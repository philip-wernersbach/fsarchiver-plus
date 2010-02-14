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
#include <libxml/xmlreader.h>
#include <parted/parted.h>

#include "fsarchiver.h"
#include "layout_read.h"
#include "strdico.h"
#include "common.h"
#include "error.h"

typedef struct s_disklayoutreader
{   xmlTextReaderPtr reader;
} cdisklayoutreader;

cdisklayoutreader *disklayoutreader_alloc()
{
    cdisklayoutreader *dlr;
    
    if ((dlr=malloc(sizeof(cdisklayoutreader)))==NULL)
    {   printf("malloc(%ld) failed\n", (long)sizeof(cdisklayoutreader));
        return NULL;
    }
    
    memset(dlr, 0, sizeof(cdisklayoutreader));
    
    return dlr;
}

int disklayoutreader_destroy(cdisklayoutreader *dlr)
{
    assert(dlr);
    free(dlr);
    return 0;
}

int disklayoutreader_open(cdisklayoutreader *dlr, const char *data)
{
    assert(dlr);
        
    if ((dlr->reader=(xmlTextReaderPtr)xmlReaderForMemory(data, strlen(data), NULL, NULL, 0)) == NULL)
    {   printf("disklayoutreader_open_read: Error creating the xml buffer\n");
        return -1;
    }
    
    if (((xmlTextReaderRead(dlr->reader)) != 1) ||
        (strcmp((char*)xmlTextReaderConstName(dlr->reader), "disklayout")!=0))
    {   printf("disklayoutreader_open_read: Error xmlTextReaderRead\n");
        return -1;
    }
    
    return 0;
}

int disklayoutreader_close(cdisklayoutreader *dlr)
{
    assert(dlr);
    
    if (((xmlTextReaderRead(dlr->reader)) != 1) ||
        (strcmp((const char*)xmlTextReaderConstName(dlr->reader), "disklayout")!=0))
    {   printf("disklayoutreader_close: expected [disklayout]\n");
        return -1;
    }
    
    xmlFreeTextReader(dlr->reader);
    
    return 0;
}

int disklayoutreader_read_partition(cdisklayoutreader *dlr, cstrdico *dico)
{
    char keyname[1024];
    const char *name;
    const char *value;
    u8 *spartnum;
    int partnum;
    int i;
    
    assert(dlr);
      
    if (xmlTextReaderAttributeCount(dlr->reader) <= 0)
    {   printf("xmlTextReaderAttributeCount()=0\n");
        return -1;
    }
    
    if ((spartnum=xmlTextReaderGetAttribute(dlr->reader, BAD_CAST "num"))==NULL)
    {   printf("xmlTextReaderGetAttribute()=NULL: cannot get partition number\n");
        return -1;
    }
    else
    {   partnum=atoi((char*)spartnum);
        free(spartnum);
    }
    
    for (i = xmlTextReaderMoveToFirstAttribute(dlr->reader); i == 1; i = xmlTextReaderMoveToNextAttribute(dlr->reader))
    {
        name = (const char *)xmlTextReaderConstLocalName(dlr->reader);
        value = (const char *)xmlTextReaderConstValue(dlr->reader);
        if (strcmp(name, "num")!=0)
        {
            snprintf(keyname, sizeof(keyname), "part%d.%s", partnum, name);
            strdico_set_value(dico, keyname, value);
        }
    }
    
    return 0;
}

int disklayoutreader_read_device(cdisklayoutreader *dlr, cstrdico *dico)
{
    char buffer[1024];
    const char *name;
    const char *value;
    int partcnt;
    int i;
    
    assert(dlr);
    
    //printf("==============================================\n");
    
    if (((xmlTextReaderRead(dlr->reader)) != 1) ||
        (strcmp((const char *)xmlTextReaderConstName(dlr->reader), "disk")!=0))
    {   printf("xmlTextReaderRead() failed\n");
        return -1;
    }
    
    for (i = xmlTextReaderMoveToFirstAttribute(dlr->reader); i == 1; i = xmlTextReaderMoveToNextAttribute(dlr->reader))
    {
        name = (const char *)xmlTextReaderConstLocalName(dlr->reader);
        value = (const char *)xmlTextReaderConstValue(dlr->reader);
        //printf("----> device: name=[%s] value=[%s]\n", name, value);
        strdico_set_value(dico, name, value);
    }
    
    for (partcnt=0; (xmlTextReaderRead(dlr->reader)==1) && (strcmp((char*)xmlTextReaderConstName(dlr->reader), "partition")==0); partcnt++)
    {
        if (disklayoutreader_read_partition(dlr, dico)!=0)
        {   printf("disklayoutreader_read_partition() failed\n");
            return -1;
        }
    }
    
    snprintf(buffer, sizeof(buffer), "%d", partcnt);
    strdico_set_value(dico, "partcount", buffer);
    
    //printf("----> device: name=[partcnt] value=[%d]\n", partcnt);
    
    if (strcmp((char*)xmlTextReaderConstName(dlr->reader), "disk")!=0)
    {   printf("expected name=[disk]\n");
        return -1;
    }
    
    //printf("==============================================\n");
    
    return 0;
}

int convert_dico_to_device(cstrdico *dico, cdiskspecs *disk)
{
    memset(disk, 0, sizeof(cdiskspecs));
    
    if ((strdico_get_s64(dico, &disk->partcount, "partcount")!=0) ||
        (strdico_get_s64(dico, &disk->sectsize, "sector_size")!=0) ||
        (strdico_get_string(dico, disk->model, sizeof(disk->model), "model")!=0) ||
        (strdico_get_string(dico, disk->path, sizeof(disk->path), "path")!=0) ||
        (strdico_get_s64(dico, &disk->length, "length")!=0) ||
        (strdico_get_string(dico, disk->disklabel, sizeof(disk->disklabel), "disklabel")!=0))        
        return -1;
    
    format_size(disk->length*disk->sectsize, disk->size, sizeof(disk->size), 'h');

    return 0;
}

int convert_dico_to_partition(cstrdico *dico, cpartspecs *part, int partnum)
{
    char keyname[1024];
    
    memset(part, 0, sizeof(cpartspecs));
    
    snprintf(keyname, sizeof(keyname), "part%d.type", partnum);
    if (strdico_get_string(dico, part->type, sizeof(part->type), keyname)!=0)
        return -1;
    snprintf(keyname, sizeof(keyname), "part%d.flags", partnum);
    if (strdico_get_string(dico, part->flags, sizeof(part->flags), keyname)!=0)
        return -1;
    snprintf(keyname, sizeof(keyname), "part%d.name", partnum);
    if (strdico_get_string(dico, part->name, sizeof(part->name), keyname)!=0)
        return -1;
    snprintf(keyname, sizeof(keyname), "part%d.fstype", partnum);
    if (strdico_get_string(dico, part->fstype, sizeof(part->fstype), keyname)!=0)
        return -1;
    snprintf(keyname, sizeof(keyname), "part%d.start", partnum);
    if (strdico_get_s64(dico, &part->start, keyname)!=0)
        return -1;
    snprintf(keyname, sizeof(keyname), "part%d.length", partnum);
    if (strdico_get_s64(dico, &part->length, keyname)!=0)
        return -1;
    snprintf(keyname, sizeof(keyname), "part%d.end", partnum);
    if (strdico_get_s64(dico, &part->end, keyname)!=0)
        return -1;
    part->num=partnum;
    
    return 0;
}

int parttable_read_xml_dump(char *partdesc, cstrdico *dico)
{
    cdisklayoutreader *dlr;
    
    if ((dlr=disklayoutreader_alloc())==NULL)
    {
        printf("Error dlr\n");
        return -1;
    }
    disklayoutreader_open(dlr, partdesc);
    disklayoutreader_read_device(dlr, dico);
    disklayoutreader_close(dlr);
    disklayoutreader_destroy(dlr);
    
    return 0;
}
