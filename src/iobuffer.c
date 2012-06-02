/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2008-2012 Francois Dupoux.  All rights reserved.
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
#include <assert.h>
#include <string.h>
#include <errno.h>

#include "fsarchiver.h"
#include "iobuffer.h"
#include "common.h"
#include "queue.h"
#include "error.h"
#include "dico.h"

struct s_logicheader;
typedef struct s_logicheader clogicheader;

struct __attribute__ ((__packed__)) s_logicheader
{
    u32 magic1; // always set to FSA_MAGIC_LOGICHEADER1
    u32 headtype; // type of header (one number per usage)
    u16 fsindex; // index of the current filesystem
    u16 dicocnt; // how many items in the dico
    u32 dicolen; // length of all items of the dico
    u32 dicosum; // 32 bit checksum of the dico
    u32 magic2; // always set to FSA_MAGIC_LOGICHEADER2
};

struct timespec iobuffer_get_timeout()
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_sec++;
    return t;
}

ciobuffer *iobuffer_alloc(u32 blocks_maxcnt, u32 blksize)
{
    ciobuffer *iob;
    pthread_mutexattr_t attr;

    if ((iob=calloc(1, sizeof(ciobuffer))) == NULL)
    {
        return NULL;
    }

    iob->blocks_maxcnt=blocks_maxcnt;
    iob->blocks_curcnt=0;
    iob->blocks_size = blksize;
    assert(pthread_mutexattr_init(&attr)==0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK)==0);
    assert(pthread_mutex_init(&iob->mutex, &attr)==0);
    assert(pthread_cond_init(&iob->cond,NULL)==0);

    return iob;
}

u32 iobuffer_get_block_size(ciobuffer *iob)
{
    return iob->blocks_size;
}

int iobuffer_destroy(ciobuffer *iob)
{
    if (iob != NULL)
    {
        assert(pthread_mutex_lock(&iob->mutex)==0);
        assert(pthread_mutex_unlock(&iob->mutex)==0);
        assert(pthread_mutex_destroy(&iob->mutex)==0);
        assert(pthread_cond_destroy(&iob->cond)==0);
        free(iob);
    }
    return FSAERR_SUCCESS;
}

// true if eof flag set and no more block to read (no locking, for internal use only)
bool iobuffer_get_end_of_buffer_locked(ciobuffer *iob)
{
    return ((iob->blocks_curcnt == 0) && (iob->endofbuffer == true));
}

// true if eof flag set and no more block to read
bool iobuffer_get_end_of_buffer(ciobuffer *iob)
{
    bool res;

    assert(pthread_mutex_lock(&iob->mutex) == 0);
    res = (iob->blocks_curcnt == 0) && (iob->endofbuffer == true);
    assert(pthread_mutex_unlock(&iob->mutex) == 0);

    return res;
}

int iobuffer_set_end_of_buffer(ciobuffer *iob, bool state)
{
    cioblock *cur;

    assert(pthread_mutex_lock(&iob->mutex)==0);

    // mark the last block as full so that the reader can get it
    for (cur = iob->list_head; (cur != NULL) && (cur->next != NULL); cur = cur->next);
    if (cur != NULL)
    {
        cur->eof = true;
    }

    // set the EOF flag
    iob->endofbuffer = state;

    assert(pthread_mutex_unlock(&iob->mutex)==0);
    pthread_cond_broadcast(&iob->cond);

    return FSAERR_SUCCESS;
}

