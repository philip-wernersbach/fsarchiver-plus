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

#include <sys/statvfs.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>

#include "fsarchiver.h"
#include "syncthread.h"
#include "dico.h"
#include "common.h"
#include "options.h"
#include "archio.h"
#include "queue.h"
#include "error.h"

/*
 * This file implement low-level management of the archive file:
 * reading and writing the archive file, volume splitting.
 * It should never be possible to lose the entire volume / archive just
 * because one critical header is corrupt. For that reason two copies of
 * the volume header are written: at the very beginning and at the very
 * end of each volume. We can read the volume if at least one of them is
 * good.
 */

carchio *archio_alloc()
{
    carchio *ai;

    if ((ai = calloc(1, sizeof(carchio))) == NULL)
        return NULL;

    assert(sizeof(ciohead)==32);
    strlist_init(&ai->vollist);

    ai->newarch = false;
    ai->lastvol = false;
    ai->curblock = 0;
    ai->archfd = -1;
    ai->archid = 0;
    ai->curvol = 0;
    ai->ecclevel = 0;

    return ai;
}

int archio_destroy(carchio *ai)
{
    strlist_destroy(&ai->vollist);
    return FSAERR_SUCCESS;
}

int archio_incvolume(carchio *ai)
{
    ai->curvol++;
    get_path_to_volume(ai->volpath, PATH_MAX, ai->basepath, ai->curvol);
    return FSAERR_SUCCESS;
}

s64 archio_get_currentpos(carchio *ai)
{
    return (s64)lseek64(ai->archfd, 0, SEEK_CUR);
}

int archio_init_read(carchio *ai, char *basepath, u32 *ecclevel)
{
    *ecclevel = 0;

    // set path to first volume
    snprintf(ai->basepath, sizeof(ai->basepath), "%s", basepath);
    snprintf(ai->volpath, sizeof(ai->volpath), "%s", basepath);

    // force the volume header to be read to get the ecclevel
    if (archio_read_block(ai, NULL, 0, NULL, NULL, 0) != FSAERR_SUCCESS)
    {
        errprintf("archio_read_block() failed to read the volume header\n");
        return FSAERR_READ;
    }

    *ecclevel = ai->ecclevel;

    return FSAERR_SUCCESS;
}

int archio_init_write(carchio *ai, char *basepath, u32 ecclevel)
{
    // set path to first volume
    snprintf(ai->basepath, sizeof(ai->basepath), "%s", basepath);
    snprintf(ai->volpath, sizeof(ai->volpath), "%s", basepath);

    // store ecclevel and generate an archive identifier
    ai->ecclevel = ecclevel;
    ai->archid = generate_random_number();

    return FSAERR_SUCCESS;
}

int archio_open_write(carchio *ai)
{
    struct stat64 st;
    long archflags=0;
    long archperm;
    ciohead head;
    int res;

    memset(&st, 0, sizeof(st));
    archflags = O_RDWR|O_CREAT|O_TRUNC|O_LARGEFILE;
    archperm = S_IRUSR|S_IWUSR|S_IRGRP|S_IROTH;

    if ((res = stat64(ai->volpath, &st)) == 0 && !S_ISREG(st.st_mode))
    {
        errprintf("%s already exists, and is not a regular file.\n", ai->basepath);
        return FSAERR_WRONGTYPE;
    }
    else if ((g_options.overwrite==0) && (res==0) && S_ISREG(st.st_mode)) // archive exists and is a regular file
    {
        errprintf("%s already exists, please remove it first.\n", ai->basepath);
        return FSAERR_EXISTS;
    }

    if ((ai->archfd = open64(ai->volpath, archflags, archperm)) < 0)
    {
        sysprintf ("cannot create archive %s\n", ai->volpath);
        return FSAERR_OPEN;
    }
    ai->newarch = true;

    strlist_add(&ai->vollist, ai->volpath);

    // write initial volume header
    memset(&head, 0, sizeof(head));
    head.magic = cpu_to_le32(FSA_MAGIC_IOPHYSHEADER);
    head.archid = cpu_to_le32(ai->archid);
    head.type = cpu_to_le16(IOHEAD_VOLHEAD);
    head.data.volhead.volnum = cpu_to_le32(ai->curvol);
    head.data.volhead.minver = cpu_to_le64(FSA_VERSION_BUILD(0, 7, 0, 0));
    head.data.volhead.ecclevel = cpu_to_le32(g_options.ecclevel);
    head.data.volhead.lastvol = cpu_to_le8(false);   
    head.csum = cpu_to_le32(fletcher32((u8 *)&head.data, sizeof(head.data)));
    if (archio_write_low_level(ai, (char*)&head, sizeof(head)) != 0)
    {
        errprintf("archio_write_low_level() failed to write volume header\n");
        return FSAERR_WRITE;
    }

    return FSAERR_SUCCESS;
}

