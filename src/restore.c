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

#include <unistd.h>
#include <fcntl.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include <attr/xattr.h>
#include <sys/time.h>
#include <sys/stat.h>
#include <gcrypt.h>
#include <limits.h>

#include "fsarchiver.h"
#include "strdico.h"
#include "dico.h"
#include "common.h"
#include "options.h"
#include "restore.h"
#include "archinfo.h"
#include "filesys.h"
#include "fs_ext2.h"
#include "fs_reiserfs.h"
#include "fs_reiser4.h"
#include "fs_btrfs.h"
#include "fs_xfs.h"
#include "fs_jfs.h"
#include "fs_ntfs.h"
#include "thread_comp.h"
#include "thread_queue2iobuf.h"
#include "thread_iobuf2archio.h"
#include "thread_compat06.h"
#include "layout_rest.h"
#include "syncthread.h"
#include "regmulti.h"
#include "crypto.h"
#include "error.h"
#include "datafile.h"
#include "queue.h"

typedef struct s_extractar
{
    int         fsid;
    cstats      stats;
    u64         cost_global;
    u64         cost_current;
}
cextractar;

// returns true if this file of a parent directory has been excluded
int is_filedir_excluded(char *relpath)
{
    char dirpath[PATH_MAX];
    char basename[PATH_MAX];
    int pos;
    
    // check if that particular file has been excluded
    extract_basename(relpath, basename, sizeof(basename));
    
    if ((exclude_check(&g_options.exclude, basename)==true) // is filename excluded ?
        || (exclude_check(&g_options.exclude, relpath)==true)) // is filepath excluded ?
    {
        msgprintf(MSG_VERB2, "file/dir=[%s] excluded because of its own name/path\n", relpath);
        return true;
    }
    
    // check if that file belongs to a directory which has been excluded
    snprintf(dirpath, sizeof(dirpath), "%s", relpath);
    for (pos=0; dirpath[pos]; pos++); // go to the end of the string
    while (pos>0)
    {
        // dirpath=parent_directory(dirpath)
        while ((pos>=0) && (dirpath[pos]!='/'))
            dirpath[pos--]=0;
        if ((pos>0) && (dirpath[pos]=='/'))
            dirpath[pos]=0;
        extract_basename(dirpath, basename, sizeof(basename));
        
        if (strlen(dirpath)>1 && strlen(basename)>0)
        {
            if ((exclude_check(&g_options.exclude, basename)==true)
                || (exclude_check(&g_options.exclude, dirpath)==true))
            {
                msgprintf(MSG_VERB2, "file/dir=[%s] excluded because of its parent=[%s]\n", relpath, dirpath);
                return true; // a parent directory is excluded
            }
        }
    }
    
    return false; // no exclusion found for that file
}

// convert an array of strings "id=x,dest=/dev/xxx,..." to an array of strdico
int convert_argv_to_strdicos(cstrdico *dicoargv[], int argc, char *cmdargv[], char *possiblekeys)
{
    cstrdico *tmpdico=NULL;
    char buffer[1024];
    struct stat64 st;
    s64 temp64;
    int fsid;
    int i;
    
    for (i=0; (i<argc) && (i < FSA_MAX_FSPERARCH) && (cmdargv[i]!=NULL); i++)
    {
        if ((tmpdico=strdico_alloc())==NULL)
            return -1;
        
        // parse argument and write (key,value) pairs in the strdico object
        if ((strdico_set_valid_keys(tmpdico, possiblekeys)!=0) ||
            (strdico_parse_string(tmpdico, cmdargv[i])!=0))
        {
            strdico_destroy(tmpdico);
            return -1;
        }
        
        // read and check "id=" key in the argument
        if (strdico_get_s64(tmpdico, &temp64, "id")!=0)
        {
            errprintf("cannot find \"id=\" key in \"%s\"\n", cmdargv[i]);
            strdico_destroy(tmpdico);
            return -1;
        }
        fsid=temp64;
        if ((fsid<0) || (fsid>FSA_MAX_FSPERARCH-1))
        {
            errprintf("invalid filesystem id [%d]: it must match a valid filesystem id as shown by archinfo\n", fsid);
            strdico_destroy(tmpdico);
            return -1;
        }
        
        // read and check "dest=" key in the argument
        if (strdico_get_string(tmpdico, buffer, sizeof(buffer), "dest")!=0)
        {
            errprintf("cannot find \"dest=\" key in \"%s\"\n", cmdargv[i]);
            strdico_destroy(tmpdico);
            return -1;
        }
        if ((stat64(buffer, &st)!=0) || (!S_ISBLK(st.st_mode)))
        {
            errprintf("\"%s\" is not a valid block device\n", buffer);
            strdico_destroy(tmpdico);
            return -1;
        }
        
        // add the current argument to the list of the strdico objects
        if (dicoargv[fsid]!=NULL)
        {
            errprintf("you mentioned filesystem with id=%d multiple times, cannot continue\n", fsid);
            strdico_destroy(tmpdico);
            return -1;
        }
        dicoargv[fsid]=tmpdico;
    }
    
    return 0;
}

int extractar_listing_print_file(cextractar *exar, int objtype, char *relpath)
{
    char strprogress[256];
    u64 progress;
    
    memset(strprogress, 0, sizeof(strprogress));
    if (exar->cost_global>0)
    {
        progress=(((exar->cost_current)*100)/(exar->cost_global));
        if (progress>=0 && progress<=100)
            snprintf(strprogress, sizeof(strprogress), "[%3d%%]", (int)progress);
    }
    msgprintf(MSG_VERB1, "-[%.2d]%s[%s] %s\n", exar->fsid, strprogress, get_objtype_name(objtype), relpath);
    return 0;
}

int extractar_restore_attr_xattr(cextractar *exar, u32 objtype, char *fullpath, char *relpath, cdico *dicoattr)
{
    char xattrname[2048];
    char xattrvalue[65535];
    u16 xattrdatasize;
    int xattrdicsize;
    int ret=0;
    int res;
    int i;
    
    // ---- restore extended attributes
    xattrdicsize=dico_count_one_section(dicoattr, DICO_OBJ_SECTION_XATTR);
    
    for (i=0; i < xattrdicsize; i+=2)
    {
        if (dico_get_string(dicoattr, DICO_OBJ_SECTION_XATTR, (u64)(i+0), xattrname, sizeof(xattrname))!=0)
        {
            errprintf("Cannot retrieve the name of an xattr for file %s: DICO_OBJ_SECTION_XATTR, key=%ld\n", relpath, (long)(i+0));
            dico_show(dicoattr, DICO_OBJ_SECTION_XATTR, "xattr");
            ret=-1;
            continue;
        }
        memset(xattrvalue, 0, sizeof(xattrvalue));
        if (dico_get_data(dicoattr, DICO_OBJ_SECTION_XATTR, (u64)(i+1), xattrvalue, sizeof(xattrvalue), &xattrdatasize)!=0)
        {
            errprintf("Cannot retrieve the value of an xattr for file %s: DICO_OBJ_SECTION_XATTR, key=%ld\n", relpath, (long)(i+1));
            dico_show(dicoattr, DICO_OBJ_SECTION_XATTR, "xattr");
            ret=-1;
            continue;
        }
        
        if ((res=lsetxattr(fullpath, xattrname, xattrvalue, xattrdatasize, 0))!=0)
        {
            sysprintf("xattr:lsetxattr(%s,%s) failed\n", relpath, xattrname);
            ret=-1;
        }
        else // success
        {
            msgprintf(MSG_VERB2, "            xattr:lsetxattr(%s, %s)=%d\n", relpath, xattrname, res);
        }
    }
    
    return ret;
}

int extractar_restore_attr_windows(cextractar *exar, u32 objtype, char *fullpath, char *relpath, cdico *dicoattr)
{
    char xattrname[2048];
    char xattrvalue[65535];
    u16 xattrdatasize;
    int xattrdicsize;
    int ret=0;
    int res;
    int i;
    
    xattrdicsize=dico_count_one_section(dicoattr, DICO_OBJ_SECTION_WINATTR);
    
    for (i=0; i < xattrdicsize; i+=2)
    {
        if (dico_get_string(dicoattr, DICO_OBJ_SECTION_WINATTR, (u64)(i+0), xattrname, sizeof(xattrname))!=0)
        {
            errprintf("Cannot retrieve the name of an winattr for file %s\n", relpath);
            dico_show(dicoattr, DICO_OBJ_SECTION_WINATTR, "winattr");
            ret=-1;
            continue;
        }
        memset(xattrvalue, 0, sizeof(xattrvalue));
        if (dico_get_data(dicoattr, DICO_OBJ_SECTION_WINATTR, (u64)(i+1), xattrvalue, sizeof(xattrvalue), &xattrdatasize)!=0)
        {
            errprintf("Cannot retrieve the value of an winattr for file %s\n", relpath);
            ret=-1;
            continue;
        }
        
        if ((res=lsetxattr(fullpath, xattrname, xattrvalue, xattrdatasize, 0))!=0)
        {
            sysprintf("winattr:lsetxattr(%s,%s) failed\n", relpath, xattrname);
            ret=-1;
        }
        else // success
        {
            msgprintf(MSG_VERB2, "            winattr:lsetxattr(%s, %s)=%d\n", relpath, xattrname, res);
        }
    }
    
    return ret;
}