// write an entire FEC block made of K * 1KB returned by fec_decode()
int iobuffer_write_fec_block(ciobuffer *iob, char *buffer, u32 bufsize, u32 bytesused)
{
    cioblock *item = NULL;
    cioblock *cur = NULL;

    if (bufsize != iob->blocks_size)
    {
        errprintf("bufsize does not match blocks_size\n");
        assert(pthread_mutex_unlock(&iob->mutex)==0);
        return FSAERR_UNKNOWN;
    }

    assert(pthread_mutex_lock(&iob->mutex)==0);

    // does not make sense to add item on a queue where endofqueue is true
    if (iob->endofbuffer == true)
    {
        assert(pthread_mutex_unlock(&iob->mutex)==0);
        return FSAERR_ENDOFFILE;
    }

    // allocate new block
    if ((item = calloc(1, sizeof(cioblock))) == NULL)
    {
        errprintf("calloc(%ld) failed: out of memory\n", (long)sizeof(cioblock));
        assert(pthread_mutex_unlock(&iob->mutex)==0);
        return FSAERR_ENOMEM;
    }

    if ((item->data = calloc(1, iob->blocks_size)) == NULL)
    {
        free(item);
        errprintf("calloc(%ld) failed: out of memory\n", (long)iob->blocks_size);
        assert(pthread_mutex_unlock(&iob->mutex)==0);
        return FSAERR_ENOMEM;
    }
    memcpy(item->data, buffer, iob->blocks_size);
    item->bytes_used = bytesused;
    item->bytes_ptr = 0;

    // wait while (queue-is-full) to let the other threads remove blocks first
    while (iob->blocks_curcnt >= iob->blocks_maxcnt)
    {
        struct timespec t=iobuffer_get_timeout();
        pthread_cond_timedwait(&iob->cond, &iob->mutex, &t);
    }

    // add new block at the end of the queue
    if (iob->list_head == NULL) // if list empty: item is head
    {
        iob->list_head = item;
    }
    else // list not empty: add items at the end
    {
        for (cur = iob->list_head; (cur != NULL) && (cur->next != NULL); cur = cur->next);
        cur->next=item;
    }
    iob->blocks_curcnt++;

    assert(pthread_mutex_unlock(&iob->mutex)==0);
    pthread_cond_broadcast(&iob->cond);

    return FSAERR_SUCCESS;
}

// read an entire FEC block made of K * 1KB ready for fec_encode()
int iobuffer_read_fec_block(ciobuffer *iob, char *buffer, u32 bufsize, u32 *bytesused)
{
    cioblock *cur;
    bool eof;
    int res;

    assert(pthread_mutex_lock(&iob->mutex) == 0);

    if (bufsize != iob->blocks_size)
    {
        errprintf("bufsize does not match blocks_size\n");
        assert(pthread_mutex_unlock(&iob->mutex)==0);
        return FSAERR_UNKNOWN;
    }

    while (iobuffer_get_end_of_buffer_locked(iob) == false)
    {
        // use first block (head) if it exists and it is full
        if ((iob->blocks_curcnt > 0) && ((cur = iob->list_head) != NULL) && ((cur->bytes_used == iob->blocks_size) || (cur->eof == true)))
        {
            iob->list_head = cur->next;
            iob->blocks_curcnt--;
            memcpy(buffer, cur->data, iob->blocks_size);
            *bytesused = cur->bytes_used;
            free(cur->data);
            free(cur);
            assert(pthread_mutex_unlock(&iob->mutex)==0);
            pthread_cond_broadcast(&iob->cond);
            return FSAERR_SUCCESS;
        }
        else // wait if the first block is not ready
        {
            struct timespec t=iobuffer_get_timeout();
            if ((res = pthread_cond_timedwait(&iob->cond, &iob->mutex, &t))!=0 && res != ETIMEDOUT)
            {
                assert(pthread_mutex_unlock(&iob->mutex)==0);
                return FSAERR_UNKNOWN;
            }
        }
    }

    eof = iobuffer_get_end_of_buffer_locked(iob);
    assert(pthread_mutex_unlock(&iob->mutex)==0);
    return (eof == true) ? FSAERR_ENDOFFILE : FSAERR_UNKNOWN;
}