int archio_open_read(carchio *ai)
{
    ciohead volhead[2];
    struct stat64 st;
    char buffer[PATH_MAX];
    u32 volnum = 0;
    u32 archid = 0;
    u64 minver = 0;
    u64 curver = 0;
    u16 headtype;
    bool validhead;
    u32 checksum;
    int i;

    // 1. check that the volume exists and is a regular file
    while (regfile_exists(ai->volpath) != true)
    {
        // wait until the queue is empty so that the main thread does not pollute the screen
        while (queue_count(g_queue) > 0) usleep(5000);
        fflush(stdout);
        fflush(stderr);

        // ask path to the current volume
        msgprintf(MSG_FORCE, "File [%s] is not found, please type the path to volume %ld:\n", ai->volpath, (long)ai->curvol);
        fprintf(stdout, "New path:> ");
        memset(buffer, 0, sizeof(buffer));
        fgets(buffer, sizeof(buffer), stdin);
        if (sscanf(buffer, "%s", ai->volpath) != 1)
        {
            errprintf("No valid alternative volume provided\n");
            return FSAERR_OPEN;
        }
    }

    if ((ai->archfd = open64(ai->volpath, O_RDONLY|O_LARGEFILE)) < 0)
    {
        sysprintf ("Cannot open archive %s\n", ai->volpath);
        return FSAERR_OPEN;
    }

    if (fstat64(ai->archfd, &st) != 0)
    {
        sysprintf("cannot read file details: fstat64(%s) failed\n", ai->volpath);
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_STAT;
    }

    if (!S_ISREG(st.st_mode))
    {
        errprintf("%s is not a regular file, cannot continue\n", ai->volpath);
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_WRONGTYPE;
    }

    if (st.st_size < (2 * sizeof(ciohead)))
    {
        errprintf("%s is not a valid fsarchiver volume: file is too small\n", ai->volpath);
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_READ;
    }

    // 2. read volfoot (contains a duplicate of things that are in volhead)
    if (lseek64(ai->archfd, st.st_size - sizeof(ciohead), SEEK_SET) < 0)
    {
        sysprintf("lseek64() failed to go to the end of the volume to get the volfoot header\n");
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_SEEK;
    }

    if (archio_read_low_level(ai, &volhead[1], sizeof(ciohead)) != 0)
    {
        errprintf("Failed to read volfoot volume header\n");
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_READ;
    }  

    // 3. read volhead
    if (lseek64(ai->archfd, 0, SEEK_SET) < 0)
    {
        sysprintf("lseek64() failed to go to the beginning of the volume to get the volhead header\n");
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_SEEK;
    }

    if (archio_read_low_level(ai, &volhead[0], sizeof(ciohead)) != 0)
    {
        errprintf("Failed to read volhead volume header\n");
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_READ;
    }  

    // 4. check that at least one of volhead or volfoot is valid
    //    this way we don't loose the entire archive if one if corrupt
    validhead = false;

    for (i = 0; (validhead == false) && (i < 2); i++)
    {
        checksum = fletcher32((u8 *)&volhead[i].data, sizeof(volhead[i].data));
        headtype = le16_to_cpu(volhead[i].type);
        if ((le32_to_cpu(volhead[i].magic) == FSA_MAGIC_IOPHYSHEADER)
            && (headtype == IOHEAD_VOLHEAD || headtype == IOHEAD_VOLFOOT)
            && (le32_to_cpu(volhead[i].csum) == checksum))
            {
                volnum = le32_to_cpu(volhead[i].data.volhead.volnum);
                minver = le64_to_cpu(volhead[i].data.volhead.minver);
                ai->ecclevel = le32_to_cpu(volhead[i].data.volhead.ecclevel);
                ai->lastvol = le8_to_cpu(volhead[i].data.volhead.lastvol);
                archid = le32_to_cpu(volhead[i].archid);
                validhead = true;
            }
            else
            {
                errprintf("The volume header (copy %d) is invalid\n", i);
            }
    }

    if (validhead == false)
    {
        errprintf("Both the volume header and footer are invalid.\n"
            "This file is either corrupt or not compatible with this\n"
            "fsarchiver version.\n");
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_CORRUPT;
    }

    // 5. check volume number is the one we expect
    if (volnum != ai->curvol)
    {
        errprintf("Unexpected fsarchiver volume number: "
            "found=%d expected=%d\n", (int)volnum, (int)ai->curvol);
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_WRONGVOL;
    }

    // 6. check minimum version requirement to read that file
    curver = FSA_VERSION_BUILD(PACKAGE_VERSION_A, PACKAGE_VERSION_B, PACKAGE_VERSION_C, PACKAGE_VERSION_D);
    if (curver < minver)
    {
        errprintf("Cannot read volume header: wrong fsarchiver version:\n"
            "- current version: %d.%d.%d.%d\n- minimum version required: %d.%d.%d.%d\n", 
            (int)FSA_VERSION_GET_A(curver), (int)FSA_VERSION_GET_B(curver),
            (int)FSA_VERSION_GET_C(curver), (int)FSA_VERSION_GET_D(curver),
            (int)FSA_VERSION_GET_A(minver), (int)FSA_VERSION_GET_B(minver),
            (int)FSA_VERSION_GET_C(minver), (int)FSA_VERSION_GET_D(minver));
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_WRONGVER;
    }

    // 7. save or check the archive id
    if (volnum == 0)
    {
        ai->archid = archid;
    }
    else if (archid != ai->archid)
    {
        errprintf("Unexpected fsarchiver archive identifier: "
            "found=%.8x expected=%.8x\n", (int)archid, (int)ai->archid);
        close(ai->archfd);
        ai->archfd = -1;
        return FSAERR_WRONGARCH;
    }

    return FSAERR_SUCCESS;
}

