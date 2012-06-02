/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2008-2011 Francois Dupoux.  All rights reserved.
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
#include <unistd.h>
#include <fcntl.h>
#include <sys/statvfs.h>
#include <sys/stat.h>
#include <assert.h>

#include "fsarchiver.h"
#include "thread_compat06.h"
#include "dico.h"
#include "common.h"
#include "error.h"
#include "syncthread.h"
#include "queue.h"
#include "options.h"
#include "comp_gzip.h"
#include "comp_bzip2.h"
#include "error.h"

char *valid_magic[]={FSA_MAGIC_MAIN, FSA_MAGIC_VOLH, FSA_MAGIC_VOLF,
    FSA_MAGIC_FSIN, FSA_MAGIC_FSYB, FSA_MAGIC_DATF, FSA_MAGIC_OBJT,
    FSA_MAGIC_BLKH, FSA_MAGIC_FILF, FSA_MAGIC_DIRS, 0};

// returns true if magic is a valid magic-string
int is_magic_valid(char *magic)
{
    int i;
    for (i=0; valid_magic[i] != 0; i++)
        if (memcmp(valid_magic[i], magic, 4)==0)
            return true;
    return false;
}

int archreader_init(carchreader *ai)
{
    assert(ai);
    memset(ai, 0, sizeof(struct s_archreader));
    ai->cryptalgo=ENCRYPT_NULL;
    ai->compalgo=COMPRESS_NULL;
    ai->fsacomp=-1;
    ai->complevel=-1;
    ai->archfd=-1;
    ai->archid=0;
    ai->curvol=0;
    ai->filefmtver=0;
    ai->hasdirsinfohead=false;
    return 0;
}

int archreader_destroy(carchreader *ai)
{
    assert(ai);
    return 0;
}

int archreader_open(carchreader *ai)
{   
    struct stat64 st;
    char volhead[64];
    int magiclen;
    
    assert(ai);
    
    // on the archive volume
    ai->archfd=open64(ai->volpath, O_RDONLY|O_LARGEFILE);
    if (ai->archfd<0)
    {   sysprintf ("cannot open archive %s\n", ai->volpath);
        return -1;
    }
    
    // check the archive volume is a regular file
    if (fstat64(ai->archfd, &st)!=0)
    {   sysprintf("fstat64(%s) failed\n", ai->volpath);
        return -1;
    }
    if (!S_ISREG(st.st_mode))
    {   errprintf("%s is not a regular file, cannot continue\n", ai->volpath);
        close(ai->archfd);
        return -1;
    }
    
    // read file format version and rewind to beginning of the volume
    if (read(ai->archfd, volhead, sizeof(volhead))!=sizeof(volhead))
    {   sysprintf("cannot read magic from %s\n", ai->volpath);
        close(ai->archfd);
        return -1;
    }
    if (lseek64(ai->archfd, 0, SEEK_SET)!=0)
    {   sysprintf("cannot rewind volume %s\n", ai->volpath);
        close(ai->archfd);
        return -1;
    }
    
    // interpret magic an get file format version
    magiclen=strlen(FSA_FILEFORMAT);
    if ((memcmp(volhead+40, "FsArCh_001", magiclen)==0) || (memcmp(volhead+40, "FsArCh_00Y", magiclen)==0))
    {
        ai->filefmtver=1;
    }
    else if (memcmp(volhead+42, "FsArCh_002", magiclen)==0)
    {
        ai->filefmtver=2;
    }
    else
    {
        close(ai->archfd);
        return -1;
    }
    
    msgprintf(MSG_VERB2, "Detected fileformat=%d in archive %s\n", (int)ai->filefmtver, ai->volpath);
    
    return 0;
}

int archreader_close(carchreader *ai)
{
    assert(ai);
    
    if (ai->archfd<0)
        return -1;
    
    lockf(ai->archfd, F_ULOCK, 0);
    close(ai->archfd);
    ai->archfd=-1;

    return 0;
}

int archreader_volpath(carchreader *ai)
{
    int res;
    res=get_path_to_volume(ai->volpath, PATH_MAX, ai->basepath, ai->curvol);
    return res;
}

int archreader_incvolume(carchreader *ai, bool waitkeypress)
{
    assert(ai);
    ai->curvol++;
    return archreader_volpath(ai);
}