// write a buffer with raw bytes to populate the list of blocks
int iobuffer_write_raw_data(ciobuffer *iob, char *buffer, u32 datasize)
{
    cioblock *item = NULL;
    cioblock *cur = NULL;
    u32 spaceleft;
    u32 cursize;

    while (datasize > 0)
    {
        assert(pthread_mutex_lock(&iob->mutex)==0);

        // get pointer to the tail of the linked list
        for (cur = iob->list_head; (cur != NULL) && (cur->next != NULL); cur = cur->next);

        // allocate new block if necessary
        if ((cur == NULL) || (cur->bytes_used == iob->blocks_size))
        {
            // does not make sense to add item on a queue where endofqueue is true
            if (iob->endofbuffer == true)
            {
                assert(pthread_mutex_unlock(&iob->mutex)==0);
                return FSAERR_ENDOFFILE;
            }

            // wait while (queue-is-full) to let the other threads remove blocks first
            while (iob->blocks_curcnt >= iob->blocks_maxcnt)
            {
                //printf("iobuffer_write_raw_data waiting (blocks_curcnt=%ld, blocks_maxcnt=%ld\n", (long)iob->blocks_curcnt, (long)iob->blocks_maxcnt);
                struct timespec t=iobuffer_get_timeout();
                pthread_cond_timedwait(&iob->cond, &iob->mutex, &t);
            }

            // allocate new block
            if ((item = calloc(1, sizeof(cioblock))) == NULL)
            {
                errprintf("calloc(%ld) failed: out of memory\n", (long)sizeof(cioblock));
                assert(pthread_mutex_unlock(&iob->mutex)==0);
                return FSAERR_ENOMEM;
            }

            if ((item->data = calloc(1, iob->blocks_size)) == NULL)
            {
                free(item);
                errprintf("calloc(%ld) failed: out of memory\n", (long)iob->blocks_size);
                assert(pthread_mutex_unlock(&iob->mutex)==0);
                return FSAERR_ENOMEM;
            }
            
            // add new block at the end of the queue
            if (iob->list_head == NULL) // if list empty: item is head
            {
                iob->list_head = item;
            }
            else // list not empty: add items at the end
            {
                for (cur = iob->list_head; (cur != NULL) && (cur->next != NULL); cur = cur->next);
                cur->next=item;
            }
            iob->blocks_curcnt++;
            cur = item;
        }

        // check how much space left in the current block
        spaceleft = iob->blocks_size - cur->bytes_used;
        cursize = min(datasize, spaceleft);
        memcpy(cur->data + cur->bytes_used, buffer, cursize);
        cur->bytes_used += cursize;
        cur->bytes_ptr += cursize;
        buffer += cursize;
        datasize -= cursize;

        assert(pthread_mutex_unlock(&iob->mutex)==0);
        pthread_cond_broadcast(&iob->cond);
    }

    return FSAERR_SUCCESS;
}

int iobuffer_write_block(ciobuffer *iob, struct s_blockinfo *blkinfo, u16 fsid)
{
    cdico *blkdico; // header written in file
    int res;

    if ((blkdico=dico_alloc())==NULL)
    {
        errprintf("dico_alloc() failed\n");
        return FSAERR_ENOMEM;
    }

    if (blkinfo->blkarsize==0)
    {
        errprintf("blkinfo->blkarsize=0: block is empty\n");
        return FSAERR_EINVAL;
    }

    // prepare header
    dico_add_u64(blkdico, 0, BLOCKHEADITEMKEY_BLOCKOFFSET, blkinfo->blkoffset);
    dico_add_u32(blkdico, 0, BLOCKHEADITEMKEY_REALSIZE, blkinfo->blkrealsize);
    dico_add_u32(blkdico, 0, BLOCKHEADITEMKEY_ARSIZE, blkinfo->blkarsize);
    dico_add_u32(blkdico, 0, BLOCKHEADITEMKEY_COMPSIZE, blkinfo->blkcompsize);
    dico_add_u32(blkdico, 0, BLOCKHEADITEMKEY_ARCSUM, blkinfo->blkarcsum);
    dico_add_u16(blkdico, 0, BLOCKHEADITEMKEY_COMPRESSALGO, blkinfo->blkcompalgo);
    dico_add_u16(blkdico, 0, BLOCKHEADITEMKEY_ENCRYPTALGO, blkinfo->blkcryptalgo);

    // write block header
    res = iobuffer_write_logichead(iob, blkdico, FSA_HEADTYPE_BLKH, fsid);
    dico_destroy(blkdico);
    if (res!=0)
    {
        msgprintf(MSG_STACK, "cannot write FSA_HTYPE_BLKH block-header\n");
        return -1;
    }

    // write block data
    if (iobuffer_write_raw_data(iob, blkinfo->blkdata, blkinfo->blkarsize)!=0)
    {
        msgprintf(MSG_STACK, "cannot write data block: writebuf_add_data() failed\n");
        return -1;
    }

    return 0;
}