int archio_close_read(carchio *ai)
{
    if (ai->archfd < 0)
        return FSAERR_EINVAL;

    close(ai->archfd);
    ai->archfd = -1;

    return FSAERR_SUCCESS;
}

int archio_close_write(carchio *ai, bool lastvol)
{
    int ret = FSAERR_SUCCESS;
    ciohead head;

    if (ai->archfd < 0)
    {
        errprintf("Error: volume is not open\n");
        return FSAERR_EINVAL;
    }

    // prepare volume header
    memset(&head, 0, sizeof(head));
    head.magic = cpu_to_le32(FSA_MAGIC_IOPHYSHEADER);
    head.archid = cpu_to_le32(ai->archid);
    head.data.volhead.volnum = cpu_to_le32(ai->curvol);
    head.data.volhead.minver = cpu_to_le64(FSA_VERSION_BUILD(0, 7, 0, 0));
    head.data.volhead.ecclevel = cpu_to_le32(g_options.ecclevel);
    head.data.volhead.lastvol = cpu_to_le8(lastvol);
    head.csum = cpu_to_le32(fletcher32((u8 *)&head.data, sizeof(head.data)));

    // write one copy of the volume header at the end of the volume
    head.type = cpu_to_le16(IOHEAD_VOLFOOT);
    if (archio_write_low_level(ai, (char*)&head, sizeof(head)) != 0)
    {
        errprintf("archio_write_low_level() failed to write the first copy of the volume header\n");
        ret = FSAERR_WRITE;
    }

    // write another copy of the volume header at the beginning of the volume
    head.type = cpu_to_le16(IOHEAD_VOLHEAD);
    if ((lseek64(ai->archfd, 0, SEEK_SET) < 0) || (archio_write_low_level(ai, (char*)&head, sizeof(head)) != 0))
    {
        errprintf("archio_write_low_level() failed to write the second copy of the volume header\n");
        ret = FSAERR_WRITE;
    }

    fsync(ai->archfd);
    close(ai->archfd);
    ai->archfd = -1;

    return ret;
}