int extractar_restore_attr_std(cextractar *exar, u32 objtype, char *fullpath, char *relpath, cdico *dicoattr)
{
    u32 mode, uid, gid;
    u64 atime, mtime;
    
    // ---- restore standard attributes (permissions, owner, ...)
    if (dico_get_u32(dicoattr, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_MODE, &mode)!=0)
        return -1;
    if (dico_get_u32(dicoattr, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_UID, &uid)!=0)
        return -2;
    if (dico_get_u32(dicoattr, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_GID, &gid)!=0)
        return -3;
    if (dico_get_u64(dicoattr, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_ATIME, &atime)!=0)
        return -4;
    if (dico_get_u64(dicoattr, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_MTIME, &mtime)!=0)
        return -5;
    
    if (lchown(fullpath, (uid_t)uid, (gid_t)gid)!=0)
    {
        sysprintf("Cannot lchown(%s) which is %s\n", fullpath, get_objtype_name(objtype));
        return -6;
    }
    
    if (objtype!=OBJTYPE_SYMLINK)
    {
        if (chmod(fullpath, (mode_t)mode)!=0)
        {
            sysprintf("chmod(%s, %lld) failed\n", fullpath, (long long)mode);
            return -7;
        }
    }
    
    // set the values for atime/mtime
    struct timeval tv[2];
    tv[0].tv_usec=0;
    tv[0].tv_sec=atime;
    tv[1].tv_usec=0;
    tv[1].tv_sec=mtime;
    if (objtype!=OBJTYPE_SYMLINK) // not a symlink
    {
        if (utimes(fullpath, tv)!=0)
        {
            sysprintf("utimes(%s) failed\n", relpath);
            return -8;
        }
    }
#ifdef HAVE_LUTIMES
    else // object is a symlink
    {
        // lutimes not implemented on rhel5: don't fail for symlinks
        lutimes(fullpath, tv);
    }
#endif // HAVE_LUTIMES
    
    return 0;
}

int extractar_restore_attr_everything(cextractar *exar, int objtype, char *fullpath, char *relpath, cdico *dicoattr)
{
    int res=0;
    
    // ---- restore standard attributes
    res+=extractar_restore_attr_std(exar, objtype, fullpath, relpath, dicoattr);
    
    // ---- restore extended attributes
    res+=extractar_restore_attr_xattr(exar, objtype, fullpath, relpath, dicoattr);
    
    // ---- restore windows attributes
    res+=extractar_restore_attr_windows(exar, objtype, fullpath, relpath, dicoattr);
    
    return (res==0)?(0):(-1);
}

int extractar_restore_obj_symlink(cextractar *exar, char *fullpath, char *relpath, char *destdir, cdico *d, int objtype, int fstype)
{
    char parentdir[PATH_MAX];
    struct timeval tv[2];
    char buffer[PATH_MAX];
    u64 targettype;
    int fdtemp;
    
    // update cost statistics and progress bar
    exar->cost_current+=FSA_COST_PER_FILE; 
    
    // check the list of excluded files/dirs
    if (is_filedir_excluded(relpath)==true)
        goto extractar_restore_obj_symlink_err;
    
    // update progress bar
    extractar_listing_print_file(exar, objtype, relpath);

    // create parent directory first
    extract_dirpath(fullpath, parentdir, sizeof(parentdir));
    mkdir_recursive(parentdir);
    
    // backup parent dir atime/mtime
    get_parent_dir_time_attrib(fullpath, parentdir, sizeof(parentdir), tv);
    
    if (dico_get_string(d, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_SYMLINK, buffer, PATH_MAX)<0)
    {
        errprintf("Cannot read field=symlink for file=[%s]\n", fullpath);
        goto extractar_restore_obj_symlink_err;
    }
    
    // in ntfs a symlink has to be recreated as a standard file or directory (depending on what the target is)
    if ((dico_get_u64(d, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_LINKTARGETTYPE, &targettype)==0)
         && (strcmp(filesys[fstype].name, "ntfs")==0))
    {
        switch (targettype)
        {
            case OBJTYPE_DIR:
                msgprintf(MSG_DEBUG1, "LINK: mklink=[%s], target=[%s], targettype=DIR\n", relpath, buffer);
                if (mkdir_recursive(fullpath)!=0)
                {
                    errprintf("Cannot create directory for ntfs symlink: path=[%s]\n", fullpath);
                    goto extractar_restore_obj_symlink_err;
                }
                break;
            case OBJTYPE_REGFILEUNIQUE:
                msgprintf(MSG_DEBUG1, "LINK: mklink=[%s], target=[%s], targettype=REGFILE\n", relpath, buffer);
                if ( (fdtemp=creat(fullpath, 0644)) < 0)
                {
                    errprintf("Cannot create file for ntfs symlink: path=[%s]\n", fullpath);
                    goto extractar_restore_obj_symlink_err;
                }
                close(fdtemp);
                break;
            default:
                msgprintf(MSG_DEBUG1, "LINK: mklink=[%s], target=[%s], targettype=UNKNOWN\n", relpath, buffer);
                errprintf("Unexpected target type for ntfs symlink: path=[%s]\n", fullpath);
                goto extractar_restore_obj_symlink_err;
                break;            
        }
    }
    else // normal symbolic link for linux filesystems
    {
        msgprintf(MSG_DEBUG1, "LINK: symlink=[%s], target=[%s] (normal symlink)\n", relpath, buffer);
        if (symlink(buffer, fullpath)<0)
        {
            sysprintf("symlink(%s, %s) failed\n", buffer, fullpath);
            goto extractar_restore_obj_symlink_err;
        }
    }
    
    if (extractar_restore_attr_everything(exar, objtype, fullpath, relpath, d)!=0)
    {
        msgprintf(MSG_STACK, "cannot restore file attributes for file [%s]\n", relpath);
        goto extractar_restore_obj_symlink_err;
    }
    
    // restore parent dir mtime/atime
    if (utimes(parentdir, tv)!=0)
    {
        sysprintf("utimes(%s) failed\n", parentdir);
        goto extractar_restore_obj_symlink_err;
    }
    
    dico_destroy(d);
    exar->stats.cnt_symlink++;
    return 0; // success
    
extractar_restore_obj_symlink_err:
    dico_destroy(d);
    exar->stats.err_symlink++;
    return 0; // non fatal error
}

int extractar_restore_obj_hardlink(cextractar *exar, char *fullpath, char *relpath, char *destdir, cdico *d, int objtype, int fstype)
{
    char parentdir[PATH_MAX];
    struct timeval tv[2];
    char buffer[PATH_MAX];
    char regfile[PATH_MAX];
    int res;
    
    // update cost statistics and progress bar
    exar->cost_current+=FSA_COST_PER_FILE; 
    
    // check the list of excluded files/dirs
    if (is_filedir_excluded(relpath)==true)
        goto extractar_restore_obj_hardlink_err;
    
    // create parent directory first
    extract_dirpath(fullpath, parentdir, sizeof(parentdir));
    mkdir_recursive(parentdir);
    
    // backup parent dir atime/mtime
    get_parent_dir_time_attrib(fullpath, parentdir, sizeof(parentdir), tv);
    
    // update progress bar
    extractar_listing_print_file(exar, objtype, relpath);
    
    if (dico_get_string(d, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_HARDLINK, buffer, PATH_MAX)<0)
    {
        msgprintf(MSG_STACK, "dico_get_string(DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_HARDLINK) failed\n");
        goto extractar_restore_obj_hardlink_err;
    }
    
    concatenate_paths(regfile, PATH_MAX, destdir, buffer);
    
    if ((res=link(regfile, fullpath))!=0)
    {
        sysprintf("link(%s, %s) failed\n", regfile, fullpath);
        goto extractar_restore_obj_hardlink_err;
    }
    
    if (extractar_restore_attr_everything(exar, objtype, fullpath, relpath, d)!=0)
    {
        msgprintf(MSG_STACK, "cannot restore file attributes for file [%s]\n", relpath);
        goto extractar_restore_obj_hardlink_err;
    }
    
    // restore parent dir mtime/atime
    if (utimes(parentdir, tv)!=0)
    {
        sysprintf("utimes(%s) failed\n", parentdir);
        goto extractar_restore_obj_hardlink_err;
    }
    
    dico_destroy(d);
    exar->stats.cnt_hardlink++;
    return 0; // success

extractar_restore_obj_hardlink_err:
    dico_destroy(d);
    exar->stats.err_hardlink++;
    return 0; // non fatal error
}