int iobuffer_write_logichead(ciobuffer *iob, cdico *d, u32 headertype, u16 fsindex)
{
    struct s_dicoitem *item;
    clogicheader header;
    u32 dicolen = 0;
    u16 dicocnt = 0;
    u16 itemnum = 0;
    u32 dicosum;
    u8 *buffer;
    u8 *bufpos;
    u16 temp16;

    // calculate length of dico
    for (dicocnt = dicolen = 0, item = d->head; item != NULL; item = item->next, dicocnt++)
    {
        dicolen += sizeof(u8);  // data type
        dicolen += sizeof(u8);  // section num
        dicolen += sizeof(u16); // key num
        dicolen += sizeof(u16); // data size
        dicolen += item->size;  // data
    }
    msgprintf(MSG_DEBUG2, "dico info: dicolen=%ld and dicocnt=%ld\n", (long)dicolen, (long)dicocnt);

    // allocate memory for dico
    if ((bufpos = buffer = malloc(dicolen)) == NULL)
    {
        errprintf("cannot allocate memory for dico");
        return FSAERR_ENOMEM;
    }

    // dump all dico items in buffer
    for (item = d->head, itemnum = 0; (item != NULL) && (itemnum < dicocnt); item = item->next, itemnum++)
    {
        msgprintf(MSG_DEBUG2, "itemnum=%d (type=%d, section=%d, key=%d, size=%d)\n", 
            (int)itemnum, (int)item->type, (int)item->section, (int)item->key, (int)item->size);

        // a. write data type buffer
        bufpos = mempcpy(bufpos, &item->type, sizeof(item->type));

        // b. write section to buffer
        bufpos = mempcpy(bufpos, &item->section, sizeof(item->section));

        // c. write key to buffer
        temp16 = cpu_to_le16(item->key);
        bufpos = mempcpy(bufpos, &temp16, sizeof(temp16));

        // d. write sizeof(data) to buffer
        temp16 = cpu_to_le16(item->size);
        bufpos = mempcpy(bufpos, &temp16, sizeof(temp16));

        // e. write data to buffer
        if (item->size > 0)
            bufpos = mempcpy(bufpos, item->data, item->size);
    }
    msgprintf(MSG_DEBUG2, "all %d items mempcopied to buffer\n", (int)itemnum);

    // prepare header
    dicosum = fletcher32(buffer, dicolen);
    memset(&header, 0, sizeof(header));
    header.magic1 = cpu_to_le32(FSA_MAGIC_LOGICHEADER1);
    header.headtype = cpu_to_le32(headertype);
    header.fsindex = cpu_to_le16(fsindex);
    header.dicocnt = cpu_to_le16(dicocnt);
    header.dicolen = cpu_to_le32(dicolen);
    header.dicosum = cpu_to_le32(dicosum);
    header.magic2 = cpu_to_le32(FSA_MAGIC_LOGICHEADER2);

    // write header
    if (iobuffer_write_raw_data(iob, (char *)&header, sizeof(header)) != 0)
    {
        errprintf("iobuffer_write_raw_data() failed to write logical-header head\n");
        free(buffer);
        return -1;
    }

    // write dico
    if (iobuffer_write_raw_data(iob, (char *)buffer, dicolen) != 0)
    {
        errprintf("iobuffer_write_raw_data() failed to write logical-header dico\n");
        free(buffer);
        return -1;
    }

    free(buffer);

    return FSAERR_SUCCESS;
}

// read a buffer of raw bytes to from the list of blocks
int iobuffer_read_raw_data(ciobuffer *iob, char *buffer, u32 datasize)
{
    cioblock *cur = NULL;
    u32 dataleft;
    u32 cursize;
    int res;

    while (datasize > 0)
    {
        assert(pthread_mutex_lock(&iob->mutex)==0);

        // fail if EOF marker is set and no block in the list
        if (iobuffer_get_end_of_buffer_locked(iob) == true)
        {
            assert(pthread_mutex_unlock(&iob->mutex)==0);
            return FSAERR_ENDOFFILE;
        }

        // use first block (head) if it exists
        if ((iob->blocks_curcnt > 0) && (iob->list_head != NULL))
        {
            // read data from first block of the list
            cur = iob->list_head;
            dataleft = cur->bytes_used - cur->bytes_ptr;
            cursize = min(datasize, dataleft);
            memcpy(buffer, cur->data + cur->bytes_ptr, cursize);
            cur->bytes_ptr += cursize;
            buffer += cursize;
            datasize -= cursize;

            // remove block if no data left
            if (cur->bytes_ptr == cur->bytes_used)
            {
                iob->list_head = cur->next;
                iob->blocks_curcnt--;
                free(cur->data);
                free(cur);
            }
        }
        else // wait if the first block is not ready
        {
            struct timespec t = iobuffer_get_timeout();
            if ((res = pthread_cond_timedwait(&iob->cond, &iob->mutex, &t))!=0 && res != ETIMEDOUT)
            {
                assert(pthread_mutex_unlock(&iob->mutex)==0);
                return FSAERR_UNKNOWN;
            }
        }
        
        assert(pthread_mutex_unlock(&iob->mutex)==0);
    }

    return FSAERR_SUCCESS;
}