int archreader_read_data(carchreader *ai, void *data, u64 size)
{
    long lres;
    
    assert(ai);

    if ((lres=read(ai->archfd, (char*)data, (long)size))!=(long)size)
    {   sysprintf("read failed: read(size=%ld)=%ld\n", (long)size, lres);
        return -1;
    }
    
    return 0;
}

int archreader_read_dico(carchreader *ai, cdico *d)
{
    u16 size;
    u32 headerlen;
    u32 origsum;
    u32 newsum;
    u8 *buffer;
    u8 *bufpos;
    u16 temp16;
    u32 temp32;
    u8 section;
    u16 count;
    u8 type;
    u16 key;
    int i;
    
    assert(ai);
    assert(d);
    
    // header-len, header-data, header-checksum
    switch (ai->filefmtver)
    {
        case 1:
            if (archreader_read_data(ai, &temp16, sizeof(temp16))!=0)
            {   errprintf("imgdisk_read_data() failed\n");
                return OLDERR_FATAL;
            }
            headerlen=le16_to_cpu(temp16);
            break;
        case 2:
            if (archreader_read_data(ai, &temp32, sizeof(temp32))!=0)
            {   errprintf("imgdisk_read_data() failed\n");
                return OLDERR_FATAL;
            }
            headerlen=le32_to_cpu(temp32);
            break;
        default:
            errprintf("Fatal error: invalid file format version: ai->filefmtver=%d\n", ai->filefmtver);
            return OLDERR_FATAL;
    }
    
    bufpos=buffer=malloc(headerlen);
    if (!buffer)
    {   errprintf("cannot allocate memory for header\n");
        return FSAERR_ENOMEM;
    }
    
    if (archreader_read_data(ai, buffer, headerlen)!=0)
    {   errprintf("cannot read header data\n");
        free(buffer);
        return OLDERR_FATAL;
    }
    
    if (archreader_read_data(ai, &temp32, sizeof(temp32))!=0)
    {   errprintf("cannot read header checksum\n");
        free(buffer);
        return OLDERR_FATAL;
    }
    origsum=le32_to_cpu(temp32);
    
    // check header-data integrity using checksum    
    newsum=fletcher32(buffer, headerlen);
    
    if (newsum!=origsum)
    {   errprintf("bad checksum for header\n");
        free(buffer);
        return OLDERR_MINOR; // header corrupt --> skip file
    }
    
    // read count from buffer
    memcpy(&temp16, bufpos, sizeof(temp16));
    bufpos+=sizeof(temp16);
    count=le16_to_cpu(temp16);
    
    // read items
    for (i=0; i < count; i++)
    {
        // a. read type from buffer
        memcpy(&type, bufpos, sizeof(type));
        bufpos+=sizeof(section);
        
        // b. read section from buffer
        memcpy(&section, bufpos, sizeof(section));
        bufpos+=sizeof(section);
        
        // c. read key from buffer
        memcpy(&temp16, bufpos, sizeof(temp16));
        bufpos+=sizeof(temp16);
        key=le16_to_cpu(temp16);
        
        // d. read sizeof(data)
        memcpy(&temp16, bufpos, sizeof(temp16));
        bufpos+=sizeof(temp16);
        size=le16_to_cpu(temp16);
        
        // e. add item to dico
        if (dico_add_generic(d, section, key, bufpos, size, type)!=0)
            return OLDERR_FATAL;
        bufpos+=size;
    }
    
    free(buffer);
    return FSAERR_SUCCESS;
}