int extractar_restore_obj_devfile(cextractar *exar, char *fullpath, char *relpath, char *destdir, cdico *d, int objtype, int fstype)
{
    char parentdir[PATH_MAX];
    struct timeval tv[2];
    u64 dev;
    u32 mode;
    
    // update cost statistics and progress bar
    exar->cost_current+=FSA_COST_PER_FILE; 
    
    // check the list of excluded files/dirs
    if (is_filedir_excluded(relpath)==true)
        goto extractar_restore_obj_devfile_err;
    
    // create parent directory first
    extract_dirpath(fullpath, parentdir, sizeof(parentdir));
    mkdir_recursive(parentdir);
    
    // backup parent dir atime/mtime
    get_parent_dir_time_attrib(fullpath, parentdir, sizeof(parentdir), tv);
    
    // update progress bar
    extractar_listing_print_file(exar, objtype, relpath);

    if (dico_get_u64(d, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_RDEV, &dev)!=0)
        goto extractar_restore_obj_devfile_err;
    if (dico_get_u32(d, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_MODE, &mode)!=0)
        goto extractar_restore_obj_devfile_err;
    if (mknod(fullpath, mode, dev)!=0)
    {
        sysprintf("mknod failed on [%s]\n", relpath);
        goto extractar_restore_obj_devfile_err;
    }
    if (extractar_restore_attr_everything(exar, objtype, fullpath, relpath, d)!=0)
    {
        msgprintf(MSG_STACK, "cannot restore file attributes for file [%s]\n", relpath);
        goto extractar_restore_obj_devfile_err;
    }
    
    // restore parent dir mtime/atime
    if (utimes(parentdir, tv)!=0)
    {
        sysprintf("utimes(%s) failed\n", parentdir);
        goto extractar_restore_obj_devfile_err;
    }
    
    dico_destroy(d);
    exar->stats.cnt_special++;
    return 0; // success

extractar_restore_obj_devfile_err:
    dico_destroy(d);
    exar->stats.err_special++;
    return 0; // non fatal error
}

int extractar_restore_obj_directory(cextractar *exar, char *fullpath, char *relpath, char *destdir, cdico *d, int objtype, int fstype)
{
    char parentdir[PATH_MAX];
    struct timeval tv[2];
    
    // update cost statistics and progress bar
    exar->cost_current+=FSA_COST_PER_FILE; 
    
    // check the list of excluded files/dirs
    if (is_filedir_excluded(relpath)==true)
        goto extractar_restore_obj_directory_err;
    
    // create parent directory first
    extract_dirpath(fullpath, parentdir, sizeof(parentdir));
    mkdir_recursive(parentdir);

    // backup parent dir atime/mtime
    get_parent_dir_time_attrib(fullpath, parentdir, sizeof(parentdir), tv);
    
    // update progress bar
    extractar_listing_print_file(exar, objtype, relpath);

    mkdir_recursive(fullpath);
    
    if (extractar_restore_attr_everything(exar, objtype, fullpath, relpath, d)!=0)
    {
        msgprintf(MSG_STACK, "cannot restore file attributes for file [%s]\n", relpath);
        goto extractar_restore_obj_directory_err;
    }
    
    // restore parent dir mtime/atime
    if (utimes(parentdir, tv)!=0)
    {
        sysprintf("utimes(%s) failed\n", parentdir);
        goto extractar_restore_obj_directory_err;
    }
    
    dico_destroy(d);
    exar->stats.cnt_dir++;
    return 0; // success

extractar_restore_obj_directory_err:
    dico_destroy(d);
    exar->stats.err_dir++;
    return 0; // non fatal error
}

int extractar_restore_obj_regfile_multi(cextractar *exar, char *destdir, cdico *dicofirstfile, int objtype, int fstype) // d = obj-header of first small file
{
    cdatafile *datafile=NULL;
    char databuf[FSA_MAX_SMALLFILESIZE];
    char basename[PATH_MAX];
    cdico *filehead=NULL;
    char fullpath[PATH_MAX];
    char relpath[PATH_MAX];
    char parentdir[PATH_MAX];
    struct timeval tv[2];
    struct s_blockinfo blkinfo;
    cregmulti regmulti;
    u8 md5sumcalc[16];
    u8 md5sumorig[16];
    int errors;
    u32 headertype;
    u32 filescount;
    u32 tmpobjtype;
    u64 datsize;
    s64 lres;
    int res;
    int i;
    
    // init
    errors=0;
    memset(&blkinfo, 0, sizeof(blkinfo));
    regmulti_init(&regmulti, FSA_MAX_BLKSIZE);
    datafile=datafile_alloc();
    
    // ---- dequeue header for each small file which is part of that group
    if (dico_get_u32(dicofirstfile, 0, DISKITEMKEY_MULTIFILESCOUNT, &filescount)!=0)
    {
        errprintf("cannot read DISKITEMKEY_MULTIFILESCOUNT from header in archive\n");
        return -1;
    }
    if (regmulti_rest_addheader(&regmulti, dicofirstfile)!=0)
    {
        errprintf("rest_addheader() failed\n");
        return -1;
    }
    
    for (i=1; i < filescount; i++) // first header was a special case (received from calling function)
    {
        if (queue_dequeue_header(g_queue, &filehead, &headertype, NULL)<=0)
        {
            errprintf("queue_dequeue_header() failed: cannot read multireg object header\n");
            errors++;
            return -1;
        }
        if (headertype != FSA_HEADTYPE_OBJT)
        {
            errprintf("header is not what we expected: found=[%ld] and expected=[%ld]\n", (long)headertype, (long)FSA_HEADTYPE_OBJT);
            return -1;
        }
        if (regmulti_rest_addheader(&regmulti, filehead)!=0)
        {
            errprintf("rest_addheader() failed for file %d\n", i);
            return -1;
        }
    }
    
    // ---- dequeue the block which contains data for several small files
    if ((lres=queue_dequeue_block(g_queue, &blkinfo))<=0)
    {
        errprintf("queue_dequeue_block()=%ld=%s failed\n", (long)lres, error_int_to_string(lres));
        return -1;
    }
    
    if (regmulti_rest_setdatablock(&regmulti, blkinfo.blkdata, blkinfo.blkrealsize)!=0)
    {
        errprintf("regmulti_rest_setdatablock() failed\n");
        return -1;
    }
    free(blkinfo.blkdata); // free memory allocated by the thread_io_reader
    
    // ---- create the set of small files using the regmulti structure
    for (i=0; i < filescount; i++)
    {
        // get header and data for a small file from the regmulti structure
        if (regmulti_rest_getfile(&regmulti, i, &filehead, databuf, &datsize, sizeof(databuf))!=0)
        {
            errprintf("rest_addheader() failed for file %d\n", i);
            filehead=NULL; // else dico_destroy would fail
            goto extractar_restore_obj_regfile_multi_err;
        }
        
        if (dico_get_u32(filehead, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_OBJTYPE, &tmpobjtype)!=0)
        {
            errprintf("Cannot read object type\n");
            goto extractar_restore_obj_regfile_multi_err;
        }
        
        if ((res=dico_get_data(filehead, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_PATH, relpath, sizeof(relpath), NULL))!=0)
        {
            errprintf("Cannot read DISKITEMKEY_PATH from header, res=%d, key=%d\n", res, DISKITEMKEY_PATH);
            dico_show(filehead, DICO_OBJ_SECTION_STDATTR, "DISKITEMKEY_PATH");
            goto extractar_restore_obj_regfile_multi_err;
        }
        concatenate_paths(fullpath, sizeof(fullpath), destdir, relpath);
        extract_basename(fullpath, basename, sizeof(basename));
        
        // update cost statistics and progress bar
        exar->cost_current+=FSA_COST_PER_FILE; 
        exar->cost_current+=datsize; // filesize
        
        // check the list of excluded files/dirs
        if (is_filedir_excluded(relpath)!=true)
        {
            // create parent directory if necessary
            extract_dirpath(fullpath, parentdir, sizeof(parentdir));
            mkdir_recursive(parentdir);
            
            // backup parent dir atime/mtime
            get_parent_dir_time_attrib(fullpath, parentdir, sizeof(parentdir), tv);
            
            extractar_listing_print_file(exar, tmpobjtype, relpath);
            
            if (dico_get_data(filehead, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_MD5SUM, md5sumorig, 16, NULL))
            {
                errprintf("cannot get md5sum from file footer for file=[%s]\n", relpath);
                dico_show(filehead, DICO_OBJ_SECTION_STDATTR, "filehead");
                goto extractar_restore_obj_regfile_multi_err;
            }
            
            if (datafile_open_write(datafile, fullpath, false, false)<0)
                goto extractar_restore_obj_regfile_multi_err;
            
            res=datafile_write(datafile, databuf, datsize);
            
            datafile_close(datafile, md5sumcalc, sizeof(md5sumcalc));
            
            if (res!=FSAERR_SUCCESS)
            {
                errprintf("removing %s\n", fullpath);
                unlink(fullpath);
                return -1;
            }
            
            if (memcmp(md5sumcalc, md5sumorig, 16)!=0)
            {
                errprintf("cannot restore file %s, the data block (which is shared by multiple files) is corrupt\n", relpath);
                res=truncate(fullpath, 0); // don't leave corrupt data in the file
                goto extractar_restore_obj_regfile_multi_err;
            }
                      
            if (extractar_restore_attr_everything(exar, objtype, fullpath, relpath, filehead)!=0)
            {
                msgprintf(MSG_STACK, "cannot restore file attributes for file [%s]\n", relpath);
                goto extractar_restore_obj_regfile_multi_err;
            }
            
            // restore parent dir mtime/atime
            if (utimes(parentdir, tv)!=0)
            {
                sysprintf("utimes(%s) failed\n", parentdir);
                goto extractar_restore_obj_regfile_multi_err;
            }
            exar->stats.cnt_regfile++;
        }
        
        dico_destroy(filehead);
        continue; // success on that file
        
extractar_restore_obj_regfile_multi_err:
        dico_destroy(filehead);
        exar->stats.err_regfile++;
        continue;
    }
    
    datafile_destroy(datafile);
    return 0;
}