int iobuffer_read_block(ciobuffer *iob, struct s_dico *in_blkdico, int *out_sumok, struct s_blockinfo *out_blkinfo)
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

    memset(out_blkinfo, 0, sizeof(struct s_blockinfo));
    *out_sumok=-1;

    if (dico_get_u64(in_blkdico, 0, BLOCKHEADITEMKEY_BLOCKOFFSET, &blockoffset) != 0)
    {
        msgprintf(3, "cannot get blockoffset from block-header\n");
        return -1;
    }

    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_REALSIZE, &curblocksize)!=0 || curblocksize>FSA_MAX_BLKSIZE)
    {
        msgprintf(3, "cannot get blocksize from block-header\n");
        return -1;
    }

    if (dico_get_u16(in_blkdico, 0, BLOCKHEADITEMKEY_COMPRESSALGO, &compalgo) != 0)
    {
        msgprintf(3, "cannot get BLOCKHEADITEMKEY_COMPRESSALGO from block-header\n");
        return -1;
    }

    if (dico_get_u16(in_blkdico, 0, BLOCKHEADITEMKEY_ENCRYPTALGO, &cryptalgo) != 0)
    {
        msgprintf(3, "cannot get BLOCKHEADITEMKEY_ENCRYPTALGO from block-header\n");
        return -1;
    }

    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_ARSIZE, &finalsize) != 0)
    {
        msgprintf(3, "cannot get BLOCKHEADITEMKEY_ARSIZE from block-header\n");
        return -1;
    }

    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_COMPSIZE, &compsize) != 0)
    {
        msgprintf(3, "cannot get BLOCKHEADITEMKEY_COMPSIZE from block-header\n");
        return -1;
    }

    if (dico_get_u32(in_blkdico, 0, BLOCKHEADITEMKEY_ARCSUM, &arblockcsumorig) != 0)
    {
        msgprintf(3, "cannot get BLOCKHEADITEMKEY_ARCSUM from block-header\n");
        return -1;
    }

    if ((buffer = malloc(finalsize)) == NULL)
    {
        errprintf("cannot allocate block: malloc(%d) failed\n", finalsize);
        return FSAERR_ENOMEM;
    }

    if (iobuffer_read_raw_data(iob, (char *)buffer, finalsize) != 0)
    {
        free(buffer);
        return FSAERR_READ;
    }

    out_blkinfo->blkdata=(char*)buffer;
    out_blkinfo->blkrealsize=curblocksize;
    out_blkinfo->blkoffset=blockoffset;
    out_blkinfo->blkarcsum=arblockcsumorig;
    out_blkinfo->blkcompalgo=compalgo;
    out_blkinfo->blkcryptalgo=cryptalgo;
    out_blkinfo->blkarsize=finalsize;
    out_blkinfo->blkcompsize=compsize;

    arblockcsumcalc = fletcher32(buffer, finalsize);
    if (arblockcsumcalc != arblockcsumorig) // bad checksum
    {
        errprintf("block is corrupt at offset=%ld, blksize=%ld\n", (long)blockoffset, (long)curblocksize);
        free(out_blkinfo->blkdata);
        if ((out_blkinfo->blkdata = malloc(curblocksize)) == NULL)
        {   errprintf("cannot allocate block: malloc(%d) failed\n", curblocksize);
            return FSAERR_ENOMEM;
        }
        memset(out_blkinfo->blkdata, 0, curblocksize);
        *out_sumok = false;
    }
    else // no corruption detected
    {
        *out_sumok=true;
    }

    return 0;
}

