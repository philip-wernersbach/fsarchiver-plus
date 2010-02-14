sbin_PROGRAMS		= fsarchiver

fsarchiver_SOURCES	= fsarchiver.c save.c restore.c probe.c \
	thread_archio.c archreader.c archwriter.c writebuf.c archinfo.c \
	thread_comp.c comp_gzip.c comp_bzip2.c comp_lzma.c comp_lzo.c crypto.c \
	fs_ntfs.c fs_ext2.c fs_reiserfs.c fs_reiser4.c fs_btrfs.c fs_xfs.c fs_jfs.c \
	layout_save.c layout_read.c layout_rest.c \
	common.c dico.c strdico.c dichl.c queue.c error.c syncthread.c \
	datafile.c strlist.c regmulti.c options.c logfile.c filesys.c devinfo.c

noinst_HEADERS		= fsarchiver.h save.h restore.h probe.h \
	thread_archio.h archreader.h archwriter.h writebuf.h archinfo.h \
	thread_comp.h comp_gzip.h comp_bzip2.h comp_lzma.h comp_lzo.h crypto.h \
	fs_ntfs.h fs_ext2.h fs_reiserfs.h fs_reiser4.h fs_btrfs.h fs_xfs.h fs_jfs.h \
	layout_save.h layout_read.h layout_rest.h \
	common.h dico.h strdico.h dichl.h queue.h error.h syncthread.h \
	datafile.h strlist.h regmulti.h options.h logfile.h types.h filesys.h devinfo.h

fsarchiver_LDADD	= -lpthread -lrt \
			  $(LZMA_LIBS) \
			  $(EXT2FS_LIBS) \
			  $(COM_ERR_LIBS) \
			  $(E2P_LIBS) \
			  $(BLKID_LIBS) \
			  $(UUID_LIBS) \
			  $(LIBPARTED_LIBS) \
			  $(LIBXML2_LIBS) 
fsarchiver_CFLAGS	= @CFLAGS@ -Wall -std=gnu99 -rdynamic -ggdb \
			  $(LZMA_CFLAGS) \
			  $(EXT2FS_CFLAGS) \
			  $(COM_ERR_LIBS) \
			  $(E2P_CFLAGS) \
			  $(BLKID_CFLAGS) \
			  $(UUID_CFLAGS) \
			  $(LIBPARTED_CFLAGS) \
			  $(LIBXML2_CFLAGS)
fsarchiver_LDFLAGS	= @FSARCHIVER_LDFLAGS@

DEFS=@DEFS@ -D_REENTRANT -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE -D_GNU_SOURCE

MAINTAINERCLEANFILES	= Makefile.in