int extractar_restore_obj_regfile_unique(cextractar *exar, char *fullpath, char *relpath, char *destdir, cdico *d, int objtype, int fstype) // large or empty files
{
    struct s_blockinfo blkinfo;
    char parentdir[PATH_MAX];
    cdatafile *datafile=NULL;
    cdico *footerdico=NULL;
    bool fatalerr=false; // error for restoration globally
    bool minorerr=false; // error for current file only
    bool delfile=false;
    struct timeval tv[2];
    u8 md5sumcalc[16];
    u8 md5sumorig[16];
    int excluded=false;
    bool sparse=false;
    u32 headertype;
    u64 filesize=0;
    u64 filepos=0;
    u64 flags=0;
    s64 lres;
    
    // init
    memset(&blkinfo, 0, sizeof(blkinfo));
    datafile=datafile_alloc();
    
    if (dico_get_u64(d, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_SIZE, &filesize)!=0)
    {
        errprintf("Cannot read filesize DISKITEMKEY_SIZE from archive for file=[%s]\n", relpath);
        minorerr=true;
    }
    
    sparse=((dico_get_u64(d, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_FLAGS, &flags)==0) && (flags&FSA_FILEFLAGS_SPARSE));
    
    // update cost statistics and progress bar
    exar->cost_current+=FSA_COST_PER_FILE; 
    exar->cost_current+=filesize;
    
    // check the list of excluded files/dirs
    if (is_filedir_excluded(relpath)==true)
    {
        excluded=true;
    }
    else if (minorerr==false) // file not excluded and no error yet
    {
        // create parent directory first
        extract_dirpath(fullpath, parentdir, sizeof(parentdir));
        mkdir_recursive(parentdir);
        
        // backup parent dir atime/mtime
        get_parent_dir_time_attrib(fullpath, parentdir, sizeof(parentdir), tv);
        
        // show progress bar
        extractar_listing_print_file(exar, objtype, relpath);
    }
    
    if ((minorerr==false) && (datafile_open_write(datafile, fullpath, excluded, sparse)<0))
        minorerr=true;
    
    msgprintf(MSG_DEBUG2, "restore_obj_regfile_unique(file=%s, size=%lld)\n", relpath, (long long)filesize);
    for (filepos=0; (minorerr == false) && (filesize > 0) && (filepos < filesize) && (get_status() == STATUS_RUNNING); filepos+=blkinfo.blkrealsize)
    {
        if ((lres=queue_dequeue_block(g_queue, &blkinfo))<=0)
        {
            errprintf("queue_dequeue_block()=%ld=%s for file(%s) failed\n", (long)lres, error_int_to_string(lres), relpath);
            delfile=true;
            minorerr=true;
            break;
        }
        
        if (blkinfo.blkoffset!=filepos)
        {
            errprintf("file offset do not match for file(%s) failed: filepos=%lld, blkinfo.blkoffset=%lld, blkinfo.blkrealsize=%lld\n", 
                relpath, (long long)filepos, (long long)blkinfo.blkoffset, (long long)blkinfo.blkrealsize);
            free(blkinfo.blkdata);
            delfile=true;
            minorerr=true;
            break;
        }
        
        if (datafile_write(datafile, blkinfo.blkdata, blkinfo.blkrealsize)!=FSAERR_SUCCESS)
        {
            free(blkinfo.blkdata);
            delfile=true;
            minorerr=true;
            fatalerr=true;
            break;
        }
        
        free(blkinfo.blkdata);
    }
    
    if ((minorerr==false) && (datafile_close(datafile, md5sumcalc, sizeof(md5sumcalc))!=0))
        minorerr=true;
    
    if ((minorerr==false) && (excluded==false))
    {
        if (extractar_restore_attr_everything(exar, objtype, fullpath, relpath, d)!=0)
        {
            msgprintf(MSG_STACK, "cannot restore file attributes for file [%s]\n", relpath);
            minorerr=true;
        }
    
        // restore parent dir mtime/atime
        if (utimes(parentdir, tv)!=0)
        {
            sysprintf("utimes(%s) failed\n", parentdir);
            minorerr=true;
        }
    }
    
    // empty files have no footer (no need for a checksum)
    if ((fatalerr==false) && (filesize>0))
    {
        if (queue_dequeue_header(g_queue, &footerdico, &headertype, NULL) <= 0)
        {
            errprintf("queue_dequeue_header() failed: cannot read footer dico\n");
            minorerr=true;
            goto restore_obj_regfile_unique_end;
        }
        
        if (excluded != true)
        {
            if (headertype != FSA_HEADTYPE_FILF)
            {
                errprintf("header is not what we expected: found=[%ld] and expected=[%ld]\n", (long)headertype, (long)FSA_HEADTYPE_FILF);
                minorerr=true;
                goto restore_obj_regfile_unique_end;
            }
            
            if (dico_get_data(footerdico, 0, BLOCKFOOTITEMKEY_MD5SUM, md5sumorig, 16, NULL))
            {
                errprintf("cannot get md5sum from file footer for file=[%s]\n", relpath);
                minorerr=true;
                goto restore_obj_regfile_unique_end;
            }
            
            if (memcmp(md5sumcalc, md5sumorig, 16)!=0)
            {
                errprintf("cannot restore file %s, file is corrupt\n", relpath);
                delfile=true; // don't leave corrupt data in the file
                minorerr=true;
                goto restore_obj_regfile_unique_end;
            }
        }
    }

restore_obj_regfile_unique_end:
    if (delfile==true)
    {
        errprintf("removing %s\n", fullpath);
        unlink(fullpath);
    }
    
    if (excluded!=true)
    {
        if (minorerr==true)
            exar->stats.err_regfile++;
        else
            exar->stats.cnt_regfile++;
    }

    if (get_status() != STATUS_RUNNING)
        errprintf("operation has been interrupted\n");

    dico_destroy(footerdico);
    dico_destroy(d);
    datafile_destroy(datafile);
    return (fatalerr==false)?(0):(-1);
}

int extractar_restore_object(cextractar *exar, int *errors, char *destdir, cdico *dicoattr, int fstype)
{
    char relpath[PATH_MAX];
    char fullpath[PATH_MAX];
    u64 filesize;
    u32 objtype;
    int res;
    
    // init
    *errors=0;
    
    if (dico_get_data(dicoattr, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_PATH, relpath, sizeof(relpath), NULL)!=0)
        return -1;
    if (dico_get_u32(dicoattr, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_OBJTYPE, &objtype)!=0)
        return -2;
    if (dico_get_u64(dicoattr, DICO_OBJ_SECTION_STDATTR, DISKITEMKEY_SIZE, &filesize)!=0)
        return -3;
    concatenate_paths(fullpath, sizeof(fullpath), destdir, relpath);
    
    // ---- recreate specific object on the filesystem
    switch (objtype)
    {
        case OBJTYPE_DIR:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_DIR, path=[%s]\n", relpath);
            res=extractar_restore_obj_directory(exar, fullpath, relpath, destdir, dicoattr, objtype, fstype);
            break;
        case OBJTYPE_SYMLINK:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_SYMLINK, path=[%s]\n", relpath);
            res=extractar_restore_obj_symlink(exar, fullpath, relpath, destdir, dicoattr, objtype, fstype);
            break;
        case OBJTYPE_HARDLINK:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_HARDLINK, path=[%s]\n", relpath);
            res=extractar_restore_obj_hardlink(exar, fullpath, relpath, destdir, dicoattr, objtype, fstype);
            break;
        case OBJTYPE_CHARDEV:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_CHARDEV, path=[%s]\n", relpath);
            res=extractar_restore_obj_devfile(exar, fullpath, relpath, destdir, dicoattr, objtype, fstype);
            break;
        case OBJTYPE_BLOCKDEV:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_BLOCKDEV, path=[%s]\n", relpath);
            res=extractar_restore_obj_devfile(exar, fullpath, relpath, destdir, dicoattr, objtype, fstype);
            break;
        case OBJTYPE_FIFO:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_FIFO, path=[%s]\n", relpath);
            res=extractar_restore_obj_devfile(exar, fullpath, relpath, destdir, dicoattr, objtype, fstype);
            break;
        case OBJTYPE_SOCKET:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_SOCKET, path=[%s]\n", relpath);
            res=extractar_restore_obj_devfile(exar, fullpath, relpath, destdir, dicoattr, objtype, fstype);
            break;
        case OBJTYPE_REGFILEUNIQUE:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_REGFILEUNIQUE, path=[%s]\n", relpath);
            if ((res=extractar_restore_obj_regfile_unique(exar, fullpath, relpath, destdir, dicoattr, objtype, fstype))<0)
            {   msgprintf(MSG_STACK, "restore_obj_regfile_unique(%s) failed with res=%d\n", relpath, res);
                return -1;
            }
            break;
        case OBJTYPE_REGFILEMULTI:
            msgprintf(MSG_DEBUG2, "objtype=OBJTYPE_REGFILEMULTI, path=[%s]\n", relpath);
            if ((res=extractar_restore_obj_regfile_multi(exar, destdir, dicoattr, objtype, fstype))<0)
            {   msgprintf(MSG_STACK, "restore_obj_regfile_multi(%s) failed with res=%d\n", relpath, res);
                return -1;
            }
            break;
        default:
            errprintf("Unknown objtype %d\n", objtype);
            return -3;
    }
    if (res!=0) // value returned by restore_obj_xxx()
    {   errprintf("Restoring file=[%s], objtype=[%s] failed\n", fullpath, get_objtype_name(objtype));
        return -1;
    }
    
    return 0;
}