int archreader_read_header(carchreader *ai, u32 *magic, cdico **d, bool allowseek, u16 *fsid)
{
    char magbuf[4];
    u32 magictmp;
    s64 curpos;
    u16 temp16;
    u32 temp32;
    u32 archid;
    int res;
    
    assert(ai);
    assert(d);
    assert(fsid);
    assert(magic);
    
    // init
    *fsid=FSA_FILESYSID_NULL;
    *magic=0;
    *d=NULL;
    
    if ((*d=dico_alloc())==NULL)
    {   errprintf("dico_alloc() failed\n");
        return OLDERR_FATAL;
    }
    
    // search for next read header marker and magic (it may be further if corruption in archive)
    if ((curpos=lseek64(ai->archfd, 0, SEEK_CUR))<0)
    {   sysprintf("lseek64() failed to get the current position in archive\n");
        return OLDERR_FATAL;
    }
    
    if ((res=archreader_read_data(ai, magbuf, sizeof(magbuf)))!=FSAERR_SUCCESS)
    {   msgprintf(MSG_STACK, "cannot read header magic: res=%d\n", res);
        return OLDERR_FATAL;
    }
    memcpy(&magictmp, magbuf, 4);
    *magic = le32_to_cpu(magictmp);
    
    // we don't want to search for the magic if it's a volume header
    if (is_magic_valid(magbuf)!=true && allowseek!=true)
    {   errprintf("cannot read header magic: this is not a valid fsarchiver file, or it has been created with a different version.\n");
        return OLDERR_FATAL;
    }
    
    while (is_magic_valid(magbuf)!=true)
    {
        if (lseek64(ai->archfd, curpos++, SEEK_SET)<0)
        {   sysprintf("lseek64(pos=%lld, SEEK_SET) failed\n", (long long)curpos);
            return OLDERR_FATAL;
        }
        if ((res=archreader_read_data(ai, magic, 4))!=FSAERR_SUCCESS)
        {   msgprintf(MSG_STACK, "cannot read header magic: res=%d\n", res);
            return OLDERR_FATAL;
        }
    }
    
    // read the archive id
    if ((res=archreader_read_data(ai, &temp32, sizeof(temp32)))!=FSAERR_SUCCESS)
    {   msgprintf(MSG_STACK, "cannot read archive-id in header: res=%d\n", res);
        return OLDERR_FATAL;
    }
    archid=le32_to_cpu(temp32);
    if (ai->archid) // only check archive-id if it's known (when main header has been read)
    {
        if (archid!=ai->archid)
        {   errprintf("archive-id in header does not match: archid=[%.8x], expected=[%.8x]\n", archid, ai->archid);
            return OLDERR_MINOR;
        }
    }
    
    // read the filesystem id
    if ((res=archreader_read_data(ai, &temp16, sizeof(temp16)))!=FSAERR_SUCCESS)
    {   msgprintf(MSG_STACK, "cannot read filesystem-id in header: res=%d\n", res);
        return OLDERR_FATAL;
    }
    *fsid=le16_to_cpu(temp16);
    
    // read the dico of the header
    if ((res=archreader_read_dico(ai, *d))!=FSAERR_SUCCESS)
    {   msgprintf(MSG_STACK, "imgdisk_read_dico() failed\n");
        return res;
    }
    
    return FSAERR_SUCCESS;
}