int archio_delete_all(carchio *ai)
{
    char volpath[PATH_MAX];
    int count;
    int i;

    if (ai->archfd >= 0)
    {
        archio_close_write(ai, false);
    }

    if (ai->newarch == true)
    {
        count = strlist_count(&ai->vollist);
        for (i = 0; i < count; i++)
        {
            if (strlist_getitem(&ai->vollist, i, volpath, sizeof(volpath)) == 0)
            {
                if (unlink(volpath) == 0)
                    msgprintf(MSG_FORCE, "removed %s\n", volpath);
                else
                    errprintf("cannot remove %s\n", volpath);
            }
        }
    }

    return FSAERR_SUCCESS;
}

int archio_write_block(carchio *ai, char *buffer, u32 bufsize, u32 bytesused, char *volpathbuf, int volpathbufsize)
{
    ciohead head;
    int res;

    memset(&head, 0, sizeof(head));
    head.magic = cpu_to_le32(FSA_MAGIC_IOPHYSHEADER);
    head.archid = cpu_to_le32(ai->archid);
    head.type = cpu_to_le16(IOHEAD_BLKHEAD);
    head.data.blkhead.blknum = cpu_to_le64(ai->curblock++);
    head.data.blkhead.blkid = cpu_to_le32(generate_random_number());
    head.data.blkhead.bytesused = cpu_to_le32(bytesused);
    head.csum = cpu_to_le32(fletcher32((u8 *)&head.data, sizeof(head.data)));

    if ((volpathbuf != NULL) && (volpathbufsize > 0))
    {
        memset(volpathbuf, 0, volpathbufsize);
    }

    // 1. close current volume if splitting enabled and current volume reached maxvolsize
    if (archio_split_check(ai, bufsize) == true)
    {
        archio_close_write(ai, false);
        archio_incvolume(ai);
    }

    // 2. create new volume if there is no current volume open
    if (ai->archfd == -1)
    {
        msgprintf(MSG_VERB2, "Creating volume %.3d: [%s]\n", (int)ai->curvol, ai->volpath);
        if ((res = archio_open_write(ai)) != 0)
        {
            msgprintf(MSG_STACK, "archio_open_write() failed\n");
            return res;
        }
    }

    // 3. write blk header
    head.type = cpu_to_le16(IOHEAD_BLKHEAD);
    if (archio_write_low_level(ai, (char *)&head, sizeof(head)) != 0)
    {
        msgprintf(MSG_STACK, "archio_write_low_level(%ld) failed to write data\n", (long)bufsize);
        return FSAERR_WRITE;
    }

    // 4. write current buffer to the archive
    if (archio_write_low_level(ai, buffer, bufsize) != 0)
    {
        msgprintf(MSG_STACK, "archio_write_low_level(%ld) failed to write data\n", (long)bufsize);
        return FSAERR_WRITE;
    }

    // 5. write blk footer
    head.type = cpu_to_le16(IOHEAD_BLKFOOT);
    if (archio_write_low_level(ai, (char *)&head, sizeof(head)) != 0)
    {
        msgprintf(MSG_STACK, "archio_write_low_level(%ld) failed to write data\n", (long)bufsize);
        return FSAERR_WRITE;
    }

    if ((volpathbuf != NULL) && (volpathbufsize > 0))
    {
        snprintf(volpathbuf, volpathbufsize, "%s", ai->volpath);
    }

    return FSAERR_SUCCESS;
}