int extractar_extract_read_objects(cextractar *exar, int *errors, char *destdir, int fstype)
{
    cdico *dicoattr=NULL;
    int headerisend;
    int headerisobj;
    u16 checkfsid;
    u32 headertype;
    int curerr;
    int type;
    int res;

    // init
    *errors=0;

    do
    {
        // skip the garbage (just ignore everything until the next FSA_HTYPE_OBJT)
        // in case the archive is corrupt and random data has been added / removed in the archive
        do
        {
            if (queue_check_next_item(g_queue, &type, &headertype) != 0)
            {
                errprintf("queue_check_next_item() failed: cannot read object from archive\n");
                return -1;
            }

            headerisobj = (headertype == FSA_HEADTYPE_OBJT);
            headerisend = (headertype == FSA_HEADTYPE_DATF);

            if ((headerisobj != true) && (headerisend != true)) // did not find expected header: skip garbage in archive
            {
                errprintf("unexpected header found in archive, skipping it: type=%d\n", type);
                if (queue_destroy_first_item(g_queue) != 0)
                {
                    errprintf("queue_destroy_first_item() failed: cannot read object from archive\n");
                    return -1;
                }
            }
        }
        while ((headerisobj != true) && (headerisend != true));

        if (headerisobj == true) // if it's an object header
        {
            // read object header from archive
            while (queue_dequeue_header(g_queue, &dicoattr, &headertype, &checkfsid)<=0)
            {
                errprintf("queue_dequeue_header() failed\n");
                (*errors)++;
            }
            
            if (checkfsid == exar->fsid) // if filesystem-id is correct
            {
                if ((res = extractar_restore_object(exar, &curerr, destdir, dicoattr, fstype)) != 0)
                {
                    msgprintf(MSG_STACK, "restore_object() failed with res=%d\n", res);
                    return -1; // fatal error
                }
            }
            else // wrong filesystem-id
            {
                errprintf("restore_object(): object has a wrong filesystem id: found=[%d], expected=[%d]\n", checkfsid, exar->fsid);
                (*errors)++;
            }
        }
    }
    while ((headerisend != true) && (get_status() == STATUS_RUNNING));

    return 0;
}

int extractar_read_disk_layout(carchinfo *archinfo, cdico **dicolayout)
{
    u32 headertype;
    int type;

    if ((queue_check_next_item(g_queue, &type, &headertype) == 0) && (headertype == FSA_HEADTYPE_DILA))
    {

        if (queue_dequeue_header(g_queue, dicolayout, &headertype, NULL) <= 0)
        {
            errprintf("queue_dequeue_header() failed: cannot read disk-layout header\n");
            return -1;
        }

        if (headertype != FSA_HEADTYPE_DILA)
        {
            errprintf("header is not what we expected: found=[%ld] and expected=[%ld]\n", (long)headertype, (long)FSA_HEADTYPE_DILA);
            return -1;
        }

        if (dico_get_u64(*dicolayout, LAYOUTHEADSEC_STD, LAYOUTHEADKEY_PTCOUNT, &archinfo->ptcount) != 0)
            archinfo->ptcount = 0;
    }

    return 0;
}

int extractar_read_mainhead(carchinfo *archinfo, cdico **dicomainhead)
{
    u8 bufcheckclear[FSA_CHECKPASSBUF_SIZE+8];
    u8 bufcheckcrypt[FSA_CHECKPASSBUF_SIZE+8];
    bool foundmain = false;
    cdico *curdico;
    u32 headertype;
    u16 cryptbufsize;
    u8 md5sumar[16];
    u8 md5sumnew[16];
    u64 clearsize;
    int passlen;
    u32 temp32;
    int type;

    assert(dicomainhead);

    // read all copies of the main header and ignore padding headers
    while ((queue_check_next_item(g_queue, &type, &headertype) == 0) && (headertype == FSA_HEADTYPE_MAIN || headertype == FSA_HEADTYPE_PADG))
    {
        if (queue_dequeue_header(g_queue, &curdico, &headertype, NULL) <= 0)
        {
            errprintf("extractar_read_mainhead() failed: cannot read main header\n");
            return -1;
        }

        if ((headertype == FSA_HEADTYPE_MAIN) && (foundmain == false))
        {
            *dicomainhead = curdico;
            foundmain = true;
        }
        else
        {
            dico_destroy(curdico);
        }
    }

    if (foundmain == false)
    {
        errprintf("extractar_read_mainhead() failed: main header not found in archive\n");
        return -1;
    }

    if (dico_get_u32(*dicomainhead, 0, MAINHEADKEY_ARCHTYPE, &archinfo->archtype)!=0)
    {
        errprintf("cannot find MAINHEADKEY_ARCHTYPE in main-header\n");
        return -1;
    }

    if (archinfo->archtype==ARCHTYPE_FILESYSTEMS && dico_get_u64(*dicomainhead, 0, MAINHEADKEY_FSCOUNT, &archinfo->fscount)!=0)
    {
        errprintf("cannot find MAINHEADKEY_FSCOUNT in main-header\n");
        return -1;
    }

    if (dico_get_data(*dicomainhead, 0, MAINHEADKEY_FILEFORMATVER, archinfo->filefmt, FSA_MAX_FILEFMTLEN, NULL)!=0)
    {
        errprintf("cannot find MAINHEADKEY_FILEFORMATVER in main-header\n");
        return -1;
    }

    if (dico_get_data(*dicomainhead, 0, MAINHEADKEY_PROGVERCREAT, archinfo->creatver, FSA_MAX_PROGVERLEN, NULL)!=0)
    {
        errprintf("cannot find MAINHEADKEY_PROGVERCREAT in main-header\n");
        return -1;
    }

    if (dico_get_data(*dicomainhead, 0, MAINHEADKEY_ARCHLABEL, archinfo->label, FSA_MAX_LABELLEN, NULL)!=0)
    {   errprintf("cannot find MAINHEADKEY_ARCHLABEL in main-header\n");
        return -1;
    }

    if (dico_get_u32(*dicomainhead, 0, MAINHEADKEY_COMPRESSALGO, &archinfo->compalgo)!=0)
    {
        errprintf("cannot find MAINHEADKEY_COMPRESSALGO in main-header\n");
        return -1;
    }

    if (dico_get_u32(*dicomainhead, 0, MAINHEADKEY_ENCRYPTALGO, &archinfo->cryptalgo)!=0)
    {
        errprintf("cannot find MAINHEADKEY_ENCRYPTALGO in main-header\n");
        return -1;
    }

    if (dico_get_u32(*dicomainhead, 0, MAINHEADKEY_COMPRESSLEVEL, &archinfo->complevel)!=0)
    {
        errprintf("cannot find MAINHEADKEY_COMPRESSLEVEL in main-header\n");
        return -1;
    }

    if (dico_get_u32(*dicomainhead, 0, MAINHEADKEY_FSACOMPLEVEL, &archinfo->fsacomp)!=0)
    {
        errprintf("cannot find MAINHEADKEY_FSACOMPLEVEL in main-header\n");
        return -1;
    }

    if (dico_get_u64(*dicomainhead, 0, MAINHEADKEY_CREATTIME, &archinfo->creattime)!=0)
    {
        errprintf("cannot find MAINHEADKEY_CREATTIME in main-header\n");
        return -1;
    }

    // MAINHEADKEY_HASDIRSINFOHEAD has been introduced in fsarchiver-0.6.7: don't fail if missing
    if (dico_get_u32(*dicomainhead, 0, MAINHEADKEY_HASDIRSINFOHEAD, &temp32) == 0)
    {
        archinfo->hasdirsinfohead = temp32;
    }

    // check the file format. "FsArCh_001" == "FsArCh_002" = 0.6.x
    if (strcmp(archinfo->filefmt, FSA_FILEFORMAT)!=0 && strcmp(archinfo->filefmt, "FsArCh_001")!=0 && strcmp(archinfo->filefmt, "FsArCh_002")!=0)
    {
        errprintf("This archive is based on a different file format: [%s]. Cannot continue.\n", archinfo->filefmt);
        errprintf("It has been created with fsarchiver [%s], you should extrat the archive using that version.\n", archinfo->creatver);
        errprintf("The current version of the program is [%s], and it's based on format [%s]\n", FSA_VERSION, FSA_FILEFORMAT);
        return -1;
    }

    // read minimum fsarchiver version requirement
    dico_get_u64(*dicomainhead, 0, MAINHEADKEY_MINFSAVERSION, &archinfo->minfsaver);

    // if encryption is enabled, check the password is correct using the encrypted random buffer saved in the archive
    if (archinfo->cryptalgo != ENCRYPT_NONE)
    {
        memset(md5sumar, 0, sizeof(md5sumar));
        memset(md5sumnew, 0, sizeof(md5sumnew));
        if (dico_get_data(*dicomainhead, 0, MAINHEADKEY_BUFCHECKPASSCRYPTBUF, bufcheckcrypt, sizeof(bufcheckcrypt), &cryptbufsize)!=0)
        {   errprintf("cannot find MAINHEADKEY_BUFCHECKPASSCRYPTBUF in main-header\n");
            return -1;
        }
        if (dico_get_data(*dicomainhead, 0, MAINHEADKEY_BUFCHECKPASSCLEARMD5, md5sumar, sizeof(md5sumar)+99, NULL)!=0)
        {   errprintf("cannot find MAINHEADKEY_BUFCHECKPASSCLEARMD5 in main-header\n");
            return -1;
        }
        
        passlen=(g_options.encryptpass==NULL)?(0):(strlen((char*)g_options.encryptpass));
        if ((g_options.encryptpass==NULL) || (passlen<FSA_MIN_PASSLEN) || (passlen>FSA_MAX_PASSLEN))
        {   errprintf("you have to provide the password which was used to create archive, no password given on the command line\n");
            return -1;
        }
        
        if (crypto_blowfish(cryptbufsize, &clearsize, bufcheckcrypt, bufcheckclear, g_options.encryptpass, strlen((char*)g_options.encryptpass), false)==0)
            gcry_md_hash_buffer(GCRY_MD_MD5, md5sumnew, bufcheckclear, clearsize);
        
        if (memcmp(md5sumar, md5sumnew, 16)!=0)
        {   errprintf("you have to provide the password which was used to create archive, cannot decrypt the test buffer.\n");
            return -1;
        }
    }

    return 0;
}

