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

#ifndef __FSARCHIVER_H__
#define __FSARCHIVER_H__

#include "types.h"

// ----------------------------------- min and max -----------------------------
#if !defined(min)
#    define min(a, b)            ((a) < (b) ? (a) : (b))
#endif

#if !defined(max)
#    define max(a, b)            ((a) > (b) ? (a) : (b))
#endif

// ----------------------------------- fsarchiver commands ------------------------------------------
enum {OPER_NULL=0, OPER_SAVEFS, OPER_RESTFS, OPER_SAVEDIR, OPER_RESTDIR, 
      OPER_ARCHINFO, OPER_PROBE, OPER_SAVEPT, OPER_RESTPT, OPER_SHOWPT};

// ----------------------------------- fsarchiver archive format ------------------------------------
enum {FSA_FMT_NULL=0, FSA_FMT_06=1, FSA_FMT_07=2};

// ----------------------------------- fsarchiver status --------------------------------------------
enum {STATUS_RUNNING,   // the program is running normally
      STATUS_ABORTED,   // the user pressed Ctrl+C and it should stop now
      STATUS_FAILED,    // one thread failed so it should stop now
      STATUS_FINISHED}; // the main thread has received what it needs, should stop

// ----------------------------------- dico sections ------------------------------------------------
enum {DICO_OBJ_SECTION_STDATTR=0, DICO_OBJ_SECTION_XATTR=1, DICO_OBJ_SECTION_WINATTR=2};

// ----------------------------------- archive types ------------------------------------------------
enum {ARCHTYPE_NULL=0, ARCHTYPE_FILESYSTEMS, ARCHTYPE_DIRECTORIES};

// ----------------------------------- volume header and footer -------------------------------------
enum {VOLUMEHEADKEY_VOLNUM, VOLUMEHEADKEY_ARCHID, VOLUMEHEADKEY_FILEFORMATVER, VOLUMEHEADKEY_PROGVERCREAT};
enum {VOLUMEFOOTKEY_VOLNUM, VOLUMEFOOTKEY_ARCHID, VOLUMEFOOTKEY_LASTVOL};

// ----------------------------------- algorithms used to process data-------------------------------
enum {COMPRESS_NULL=0, COMPRESS_NONE, COMPRESS_LZO, COMPRESS_GZIP, COMPRESS_BZIP2, COMPRESS_LZMA};
enum {ENCRYPT_NULL=0, ENCRYPT_NONE, ENCRYPT_BLOWFISH};

// ----------------------------------- dico keys ----------------------------------------------------
enum {OBJTYPE_NULL=0, OBJTYPE_DIR, OBJTYPE_SYMLINK, OBJTYPE_HARDLINK, OBJTYPE_CHARDEV, 
      OBJTYPE_BLOCKDEV, OBJTYPE_FIFO, OBJTYPE_SOCKET, OBJTYPE_REGFILEUNIQUE, OBJTYPE_REGFILEMULTI};

enum {DISKITEMKEY_NULL=0, DISKITEMKEY_OBJECTID, DISKITEMKEY_PATH, DISKITEMKEY_OBJTYPE, 
      DISKITEMKEY_SYMLINK, DISKITEMKEY_HARDLINK, DISKITEMKEY_RDEV, DISKITEMKEY_MODE, 
      DISKITEMKEY_SIZE, DISKITEMKEY_UID, DISKITEMKEY_GID, DISKITEMKEY_ATIME, DISKITEMKEY_MTIME,
      DISKITEMKEY_MD5SUM, DISKITEMKEY_MULTIFILESCOUNT, DISKITEMKEY_MULTIFILESOFFSET,
      DISKITEMKEY_LINKTARGETTYPE, DISKITEMKEY_FLAGS};

enum {BLOCKHEADITEMKEY_NULL=0, BLOCKHEADITEMKEY_REALSIZE, BLOCKHEADITEMKEY_BLOCKOFFSET, 
      BLOCKHEADITEMKEY_COMPRESSALGO, BLOCKHEADITEMKEY_ENCRYPTALGO, BLOCKHEADITEMKEY_ARSIZE, 
      BLOCKHEADITEMKEY_COMPSIZE, BLOCKHEADITEMKEY_ARCSUM};

enum {BLOCKFOOTITEMKEY_NULL=0, BLOCKFOOTITEMKEY_MD5SUM};

enum {MAINHEADKEY_NULL=0, MAINHEADKEY_FILEFORMATVER, MAINHEADKEY_PROGVERCREAT, MAINHEADKEY_ARCHIVEID, 
      MAINHEADKEY_CREATTIME, MAINHEADKEY_ARCHLABEL, MAINHEADKEY_ARCHTYPE, MAINHEADKEY_FSCOUNT, 
      MAINHEADKEY_COMPRESSALGO, MAINHEADKEY_COMPRESSLEVEL, MAINHEADKEY_ENCRYPTALGO, 
      MAINHEADKEY_BUFCHECKPASSCLEARMD5, MAINHEADKEY_BUFCHECKPASSCRYPTBUF, MAINHEADKEY_FSACOMPLEVEL,
      MAINHEADKEY_MINFSAVERSION, MAINHEADKEY_HASDIRSINFOHEAD};