int archio_read_block(carchio *ai, char *buffer, u32 datsize, u32 *bytesused, char *volpathbuf, int volpathbufsize)
{
    char blkbuf[(FSA_FEC_MAXVAL_N * FSA_FEC_PACKET_SIZE) + (2 * sizeof(ciohead))];
    ciohead *header1, *header2, *header;
    bool valid_header1 = false;
    bool valid_header2 = false;
    bool valid_block = false;
    u64 bytesignored = 0;
    s64 curpos = 0;
    bool end_of_vol;
    u32 totalsize;
    int res;

    if ((volpathbuf != NULL) && (volpathbufsize > 0))
    {
        memset(volpathbuf, 0, volpathbufsize);
    }

    if (datsize > (FSA_FEC_MAXVAL_N * FSA_FEC_PACKET_SIZE))
    {
        errprintf("archio_read_block(): Invalid block size: %ld\n", (long)datsize);
        return FSAERR_EINVAL;
    }

    // pointers to block header/footer in big buffer
    header1 = (ciohead *)&blkbuf[0];
    header2 = (ciohead *)&blkbuf[sizeof(ciohead) + datsize];
    totalsize = datsize + (2 * sizeof(ciohead));
    assert (totalsize <= sizeof(blkbuf));

    // read until we find a block with either a valid blk-head or a valid
    // blk-foot (the block should survive to the loss of one header)
    do
    {
        end_of_vol = false;
        valid_block = false;
        header = NULL;

        // open volume if there is no current volume open
        if (ai->archfd == -1)
        {
            msgprintf(MSG_VERB2, "Opening volume %.3d: [%s]\n", (int)ai->curvol, ai->volpath);
            if ((res = archio_open_read(ai)) != 0)
            {
                msgprintf(MSG_STACK, "archio_open_read() failed\n");
                return res;
            }
        }

        if ((buffer == NULL) || (datsize == 0))
        {
            return FSAERR_SUCCESS;
        }

        if ((curpos = lseek64(ai->archfd, 0, SEEK_CUR)) < 0)
        {
            sysprintf("lseek64() failed to get the current position in archive\n");
            return FSAERR_SEEK;
        }

        // read data from file
        memset(blkbuf, 0, sizeof(blkbuf));
        res = archio_read_low_level(ai, blkbuf, totalsize);
        if ((res != FSAERR_SUCCESS) && (res != FSAERR_ENDOFFILE))
        {
            errprintf("archio_read_low_level(): failed to read archive\n");
            return FSAERR_READ;
        }

        // check if headers are valid
        valid_header1 = ((le32_to_cpu(header1->magic) == FSA_MAGIC_IOPHYSHEADER)
            && (le32_to_cpu(header1->archid) == ai->archid)
            && (le32_to_cpu(header1->csum) == fletcher32((u8 *)&header1->data, sizeof(header1->data))));
        valid_header2 = ((le32_to_cpu(header2->magic) == FSA_MAGIC_IOPHYSHEADER)
            && (le32_to_cpu(header2->archid) == ai->archid)
            && (le32_to_cpu(header2->csum) == fletcher32((u8 *)&header2->data, sizeof(header2->data))));

        // splitting: either proper "end of volume" marker found or unexpected end of current volume
        if ((valid_header1 == true) && (le16_to_cpu(header1->type) == IOHEAD_VOLFOOT))
        {
            end_of_vol = true;
        }
        else if (res == FSAERR_ENDOFFILE)
        {
            errprintf("archio_read_low_level(): unexpected end of volume %s (curblock=%lld)\n", ai->volpath, (long long)ai->curblock);
            end_of_vol = true;
        }

        if (end_of_vol == true)
        {
            archio_close_read(ai);
            if (ai->lastvol == true)
            {
                return FSAERR_ENDOFFILE;
            }
            else
            {
                archio_incvolume(ai);
                continue;
            }
        }

        // accept block if only one header valid so that corruption of one
        // block header does not mean we lose one entire block of data
        if ((valid_header1 == true) && (le16_to_cpu(header1->type) == IOHEAD_BLKHEAD))
        {
            valid_block = true;
            header = header1;
        }
        else if ((valid_header2 == true) && (le16_to_cpu(header2->type) == IOHEAD_BLKFOOT))
        {
            valid_block = true;
            header = header2;
        }
        else
        {
            valid_block = false;
            if (lseek64(ai->archfd, curpos+1, SEEK_SET) < 0)
            {
                sysprintf("lseek64(pos=%lld, SEEK_SET) failed\n", (long long)curpos);
                return FSAERR_SEEK;
            }
            bytesignored++;
        }
    }
    while (valid_block == false);

    if (buffer != NULL)
    {
        memcpy(buffer, blkbuf + sizeof(ciohead), datsize);
    }

    if (bytesignored > 0)
    {
        errprintf("skipped %lld bytes of data to find a valid data block in volume %s\n", (long long)bytesignored, ai->volpath);
    }

    if (bytesused != NULL)
    {
        *bytesused = le32_to_cpu(header->data.blkhead.bytesused);
    }

    if ((volpathbuf != NULL) && (volpathbufsize > 0))
    {
        snprintf(volpathbuf, volpathbufsize, "%s", ai->volpath);
    }

    return FSAERR_SUCCESS;
}