int extractar_filesystem_extract(cextractar *exar, cdico *dicofs, cstrdico *dicocmdline)
{
    char filesystem[FSA_MAX_FSNAMELEN];
    char text[FSA_MAX_FSNAMELEN];
    char fsbuf[FSA_MAX_FSNAMELEN];
    char mountinfo[4096];
    char partition[1024];
    char mkfsoptions[1024];
    char tempbuf[1024];
    cdico *dicobegin=NULL;
    cdico *dicoend=NULL;
    char mntbuf[PATH_MAX];
    u64 fsbytestotal;
    u64 fsbytesused;
    u32 headertype;
    char optbuf[128];
    int readwrite;
    int errors=0;
    u64 minver;
    u64 curver;
    int fstype;
    int ret=0;
    int res;
    
    // init
    memset(partition, 0, sizeof(partition));
    
    // read destination partition from dicocmdline
    if (strdico_get_string(dicocmdline, partition, sizeof(partition), "dest") != 0)
    {
        errprintf("strdico_get_string(dicocmdline, 'dest') failed\n");
        return -1;
    }
    
    // check that the minimum fsarchiver version required is ok
    if (dico_get_u64(dicofs, 0, FSYSHEADKEY_MINFSAVERSION, &minver) != 0)
    {
        errprintf("dico_get_u64(FSYSHEADKEY_MINFSAVERSION) failed\n");
        return -1;
    }
    curver=FSA_VERSION_BUILD(PACKAGE_VERSION_A, PACKAGE_VERSION_B, PACKAGE_VERSION_C, PACKAGE_VERSION_D);
    msgprintf(MSG_VERB2, "Current fsarchiver version: %d.%d.%d.%d\n", (int)FSA_VERSION_GET_A(curver), 
        (int)FSA_VERSION_GET_B(curver), (int)FSA_VERSION_GET_C(curver), (int)FSA_VERSION_GET_D(curver));
    msgprintf(MSG_VERB2, "Minimum fsarchiver version for that filesystem: %d.%d.%d.%d\n", (int)FSA_VERSION_GET_A(minver), 
        (int)FSA_VERSION_GET_B(minver), (int)FSA_VERSION_GET_C(minver), (int)FSA_VERSION_GET_D(minver));
    if (curver < minver)
    {
        errprintf("This filesystem can only be restored with fsarchiver %d.%d.%d.%d or more recent\n",
        (int)FSA_VERSION_GET_A(minver), (int)FSA_VERSION_GET_B(minver), (int)FSA_VERSION_GET_C(minver),
        (int)FSA_VERSION_GET_D(minver));
        return -1;
    }
    
    // check the partition is not mounted
    res=generic_get_mntinfo(partition, &readwrite, mntbuf, sizeof(mntbuf), optbuf, sizeof(optbuf), fsbuf, sizeof(fsbuf));
    if (res==0)
    {
        errprintf("partition [%s] is mounted on [%s].\ncannot restore an archive to a partition "
            "which is mounted, unmount it first: umount %s\n", partition, mntbuf, mntbuf);
        return -1;
    }
    
    // read filesystem-header from archive
    if (queue_dequeue_header(g_queue, &dicobegin, &headertype, NULL) <= 0)
    {
        errprintf("queue_dequeue_header() failed: cannot read file system dico\n");
        return -1;
    }
    dico_destroy(dicobegin);
    
    if (headertype != FSA_HEADTYPE_FSYB)
    {
        errprintf("header is not what we expected: found=[%ld] and expected=[%ld]\n", (long)headertype, (long)FSA_HEADTYPE_FSYB);
        return -1;
    }
    
    // if a filesystem to use was specified: overwrite the default one
    if (strdico_get_string(dicocmdline, tempbuf, sizeof(tempbuf), "mkfs") == 0)
    {
        snprintf(filesystem, sizeof(filesystem), "%s", tempbuf);
    }
    else if ((dico_get_string(dicofs, 0, FSYSHEADKEY_FILESYSTEM, filesystem, sizeof(filesystem))) < 0)
    {
        errprintf("dico_get_string(FSYSHEADKEY_FILESYSTEM) failed\n");
        return -1;
    }
       
    // read make file system options from dicocmdline
    if (strdico_get_string(dicocmdline, mkfsoptions, sizeof(mkfsoptions), "mkfsopt")!=0)
    {
        msgprintf(MSG_VERB2,"strdico_get_string(dicocmdline, 'mkfsopt') doesn't exist\n");
    }

    if (dico_get_u64(dicofs, 0, FSYSHEADKEY_BYTESTOTAL, &fsbytestotal) != 0)
    {
        errprintf("dico_get_string(FSYSHEADKEY_BYTESTOTAL) failed\n");
        return -1;
    }
    
    if (dico_get_u64(dicofs, 0, FSYSHEADKEY_BYTESUSED, &fsbytesused) != 0)
    {
        errprintf("dico_get_string(FSYSHEADKEY_BYTESUSED) failed\n");
        return -1;
    }

    msgprintf(MSG_VERB2, "filesystem=[%s]\n", filesystem);
    msgprintf(MSG_VERB2, "filesystemoptions=[%s]\n", mkfsoptions);
    msgprintf(MSG_VERB2, "fsbytestotal=[%s]\n", format_size(fsbytestotal, text, sizeof(text), 'h'));
    msgprintf(MSG_VERB2, "fsbytesused=[%s]\n", format_size(fsbytesused, text, sizeof(text), 'h'));

    // get index of the filesystem in the filesystem table
    if (generic_get_fstype(filesystem, &fstype) != 0)
    {
        errprintf("filesystem [%s] is not supported by fsarchiver\n", filesystem);
        return -1;
    }

    // ---- make the filesystem
    if (filesys[fstype].mkfs(dicofs, partition, mkfsoptions) != 0)
    {
        errprintf("cannot format the filesystem %s on partition %s\n", filesystem, partition);
        return -1;
    }

    // ---- mount the new filesystem
    mkdir_recursive(mntbuf);
    generate_random_tmpdir(mntbuf, sizeof(mntbuf), 0);
    mkdir_recursive(mntbuf);

    if ((dico_get_string(dicofs, 0, FSYSHEADKEY_MOUNTINFO, mountinfo, sizeof(mountinfo))) < 0)
        memset(mountinfo, 0, sizeof(mountinfo));
    msgprintf(MSG_VERB1, "Mount information: [%s]\n", mountinfo);
    if (filesys[fstype].mount(partition, mntbuf, filesys[fstype].name, 0, mountinfo) != 0)
    {
        errprintf("partition [%s] cannot be mounted on %s. cannot continue.\n", partition, mntbuf);
        return -1;
    }
    
    if (extractar_extract_read_objects(exar, &errors, mntbuf, fstype) != 0)
    {
        msgprintf(MSG_STACK, "extract_read_objects(%s) failed\n", mntbuf);
        ret=-1;
        goto filesystem_extract_umount;
    }
    else if (errors > 0)
    {
        msgprintf(MSG_DEBUG1, "extract_read_objects(%s) worked with errors\n", mntbuf);
        ret=-1;
        goto filesystem_extract_umount;
    }

    // read "end of file-system" header from archive
    if (queue_dequeue_header(g_queue, &dicoend, &headertype, NULL) <= 0)
    {
        errprintf("queue_dequeue_header() failed\n");
        ret=-1;
        goto filesystem_extract_umount;
    }
    dico_destroy(dicoend);

    if ((get_status() == STATUS_RUNNING) && (headertype != FSA_HEADTYPE_DATF))
    {
        errprintf("header is not what we expected: found=[%ld] and expected=[%ld]\n", (long)headertype, (long)FSA_HEADTYPE_DATF);
        goto filesystem_extract_umount;
    }

filesystem_extract_umount:
    if (filesys[fstype].umount(partition, mntbuf) != 0)
    {
        sysprintf("cannot umount %s\n", mntbuf);
        ret=-1;
    }
    else
    {
        rmdir(mntbuf); // remove temp dir created by fsarchiver
    }
    return ret;
}