enum {LAYOUTHEADKEY_NULL=0, LAYOUTHEADKEY_PTCOUNT};

enum {LAYOUTHEADSEC_STD=0, LAYOUTHEADSEC_PARTTABLE=1};

enum {FSYSHEADKEY_NULL=0, FSYSHEADKEY_FILESYSTEM, FSYSHEADKEY_MNTPATH, FSYSHEADKEY_BYTESTOTAL, 
      FSYSHEADKEY_BYTESUSED, FSYSHEADKEY_FSLABEL, FSYSHEADKEY_FSUUID, FSYSHEADKEY_FSINODESIZE, 
      FSYSHEADKEY_FSVERSION, FSYSHEADKEY_FSEXTDEFMNTOPT, FSYSHEADKEY_FSEXTREVISION, 
      FSYSHEADKEY_FSEXTBLOCKSIZE, FSYSHEADKEY_FSEXTFEATURECOMPAT, FSYSHEADKEY_FSEXTFEATUREINCOMPAT,
      FSYSHEADKEY_FSEXTFEATUREROCOMPAT, FSYSHEADKEY_FSREISERBLOCKSIZE, FSYSHEADKEY_FSREISER4BLOCKSIZE, 
      FSYSHEADKEY_FSXFSBLOCKSIZE, FSYSHEADKEY_FSBTRFSSECTORSIZE, FSYSHEADKEY_NTFSSECTORSIZE,
      FSYSHEADKEY_NTFSCLUSTERSIZE, FSYSHEADKEY_BTRFSFEATURECOMPAT, FSYSHEADKEY_BTRFSFEATUREINCOMPAT,
      FSYSHEADKEY_BTRFSFEATUREROCOMPAT, FSYSHEADKEY_NTFSUUID, FSYSHEADKEY_MINFSAVERSION,
      FSYSHEADKEY_MOUNTINFO, FSYSHEADKEY_ORIGDEV, FSYSHEADKEY_TOTALCOST,
      FSYSHEADKEY_FSEXTFSCKMAXMNTCOUNT, FSYSHEADKEY_FSEXTFSCKCHECKINTERVAL, 
      FSYSHEADKEY_FSEXTEOPTRAIDSTRIPEWIDTH, FSYSHEADKEY_FSEXTEOPTRAIDSTRIDE};

enum {DIRSINFOKEY_NULL=0, DIRSINFOKEY_TOTALCOST};

// -------------------------------- fsarchiver errors ---------------------------------------------
enum {FSAERR_SUCCESS=0,           // success
      FSAERR_UNKNOWN=-1,          // uknown error (default code that means error)
      FSAERR_ENOMEM=-2,           // out of memory error
      FSAERR_EINVAL=-3,           // invalid parameter
      FSAERR_ENOENT=-4,           // entry not found
      FSAERR_ENDOFFILE=-5,        // end of file/queue
      FSAERR_WRONGTYPE=-6,        // wrong type of data
      FSAERR_NOTOPEN=-7,          // resource has been closed
      FSAERR_ENOSPC=-8,           // no space left on device
      FSAERR_SEEK=-9,             // lseek64 error
      FSAERR_READ=-10,            // read error
      FSAERR_WRITE=-11,           // write error
      FSAERR_CORRUPT=-12,         // archive is corrupt
      FSAERR_WRONGVOL=-13,        // wrong volume
      FSAERR_WRONGVER=-14,        // wrong version
      FSAERR_WRONGARCH=-15,       // wrong archive
      FSAERR_OPEN=-16,            // file cannot be open
      FSAERR_EXISTS=-17,          // file already exists
      FSAERR_STAT=-18,            // cannot get file info
};

// ----------------------------- fsarchiver const ------------------------
#define FSA_VERSION              PACKAGE_VERSION
#define FSA_RELDATE              PACKAGE_RELDATE
#define FSA_FILEFORMAT           PACKAGE_FILEFMT

#define FSA_CONFIG_FILE          "/etc/fsarchiver.conf"

#define FSA_GCRYPT_VERSION       "1.2.3"

#define FSA_MAX_FILEFMTLEN       32
#define FSA_MAX_PROGVERLEN       32
#define FSA_MAX_FSNAMELEN        128
#define FSA_MAX_DEVLEN           256
#define FSA_MAX_UUIDLEN          128
#define FSA_MAX_BLKDEVICES       256

#define FSA_MAX_SMALLFILECOUNT   512            // there can be up to FSA_MAX_SMALLFILECOUNT files copied in a single data block 
#define FSA_MAX_SMALLFILESIZE    131072         // files smaller than that will be grouped with other small files in a single data block