int archreader_read_volheader(carchreader *ai)
{
    char creatver[FSA_MAX_PROGVERLEN];
    char filefmt[FSA_MAX_FILEFMTLEN];
    u32 magic = 0;
    cdico *d;
    u32 volnum;
    u32 readid;
    u16 fsid;
    int res;
    int ret=0;
    
    // init
    assert(ai);

    // ---- a. read header from archive file
    if ((res=archreader_read_header(ai, &magic, &d, false, &fsid))!=FSAERR_SUCCESS)
    {   errprintf("archreader_read_header() failed to read the archive header\n");
        return -1;
    }
    
    // ---- b. check the magic is what we expected
    if (memcmp(&magic, FSA_MAGIC_VOLH, 4)!=0)
    {   errprintf("magic is not what we expected: found=[%ld]=[%4s] and expected=[%s]\n", (long)magic, (char*)&magic, FSA_MAGIC_VOLH);
        ret=-1; goto archio_read_volheader_error;
    }
    
    if (dico_get_u32(d, 0, VOLUMEHEADKEY_ARCHID, &readid)!=0)
    {   errprintf("cannot get VOLUMEHEADKEY_ARCHID from the volume header\n");
        ret=-1; goto archio_read_volheader_error;
    }
    
    // ---- c. check the archive id
    if (ai->archid==0) // archid not know: this is the first volume
    {
        ai->archid=readid;
    }
    else if (readid!=ai->archid) // archid known: not the first volume
    {   errprintf("wrong header id: found=%.8x and expected=%.8x\n", readid, ai->archid);
        ret=-1; goto archio_read_volheader_error;
    }
    
    // ---- d. check the volnum
    if (dico_get_u32(d, 0, VOLUMEHEADKEY_VOLNUM, &volnum)!=0)
    {   errprintf("cannot get VOLUMEHEADKEY_VOLNUM from the volume header\n");
        ret=-1; goto archio_read_volheader_error;
    }
    if (volnum!=ai->curvol) // not the right volume number
    {   errprintf("wrong volume number in [%s]: volnum is %d and we need volnum %d\n", ai->volpath, (int)volnum, (int)ai->curvol);
        ret=-1; goto archio_read_volheader_error;
    }
    
    // ---- d. check the the file format
    if (dico_get_data(d, 0, VOLUMEHEADKEY_FILEFORMATVER, filefmt, FSA_MAX_FILEFMTLEN, NULL)!=0)
    {   errprintf("cannot find VOLUMEHEADKEY_FILEFORMATVER in main-header\n");
        ret=-1; goto archio_read_volheader_error;
    }
    
    if (ai->filefmt[0]==0) // filefmt not know: this is the first volume
    {
        memcpy(ai->filefmt, filefmt, FSA_MAX_FILEFMTLEN);
    }
    else if (strncmp(filefmt, ai->filefmt, FSA_MAX_FILEFMTLEN)!=0)
    {
        errprintf("This archive is based on a different file format: [%s]. Cannot continue.\n", ai->filefmt);
        errprintf("It has been created with fsarchiver [%s], you should extrat the archive using that version.\n", ai->creatver);
        errprintf("The current version of the program is [%s], and it's based on format [%s]\n", FSA_VERSION, FSA_FILEFORMAT);
        ret=-1; goto archio_read_volheader_error;
    }
    
    if (dico_get_data(d, 0, VOLUMEHEADKEY_PROGVERCREAT, creatver, FSA_MAX_PROGVERLEN, NULL)!=0)
    {   errprintf("cannot find VOLUMEHEADKEY_PROGVERCREAT in main-header\n");
        ret=-1; goto archio_read_volheader_error;
    }
    
    if (ai->creatver[0]==0)
        memcpy(ai->creatver, creatver, FSA_MAX_PROGVERLEN);
    
archio_read_volheader_error:
    dico_destroy(d);
    
    return ret;
}

int archreader_read_block(carchreader *ai, cdico *in_blkdico, int in_skipblock, int *out_sumok, struct s_blockinfo *out_blkinfo)
{
    u32 arblockcsumorig;
    u32 arblockcsumcalc;
    u32 curblocksize; // data size
    u64 blockoffset; // offset of the block in the file
    u16 compalgo; // compression algo used
    u16 cryptalgo; // encryption algo used
    u32 finalsize; // compressed  block size
    u32 compsize;
    u8 *buffer;
    
    assert(ai);
    assert(out_sumok);
    assert(in_blkdico);
    assert(out_blkinfo);
    
    // init
    memset(out_blkinfo, 0, sizeof(struct s_blockinfo));
    *out_sumok=-1;
    
    if (dico_get_u64(in_blkdico, 0, BLOCKHEADITEMKEY_BLOCKOFFSET, &blockoffset)!=0)
    {   msgprintf(3, "cannot get blockoffset from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_REALSIZE, &curblocksize)!=0 || curblocksize>FSA_MAX_BLKSIZE)
    {   msgprintf(3, "cannot get blocksize from block-header\n");
        return -1;
    }
    
    if (dico_get_u16(in_blkdico, 0, BLOCKHEADITEMKEY_COMPRESSALGO, &compalgo)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_COMPRESSALGO from block-header\n");
        return -1;
    }
    
    if (dico_get_u16(in_blkdico, 0, BLOCKHEADITEMKEY_ENCRYPTALGO, &cryptalgo)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ENCRYPTALGO from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_ARSIZE, &finalsize)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ARSIZE from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_COMPSIZE, &compsize)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_COMPSIZE from block-header\n");
        return -1;
    }
    
    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_ARCSUM, &arblockcsumorig)!=0)
    {   msgprintf(3, "cannot get BLOCKHEADITEMKEY_ARCSUM from block-header\n");
        return -1;
    }
    
    if (in_skipblock==true) // the main thread does not need that block (block belongs to a filesys we want to skip)
    {
        if (lseek64(ai->archfd, (long)finalsize, SEEK_CUR)<0)
        {   sysprintf("cannot skip block (finalsize=%ld) failed\n", (long)finalsize);
            return -1;
        }
        return 0;
    }
    
    // ---- allocate memory
    if ((buffer=malloc(finalsize))==NULL)
    {   errprintf("cannot allocate block: malloc(%d) failed\n", finalsize);
        return FSAERR_ENOMEM;
    }
    
    if (read(ai->archfd, buffer, (long)finalsize)!=(long)finalsize)
    {   sysprintf("cannot read block (finalsize=%ld) failed\n", (long)finalsize);
        free(buffer);
        return -1;
    }
    
    // prepare blkinfo
    out_blkinfo->blkdata=(char*)buffer;
    out_blkinfo->blkrealsize=curblocksize;
    out_blkinfo->blkoffset=blockoffset;
    out_blkinfo->blkarcsum=arblockcsumorig;
    out_blkinfo->blkcompalgo=compalgo;
    out_blkinfo->blkcryptalgo=cryptalgo;
    out_blkinfo->blkarsize=finalsize;
    out_blkinfo->blkcompsize=compsize;
    
    // ---- checksum
    arblockcsumcalc=fletcher32(buffer, finalsize);
    if (arblockcsumcalc!=arblockcsumorig) // bad checksum
    {
        errprintf("block is corrupt at offset=%ld, blksize=%ld\n", (long)blockoffset, (long)curblocksize);
        free(out_blkinfo->blkdata);
        if ((out_blkinfo->blkdata=malloc(curblocksize))==NULL)
        {   errprintf("cannot allocate block: malloc(%d) failed\n", curblocksize);
            return FSAERR_ENOMEM;
        }
        memset(out_blkinfo->blkdata, 0, curblocksize);
        *out_sumok=false;
        // go to the beginning of the corrupted contents so that the next header is searched here
        if (lseek64(ai->archfd, -(long long)finalsize, SEEK_CUR)<0)
        {   errprintf("lseek64() failed\n");
        }
    }
    else // no corruption detected
    {
        *out_sumok=true;
    }
    
    return 0;
}