int restore(int argc, char **argv, int oper)
{
    pthread_t thread_decomp[FSA_MAX_COMPJOBS]; // reads blocks from queue and does decompression/decryption
    pthread_t thread_enqueue; // read raw bytes from iobuffer and put headers and blocks to the queue
    pthread_t thread_iobuffer; // read archive, fec_decode and store raw bytes in iobuffer
    pthread_t thread_compat06; // read archive, fec_decode and store raw bytes in iobuffer
    cdico *dicofsinfo[FSA_MAX_FSPERARCH];
    cstrdico *dicoargv[FSA_MAX_FSPERARCH];
    cdico *dicomainhead=NULL;
    cdico *dicodisklayout=NULL;
    carchinfo archinfo;
    char *destdir=argv[0];
    cdico *dirsinfo=NULL;
    char restptbuf[65535];
    struct stat64 st;
    cextractar exar;
    u32 headertype;
    u64 totalerr=0;
    u64 fscost;
    u64 curver;
    int errors=0;
    int ret=0;
    int i;

    // init
    memset(&archinfo, 0, sizeof(archinfo));
    memset(&exar, 0, sizeof(exar));
    exar.cost_global = 0;
    exar.cost_current = 0;

    // set all threads to 0
    memset(thread_decomp, 0, sizeof(thread_decomp));
    thread_enqueue = 0;
    thread_iobuffer = 0;
    thread_compat06 = 0;

    // init misc data struct to zero
    for (i = 0; i < FSA_MAX_FSPERARCH; i++)
        dicoargv[i]=NULL;
    for (i = 0; i < FSA_MAX_FSPERARCH; i++)
        dicofsinfo[i] = NULL;
    for (i = 0; i < FSA_MAX_COMPJOBS; i++)
        thread_decomp[i] = 0;
    for (i = 0; i < FSA_MAX_FSPERARCH; i++)
        g_fsbitmap[i] = 0;
    thread_enqueue = 0;

    // check the archive file exists
    while (regfile_exists(g_archive) != true)
    {
        errprintf("The archive file cannot be found: %s\n", g_archive);
        goto do_extract_error;
    }

    // detect version of the file format
    if ((g_archver = detect_file_format_version(g_archive)) == FSA_FMT_NULL)
    {
        errprintf("Invalid archive file: %s\n", g_archive);
        goto do_extract_error;
    }

    // convert the command line arguments to dicos and init g_fsbitmap
    switch (oper)
    {
        case OPER_RESTFS:
            // convert the arguments from the command line to dico
            if (convert_argv_to_strdicos(dicoargv, argc, argv, "id,dest,mkfs,mkfsopt")!=0)
            {
                msgprintf(MSG_STACK, "convert_argv_to_dico() failed\n");
                goto do_extract_error;
            }
            // say to the threadio_readarch thread which filesystems have to be read in archive
            for (i=0; i<FSA_MAX_FSPERARCH; i++)
                g_fsbitmap[i]=!!(dicoargv[i]!=NULL);
            break;

       case OPER_RESTDIR: // the files are all considered as belonging to fsid==0
            g_fsbitmap[0]=1;
            break;

        case OPER_RESTPT:
            // convert the arguments from the command line to dico
            if (convert_argv_to_strdicos(dicoargv, argc, argv, "id,dest")!=0)
            {
                msgprintf(MSG_STACK, "convert_argv_to_dico() failed\n");
                goto do_extract_error;
            }
     }

    // create decompression threads
    for (i = 0; (i < g_options.compressjobs) && (i < FSA_MAX_COMPJOBS); i++)
    {
        if (pthread_create(&thread_decomp[i], NULL, thread_decomp_fct, NULL) != 0)
        {   errprintf("pthread_create(thread_decomp_fct) failed\n");
            goto do_extract_error;
        }
    }

    // create archive-reader thread(s)
    switch (g_archver)
    {
        case FSA_FMT_06: // archive in 0.6.x archive file format
            if (pthread_create(&thread_compat06, NULL, thread_compat06_fct, NULL) != 0)
            {
                errprintf("pthread_create(thread_reader_fct) failed\n");
                goto do_extract_error;
            }
            break;

        case FSA_FMT_07: // archive in 0.7.x archive file format
            if (pthread_create(&thread_iobuffer, NULL, thread_archio_to_iobuf_fct, NULL) != 0)
            {
                errprintf("pthread_create(thread_archio_to_iobuf_fct) failed\n");
                ret=-1;
                goto do_extract_error;
            }
            if (pthread_create(&thread_enqueue, NULL, thread_iobuf_to_queue_fct, NULL) != 0)
            {
                errprintf("pthread_create(thread_iobuf_to_queue_fct) failed\n");
                goto do_extract_error;
            }
            break;

        default:
            errprintf("Invalid archive file format: %d\n", g_archver);
            goto do_extract_error;
            break;
    }

    // read archive main header
    if (extractar_read_mainhead(&archinfo, &dicomainhead) != 0)
    {
        msgprintf(MSG_STACK, "read_mainhead() failed\n");
        goto do_extract_error;
    }

    // read disk layout header
    if (extractar_read_disk_layout(&archinfo, &dicodisklayout) != 0)
    {
        errprintf("extractar_read_disk_layout() failed: cannot read disk layout\n");
    }

    // check that the minimum fsarchiver version required is ok
    curver = FSA_VERSION_BUILD(PACKAGE_VERSION_A, PACKAGE_VERSION_B, PACKAGE_VERSION_C, PACKAGE_VERSION_D);
    msgprintf(MSG_VERB2, "Current fsarchiver version: %d.%d.%d.%d\n", (int)FSA_VERSION_GET_A(curver), 
        (int)FSA_VERSION_GET_B(curver), (int)FSA_VERSION_GET_C(curver), (int)FSA_VERSION_GET_D(curver));
    msgprintf(MSG_VERB2, "Minimum fsarchiver version for that archive: %d.%d.%d.%d\n", (int)FSA_VERSION_GET_A(archinfo.minfsaver), 
        (int)FSA_VERSION_GET_B(archinfo.minfsaver), (int)FSA_VERSION_GET_C(archinfo.minfsaver), (int)FSA_VERSION_GET_D(archinfo.minfsaver));

    if (((oper == OPER_RESTFS) || (oper == OPER_RESTDIR)) && (curver < archinfo.minfsaver))
    {
        errprintf("This archive can only be restored with fsarchiver %d.%d.%d.%d or more recent\n",
        (int)FSA_VERSION_GET_A(archinfo.minfsaver), (int)FSA_VERSION_GET_B(archinfo.minfsaver), 
        (int)FSA_VERSION_GET_C(archinfo.minfsaver), (int)FSA_VERSION_GET_D(archinfo.minfsaver));
        goto do_extract_error;
    }

    // check that the operation requested on the command line matches the archive type
    switch (archinfo.archtype)
    {
        case ARCHTYPE_FILESYSTEMS:
            if (oper != OPER_RESTFS && oper != OPER_RESTPT && oper != OPER_SHOWPT && oper != OPER_ARCHINFO)
            {
                errprintf("this archive contains filesystems. The command is not appropriate for that type of archive\n");
                goto do_extract_error;
            }
            break;

        case ARCHTYPE_DIRECTORIES:
            if (oper != OPER_RESTDIR && oper != OPER_ARCHINFO)
            {
                errprintf("this archive contains flat files & directories. The command is not appropriate for that type of archive\n");
                goto do_extract_error;
            }
            break;

        default:
            errprintf("this archive has an unknown type: %d, cannot continue\n", archinfo.archtype);
            goto do_extract_error;
            break;
    }

    // check that password has been provided if necessary
    if ((archinfo.cryptalgo != ENCRYPT_NONE) && (g_options.encryptalgo != ENCRYPT_BLOWFISH))
    {
        errprintf("this archive has been encrypted, you have to provide a password on the command line using option '-c'\n");
        goto do_extract_error;
    }

    // check arguments are valid
    switch (oper)
    {
        case OPER_RESTFS:
            if (archinfo.fscount<=0)
            {
                errprintf("the archive contain no filesystem\n");
                goto do_extract_error;
            }
            for (i=0; i<FSA_MAX_FSPERARCH; i++)
            {
                if ((dicoargv[i]!=NULL) && (i >= archinfo.fscount))
                {
                    errprintf("invalid filesystem id: [%d]. the filesystem id must be an integer between 0 and %d\n", (int)i, (int)(archinfo.fscount-1));
                    goto do_extract_error;
                }
            }
            break;
    
        case OPER_RESTDIR:
            destdir=argv[0];
            if (stat64(destdir, &st)!=0)
            {   
                switch (errno)
                {
                    case ENOENT:
                        sysprintf("%s does not exist, cannot continue\n", destdir);
                        break;
                    default:
                        sysprintf("fstat64(%s) failed\n", destdir);
                        break;
                }
                goto do_extract_error;
            }
            
            if (!S_ISDIR(st.st_mode))
            {
                errprintf("%s is not a valid directory, cannot continue\n", destdir);
                goto do_extract_error;
            }
            break;
            
        case OPER_RESTPT:
            for (i=0; i < FSA_MAX_PTPERARCH; i++)
            {
                if ((dicoargv[i] != NULL) && (i >= archinfo.ptcount))
                {
                    errprintf("invalid partition-table id: [%d]. the partition-table id must be an integer between 0 and %d\n", 
                        (int)i, (int)(archinfo.ptcount-1));
                    goto do_extract_error;
                }
            }
            // fallthrough
        case OPER_SHOWPT:
            if (archinfo.ptcount<=0)
            {
                errprintf("the archive contain no partition-table\n");
                goto do_extract_error;
            }
            break;
    }

    // read the fsinfo header for each filesystem and calculate total cost of the restfs
    for (i = 0; (archinfo.archtype == ARCHTYPE_FILESYSTEMS) && (i < archinfo.fscount) && (i < FSA_MAX_FSPERARCH); i++)
    {
        if (queue_dequeue_header(g_queue, &dicofsinfo[i], &headertype, NULL)<=0)
        {
            errprintf("queue_dequeue_header() failed: cannot read filesystem-info dico\n");
            goto do_extract_error;
        }
        if (headertype != FSA_HEADTYPE_FSIN)
        {
            errprintf("header is not what we expected: found=[%ld] and expected=[%ld]\n", (long)headertype, (long)FSA_HEADTYPE_FSIN);
            goto do_extract_error;
        }
        if ((dicoargv[i] != NULL) && (dico_get_u64(dicofsinfo[i], 0, FSYSHEADKEY_TOTALCOST, &fscost) == 0))
        {
            exar.cost_global+=fscost;
        }
    }

    // read the dirsinfo header which contains the statistrics (only if this header is present)
    if ((archinfo.archtype == ARCHTYPE_DIRECTORIES) && (archinfo.hasdirsinfohead == true))
    {
        if (queue_dequeue_header(g_queue, &dirsinfo, &headertype, NULL)<=0)
        {
            errprintf("queue_dequeue_header() failed: cannot read the dirsinfo header\n");
            goto do_extract_error;
        }
        if (headertype != FSA_HEADTYPE_DIRS)
        {
            errprintf("header is not what we expected: found=[%ld] and expected=[%ld]\n", (long)headertype, (long)FSA_HEADTYPE_DIRS);
            goto do_extract_error;
        }
        if ((dirsinfo!=NULL) && (dico_get_u64(dirsinfo, 0, DIRSINFOKEY_TOTALCOST, &exar.cost_global)!=0))
        {
            errprintf("cannot read DIRSINFOKEY_TOTALCOST in dirsinfo\n");
            goto do_extract_error;
        }
    }

    switch (oper)
    {
        case OPER_RESTFS:
            for (i=0; (i < archinfo.fscount) && (i < FSA_MAX_FSPERARCH) && (get_status() == STATUS_RUNNING); i++)
            {
                if (dicoargv[i]!=NULL) // that filesystem has been requested on the command line
                {
                    exar.fsid=i;
                    memset(&exar.stats, 0, sizeof(exar.stats)); // init stats to zero
                    msgprintf(MSG_VERB1, "============= extracting filesystem %d =============\n", i);
                    if (extractar_filesystem_extract(&exar, dicofsinfo[i], dicoargv[i])!=0)
                    {
                        msgprintf(MSG_STACK, "extract_filesystem(%d) failed\n", i);
                        goto do_extract_error;
                    }
                    if (get_status() == STATUS_RUNNING)
                        stats_show(exar.stats, i);
                    totalerr+=stats_errcount(exar.stats);
                }
                // else: the thread_queue2iobuf automatically skips filesystem when g_fsbitmap[fsid]==0
            }
            break;

        case OPER_RESTDIR:
            exar.fsid=0;
            memset(&exar.stats, 0, sizeof(exar.stats)); // init stats to zero
            if (extractar_extract_read_objects(&exar, &errors, destdir, 0)!=0) // TODO: get the right fstype
            {
                errprintf("extract_read_objects(%s) failed\n", destdir);
                goto do_extract_error;
            }
            stats_show(exar.stats, 0);
            totalerr += stats_errcount(exar.stats);
            break;

        case OPER_RESTPT:
            for (i=0; (i < archinfo.ptcount) && (i < FSA_MAX_PTPERARCH) && (get_status() == STATUS_RUNNING); i++)
            {
                if (dicoargv[i] != NULL) // that parttable has been requested on the command line
                {
                    if (dico_get_string(dicodisklayout, LAYOUTHEADSEC_PARTTABLE, i, restptbuf, (u16)sizeof(restptbuf)) < 0)
                    {
                        errprintf("dico_get_string(%d, %d) failed to read the partition table description\n", LAYOUTHEADSEC_PARTTABLE, i);
                        goto do_extract_error;
                    }
                    if (restpt(restptbuf, i, dicoargv[i])!=0)
                    {
                        msgprintf(MSG_STACK, "restpt() failed to restore partition table number %d\n", i);
                        goto do_extract_error;
                    }
                }
            }
            // stop reading the archive file
            if (get_status() == STATUS_RUNNING)
                set_status(STATUS_FINISHED, "");
            break;

        case OPER_SHOWPT:
            for (i = 0; (i < archinfo.ptcount) && (i < FSA_MAX_PTPERARCH) && (get_status() == STATUS_RUNNING); i++)
            {
                if (dico_get_string(dicodisklayout, LAYOUTHEADSEC_PARTTABLE, i, restptbuf, (u16)sizeof(restptbuf))<0)
                {
                    errprintf("dico_get_string(%d, %d) failed to read the partition table description\n", LAYOUTHEADSEC_PARTTABLE, i);
                    goto do_extract_error;
                }
                if (showpt(restptbuf, i) != 0)
                {
                    msgprintf(MSG_STACK, "showpt() failed to restore partition table number %d\n", i);
                    goto do_extract_error;
                }
            }
            // stop reading the archive file
            if (get_status() == STATUS_RUNNING)
                set_status(STATUS_FINISHED, "");
            break;

        case OPER_ARCHINFO:
            if (archinfo_show_mainhead(&archinfo)!=0)
            {
                errprintf("archinfo_show_mainhead() failed\n");
                goto do_extract_error;
            }
            for (i = 0; (archinfo.archtype == ARCHTYPE_FILESYSTEMS) && (i < archinfo.fscount) && (i < FSA_MAX_FSPERARCH); i++)
            {
                if (archinfo_show_fshead(dicofsinfo[i], i) != 0)
                {
                    errprintf("archinfo_show_fshead() failed\n");
                    goto do_extract_error;
                }
            }
            // stop reading the archive file
            if (get_status() == STATUS_RUNNING)
                set_status(STATUS_FINISHED, "");
            break;
    }

    switch (get_status())
    {
        case STATUS_RUNNING:
        case STATUS_FINISHED:
            goto do_extract_cleanup;
        
        case STATUS_ABORTED:
            //msgprintf(MSG_FORCE, "operation aborted by user\n");
            goto do_extract_error;
        
        case STATUS_FAILED:
            //msgprintf(MSG_FORCE, "operation failed\n");
            goto do_extract_error;
    }

do_extract_error:
    if (get_status() == STATUS_RUNNING)
        set_status(STATUS_FAILED, "restore.c(do_extract_error)");
    ret=-1;

do_extract_cleanup:
    msgprintf(MSG_DEBUG1, "THREAD-MAIN2: exit\n");
    msgprintf(MSG_DEBUG2, "queue_count_items_todo(g_queue)=%d\n", (int)queue_count_items_todo(g_queue));
    while (queue_count_items_todo(g_queue) > 0) // let thread_compress process all the pending blocks
    {
        msgprintf(MSG_DEBUG2, "queue_count_items_todo(): %ld\n", (long)queue_count_items_todo(g_queue));
        usleep(10000);
    }
    msgprintf(MSG_DEBUG2, "queue_count_items_todo(g_queue)=%d\n", (int)queue_count_items_todo(g_queue));
    // now we are sure that thread_compress is not working on an item in the queue so we can empty the queue
    while ((get_secthreads() > 0) && (queue_get_end_of_queue(g_queue) == false))
        queue_destroy_first_item(g_queue);
    msgprintf(MSG_DEBUG1, "THREAD-MAIN2: queue is now empty\n");
    // the queue is empty, so thread_compress should now exit

    for (i = 0; (i < g_options.compressjobs) && (i < FSA_MAX_COMPJOBS); i++)
        if ((thread_decomp[i] != 0) && (pthread_join(thread_decomp[i], NULL) != 0))
            errprintf("pthread_join(thread_decomp) failed\n");

    if ((thread_enqueue != 0) && (pthread_join(thread_enqueue, NULL) != 0))
        errprintf("pthread_join(thread_reader) failed\n");

    if ((thread_iobuffer != 0) && (pthread_join(thread_iobuffer, NULL) != 0))
        errprintf("pthread_join(thread_iobuffer) failed\n");

    if ((thread_compat06 != 0) && (pthread_join(thread_compat06, NULL) != 0))
        errprintf("pthread_join(thread_iobuffer) failed\n");

    for (i = 0; i < FSA_MAX_FSPERARCH; i++)
        if (dicoargv[i]!=NULL)
            strdico_destroy(dicoargv[i]);

    for (i = 0; i < FSA_MAX_FSPERARCH; i++)
    {
        if (dicofsinfo[i] != NULL)
        {
            dico_destroy(dicofsinfo[i]);
            dicofsinfo[i] = NULL;
        }
    }

    // change the status if there were non-critical errors
    if (totalerr > 0)
        ret=-1;

    dico_destroy(dicomainhead);
    return ret;
}