#define FSA_MAX_FSPERARCH        128
#define FSA_MAX_PTPERARCH        128
#define FSA_MAX_COMPJOBS         32
#define FSA_MAX_QUEUESIZE        32
#define FSA_MAX_BLKSIZE          921600
#define FSA_DEF_BLKSIZE          262144
#define FSA_DEF_ECCLEVEL         1              // default ecc level: add one extra FEC packet (N = K + 1) by default
#define FSA_MIN_ECCLEVEL         0              // minimum ecc level: add no extra FEC packets: N = K
#define FSA_MAX_ECCLEVEL         16             // minimum ecc level: add 16 extra FEC packets: N = K + 16
#define FSA_DEF_COMPRESS_ALGO    COMPRESS_GZIP  // compress using gzip by default
#define FSA_DEF_COMPRESS_LEVEL   6              // compress with "gzip -6" by default
#define FSA_COST_PER_FILE        16384          // how much it cost to copy an empty file/dir/link: used to eval the progress bar

#define FSA_FEC_IOBUFSIZE        128            // how many Forward-Error-Correction blocks (1 block = K chunks) in the iobuffer
#define FSA_FEC_PACKET_SIZE      4096           // size of a packet passed to the Forward-Error-Correction algorithm
#define FSA_FEC_VALUE_K          16             // number of raw chunks passed to the Forward-Error-Correction algorithm
#define FSA_FEC_MAXVAL_N         (FSA_FEC_VALUE_K + FSA_MAX_ECCLEVEL) // maximum possible value for the N number in the FEC algorithm

#define FSA_MAX_LABELLEN         512
#define FSA_MIN_PASSLEN          6
#define FSA_MAX_PASSLEN          64

#define FSA_FILESYSID_NULL       0xFFFF
#define FSA_CHECKPASSBUF_SIZE    4096
#define FSA_MAINHEAD_PADDING     65535          // size of the padding used to make the main header bigger (to escape corruptions)
#define FSA_MAINHEAD_COPIES      3              // how many copies of the main header do we want to write in the archive
#define FSA_FILEFLAGS_SPARSE     1<<0           // set when a regfile is a sparse file

// ----------------------------- fsarchiver logical header types -------------------------------------
#define FSA_HEADTYPE_MAIN        ((u32)0x68437241) // archive header (at the beginning of the first volume)
#define FSA_HEADTYPE_PADG        ((u32)0x67446150) // padding used to separate different copies of the same header
#define FSA_HEADTYPE_DILA        ((u32)0x614C6944) // disk layout header (at the beginning of the first volume)
#define FSA_HEADTYPE_FSIN        ((u32)0x6E497346) // filesys info (one per filesystem at the beginning of the archive)
#define FSA_HEADTYPE_FSYB        ((u32)0x73597346) // filesys begin (one per filesystem when the filesys contents start)
#define FSA_HEADTYPE_DIRS        ((u32)0x73526944) // dirs info (one per archive after mainhead before flat dirs/files)
#define FSA_HEADTYPE_OBJT        ((u32)0x744A624F) // object header (one per object: regfiles, dirs, symlinks, ...)
#define FSA_HEADTYPE_BLKH        ((u32)0x684B6C42) // datablk header (one per data block, each regfile may have [0-n])
#define FSA_HEADTYPE_FILF        ((u32)0x664C6946) // filedat footer (one per regfile, after the list of data blocks)
#define FSA_HEADTYPE_DATF        ((u32)0x6E456144) // data footer (one per file system, at the end of its contents, or after the contents of the flatfiles)

// ----------------------------- fsarchiver magic numbers --------------------------------------------
#define FSA_MAGIC_IOPHYSHEADER   ((u32)0x31417346) // written in each physical-header ("FsA1" in little-endian)
#define FSA_MAGIC_LOGICHEADER1   ((u32)0x31486C46) // starts each logical-header ("FlH1" in little-endian)
#define FSA_MAGIC_LOGICHEADER2   ((u32)0x32486C46) // finishes each logical-header ("FlH2" in little-endian)

// ----------------------------- global variables ----------------------------------------------------
extern u32 g_valid_header_types[];
extern char g_archive[];
extern int g_archver;

// -------------------------------- version_number to u64 --------------------------------------------
#define FSA_VERSION_BUILD(a, b, c, d)     ((u64)((((u64)a&0xFFFF)<<48)+(((u64)b&0xFFFF)<<32)+(((u64)c&0xFFFF)<<16)+(((u64)d&0xFFFF)<<0)))
#define FSA_VERSION_GET_A(ver)            ((((u64)ver)>>48)&0xFFFF)
#define FSA_VERSION_GET_B(ver)            ((((u64)ver)>>32)&0xFFFF)
#define FSA_VERSION_GET_C(ver)            ((((u64)ver)>>16)&0xFFFF)
#define FSA_VERSION_GET_D(ver)            ((((u64)ver)>>0)&0xFFFF)

#endif // __FSARCHIVER_H__