void *thread_compat06_fct(void *args)
{
    struct s_blockinfo blkinfo;
    u32 endofarchive=false;
    carchreader ai;
    cdico *dico=NULL;
    int skipblock;
    u32 magic;
    u16 fsid;
    int sumok;
    int status;
    u64 errors;
    s64 lres;
    int res;

    // init
    memset(&ai, 0, sizeof(ai));
    inc_secthreads();
    errors=0;

    // init archive
    archreader_init(&ai);

    // set archive path
    snprintf(ai.basepath, PATH_MAX, "%s", g_archive);

    // open archive file
    if (archreader_volpath(&ai)!=0)
    {   errprintf("archreader_volpath() failed\n");
        goto thread_reader_fct_error;
    }
    
    if (archreader_open(&ai)!=0)
    {   errprintf("archreader_open(%s) failed\n", ai.basepath);
        goto thread_reader_fct_error;
    }
    
    // read volume header
    if (archreader_read_volheader(&ai)!=0)
    {   errprintf("archio_read_volheader() failed\n");
        goto thread_reader_fct_error;
    }
    
    // ---- read main archive header
    if ((res=archreader_read_header(&ai, &magic, &dico, false, &fsid))!=FSAERR_SUCCESS)
    {   errprintf("archreader_read_header() failed to read the archive header\n");
        goto thread_reader_fct_error; // this header is required to continue
    }
    
    if (dico_get_u32(dico, 0, MAINHEADKEY_ARCHIVEID, &ai.archid)!=0)
    {   msgprintf(3, "cannot get archive-id from main header\n");
        goto thread_reader_fct_error;
    }
    
    if ((lres=queue_add_header(g_queue, dico, magic, fsid))!=FSAERR_SUCCESS)
    {   errprintf("queue_add_header()=%ld=%s failed to add the archive header\n", (long)lres, error_int_to_string(lres));
        goto thread_reader_fct_error;
    }
    
    // read all other data from file (filesys-header, normal objects headers, ...)
    while (endofarchive==false && get_status()==STATUS_RUNNING)
    {
        if ((res=archreader_read_header(&ai, &magic, &dico, true, &fsid))!=FSAERR_SUCCESS)
        {   dico_destroy(dico);
            msgprintf(MSG_STACK, "archreader_read_header() failed to read next header\n");
            if (res==OLDERR_MINOR) // header is corrupt or not what we expected
            {   errors++;
                msgprintf(MSG_DEBUG1, "OLDERR_MINOR\n");
                continue;
            }
            else // fatal error (eg: cannot read archive from disk)
            {
                msgprintf(MSG_DEBUG1, "!OLDERR_MINOR\n");
                goto thread_reader_fct_error;
            }
        }
        
        // read header and see if it's for archive management or higher level data
        if (memcmp(&magic, FSA_MAGIC_VOLF, 4)==0) // header is "end of volume"
        {
            archreader_close(&ai);
            
            // check the "end of archive" flag in header
            if (dico_get_u32(dico, 0, VOLUMEFOOTKEY_LASTVOL, &endofarchive)!=0)
            {   errprintf("cannot get compr from block-header\n");
                goto thread_reader_fct_error;
            }
            msgprintf(MSG_VERB2, "End of volume [%s]\n", ai.volpath);
            if (endofarchive!=true)
            {
                archreader_incvolume(&ai, false);
                while (regfile_exists(ai.volpath)!=true)
                {
                    // wait until the queue is empty so that the main thread does not pollute the screen
                    while (queue_count(g_queue)>0)
                        usleep(5000);
                    fflush(stdout);
                    fflush(stderr);
                    msgprintf(MSG_FORCE, "File [%s] is not found, please type the path to volume %ld:\n", ai.volpath, (long)ai.curvol);
                    fprintf(stdout, "New path:> ");
                    res=scanf("%s", ai.volpath);
                }
                
                msgprintf(MSG_VERB2, "New volume is [%s]\n", ai.volpath);
                if (archreader_open(&ai)!=0)
                {   msgprintf(MSG_STACK, "archreader_open() failed\n");
                    goto thread_reader_fct_error;
                }
                if (archreader_read_volheader(&ai)!=0)
                {   msgprintf(MSG_STACK, "archio_read_volheader() failed\n");
                    goto thread_reader_fct_error;
                }
            }
            dico_destroy(dico);
        }
        else // high-level archive (not involved in volume management)
        {
            if (memcmp(&magic, FSA_MAGIC_BLKH, 4)==0) // header starts a data block
            {
                skipblock=(g_fsbitmap[fsid]==0);
                //errprintf("DEBUG: skipblock=%d g_fsbitmap[fsid=%d]=%d\n", skipblock, (int)fsid, (int)g_fsbitmap[fsid]);
                if (archreader_read_block(&ai, dico, skipblock, &sumok, &blkinfo)!=0)
                {   msgprintf(MSG_STACK, "archreader_read_block() failed\n");
                    goto thread_reader_fct_error;
                }
                
                if (skipblock==false)
                {
                    status=((sumok==true)?QITEM_STATUS_TODO:QITEM_STATUS_DONE);
                    if ((lres=queue_add_block(g_queue, &blkinfo, status))!=FSAERR_SUCCESS)
                    {   if (lres!=FSAERR_NOTOPEN)
                            errprintf("queue_add_block()=%ld=%s failed\n", (long)lres, error_int_to_string(lres));
                        goto thread_reader_fct_error;
                    }
                    if (sumok==false) errors++;
                    dico_destroy(dico);
                }
            }
            else // another higher level header
            {
                // if it's a global header or a if this local header belongs to a filesystem that the main thread needs
                if (fsid==FSA_FILESYSID_NULL || g_fsbitmap[fsid]==1)
                {
                    if ((lres=queue_add_header(g_queue, dico, magic, fsid))!=FSAERR_SUCCESS)
                    {   msgprintf(MSG_STACK, "queue_add_header()=%ld=%s failed\n", (long)lres, error_int_to_string(lres));
                        goto thread_reader_fct_error;
                    }
                }
                else // header not used: remove data strucutre in dynamic memory
                {
                    dico_destroy(dico);
                }
            }
        }
    }
    
thread_reader_fct_error:
    archreader_destroy(&ai);
    msgprintf(MSG_DEBUG1, "THREAD-READER: queue_set_end_of_queue(g_queue, true)\n");
    queue_set_end_of_queue(g_queue, true); // don't wait for more data from this thread
    dec_secthreads();
    msgprintf(MSG_DEBUG1, "THREAD-READER: exit\n");
    return NULL;
}