int archio_read_low_level(carchio *ai, void *data, u32 bufsize)
{
    long lres;

    errno = 0;
    if ((lres = read(ai->archfd, (char*)data, (long)bufsize)) != (long)bufsize)
    {
        if ((lres >= 0) && (lres < (long)bufsize))
        {
            //sysprintf("read(size=%ld) failed: unexpected end of archive volume: lres=%ld\n", (long)bufsize, lres);
            return FSAERR_ENDOFFILE;
        }
        else
        {
            sysprintf("read(size=%ld) failed: lres=%ld\n", (long)bufsize, lres);
            return FSAERR_READ;
        }
    }

    return FSAERR_SUCCESS;
}

int archio_write_low_level(carchio *ai, char *buffer, u32 bufsize)
{
    struct statvfs64 statvfsbuf;
    char textbuf[128];
    long lres;

    errno = 0;
    if ((lres = write(ai->archfd, buffer, bufsize)) != bufsize)
    {
        errprintf("write(size=%ld) returned %ld\n", (long)bufsize, (long)lres);
        if ((lres >= 0) && (lres < (long)bufsize)) // probably "no space left"
        {
            if (fstatvfs64(ai->archfd, &statvfsbuf) == 0)
            {
                u64 freebytes = statvfsbuf.f_bfree * statvfsbuf.f_bsize;
                errprintf("Cannot write to the archive file. Space on device is %s. \n"
                    "If the archive is being written to a FAT filesystem, you may have reached \n"
                    "the maximum filesize that it can handle (in general 2 GB)\n", 
                    format_size(freebytes, textbuf, sizeof(textbuf), 'h'));
                return FSAERR_ENOSPC;
            }
        }
        else // another error
        {
            sysprintf("write(size=%ld) failed\n", (long)bufsize);
        }
        return FSAERR_WRITE;
    }

    return FSAERR_SUCCESS;
}

int archio_split_check(carchio *ai, u32 size)
{
    s64 cursize = 0;

    if ((ai->archfd >= 0) && ((cursize = archio_get_currentpos(ai)) >= 0) && (g_options.splitsize > 0) && (cursize + size + sizeof(ciohead) > g_options.splitsize))
    {
        msgprintf(MSG_DEBUG2, "splitchk: YES --> archfd=%d, cursize=%lld, g_options.splitsize=%lld, cursize+size=%lld, wb->size=%lld\n",
            (int)ai->archfd, (long long)cursize, (long long)g_options.splitsize, (long long)cursize+size, (long long)size);
        return true;
    }
    else
    {
        msgprintf(MSG_DEBUG2, "splitchk: NO --> archfd=%d, cursize=%lld, g_options.splitsize=%lld, cursize+wb->size=%lld, wb->size=%lld\n",
            (int)ai->archfd, (long long)cursize, (long long)g_options.splitsize, (long long)cursize+size, (long long)size);
        return false;
    }
}