int iobuffer_read_logichead(ciobuffer *iob, u32 *headertype, struct s_dico **d, u16 *fsindex)
{
    char readbuf[sizeof(clogicheader)];
    clogicheader *header = NULL;
    u64 bytesignored = 0;
    u8 *buffer = NULL;
    bool valid;
    u32 dicolen;
    u16 dicocnt;
    u8 *bufpos;
    u16 temp16;
    u8 section;
    u16 size;
    u8 type;
    u16 key;
    int res;
    int i;

    // init
    header = (clogicheader *)readbuf;
    *headertype = 0;
    *fsindex = FSA_FILESYSID_NULL;
    *d = NULL;

    if ((*d = dico_alloc()) == NULL)
    {
        errprintf("dico_alloc() failed\n");
        return FSAERR_ENOMEM;
    }

    // retry until we read a valid logical-header (skip all corruptions)
    do
    {
        // attempt to read logical-header header entirely from archive
        if ((res = iobuffer_read_raw_data(iob, readbuf, sizeof(readbuf))) != FSAERR_SUCCESS)
        {
            return FSAERR_READ;
        }

        // move to next byte in the archive until we find a valid header
        // (valid magic1 and magic2). two magic numbers are required so that we know
        // if some rubbish has been added in the middle of the header. In that case we
        // do not want to consider that  rubbish and use it as if it was valid header
        while ((le32_to_cpu(header->magic1) != FSA_MAGIC_LOGICHEADER1) || (le32_to_cpu(header->magic2) != FSA_MAGIC_LOGICHEADER2))
        {
            memcpy(readbuf, readbuf+1, sizeof(readbuf)-1); // ignore first byte we have read previously
            if ((res = iobuffer_read_raw_data(iob, &readbuf[sizeof(readbuf)-1], 1)) != FSAERR_SUCCESS) // read next byte
            {
                msgprintf(MSG_STACK, "iobuffer_read_raw_data() failed to read logical-header header: res=%d\n", res);
                return FSAERR_READ;
            }
            bytesignored++;
        }
        if (bytesignored > 0)
        {
            errprintf("ignored %ld bytes to find the next logical-header\n", (long)bytesignored);
        }

        // read important values from the header
        dicolen = le32_to_cpu(header->dicolen);
        dicocnt = le16_to_cpu(header->dicocnt);

        // allocate memory for the dico
        if ((bufpos = buffer = malloc(dicolen)) == NULL)
        {
            errprintf("cannot allocate memory for header\n");
            return FSAERR_ENOMEM;
        }

        // read dico items in buffer
        if ((res = iobuffer_read_raw_data(iob, (char *)buffer, dicolen)) != 0)
        {
            errprintf("cannot read header data: iobuffer_read_raw_data()=%d\n", res);
            free(buffer);
            return FSAERR_READ;
        }

        // check the dico integrity
        valid = fletcher32(buffer, dicolen) == le32_to_cpu(header->dicosum);
        if (valid == false)
        {
            errprintf("current logical-header is invalid: ignoring it\n");
            free(buffer);
        }
    }
    while (valid == false);

    // write header info
    *headertype = le32_to_cpu(header->headtype);
    *fsindex = le16_to_cpu(header->fsindex);

    // read items
    for (i = 0; i < dicocnt; i++)
    {
        // a. read type from buffer
        memcpy(&type, bufpos, sizeof(type));
        bufpos += sizeof(section);

        // b. read section from buffer
        memcpy(&section, bufpos, sizeof(section));
        bufpos += sizeof(section);

        // c. read key from buffer
        memcpy(&temp16, bufpos, sizeof(temp16));
        bufpos += sizeof(temp16);
        key = le16_to_cpu(temp16);

        // d. read sizeof(data)
        memcpy(&temp16, bufpos, sizeof(temp16));
        bufpos += sizeof(temp16);
        size = le16_to_cpu(temp16);

        // e. add item to dico
        if ((res = dico_add_generic(*d, section, key, bufpos, size, type)) != 0)
        {
            free(buffer);
            return res;
        }
        bufpos += size;
    }

    free(buffer);

    return FSAERR_SUCCESS;
}
