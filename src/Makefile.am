sbin_PROGRAMS		= fsarchiver

fsarchiver_SOURCES	= fsarchiver.c save.c restore.c probe.c \
	thread_queue2iobuf.c archio.c archinfo.c thread_compat06.c \
	thread_comp.c comp_gzip.c comp_bzip2.c comp_lzma.c comp_lzo.c crypto.c \
	fs_ntfs.c fs_ext2.c fs_reiserfs.c fs_reiser4.c fs_btrfs.c fs_xfs.c fs_jfs.c \
	common.c dico.c strdico.c dichl.c queue.c error.c syncthread.c \
	datafile.c strlist.c regmulti.c options.c logfile.c filesys.c devinfo.c \
	layout_save.c layout_rest.c serializer.c thread_iobuf2archio.c fec.c iobuffer.c

noinst_HEADERS		= fsarchiver.h save.h restore.h probe.h \
	thread_queue2iobuf.h archio.h archinfo.h thread_compat06.h \
	thread_comp.h comp_gzip.h comp_bzip2.h comp_lzma.h comp_lzo.h crypto.h \
	fs_ntfs.h fs_ext2.h fs_reiserfs.h fs_reiser4.h fs_btrfs.h fs_xfs.h fs_jfs.h \
	common.h dico.h strdico.h dichl.h queue.h error.h syncthread.h \
	datafile.h strlist.h regmulti.h options.h logfile.h types.h filesys.h devinfo.h \
	layout_save.h layout_rest.h serializer.h thread_iobuf2archio.h fec.h iobuffer.h

fsarchiver_LDADD	= -lpthread -lrt \
			  $(LZMA_LIBS) \
			  $(EXT2FS_LIBS) \
			  $(COM_ERR_LIBS) \
			  $(E2P_LIBS) \
			  $(BLKID_LIBS) \
			  $(UUID_LIBS) \
			  $(LIBPARTED_LIBS) \
			  $(LIBXML2_LIBS) 
fsarchiver_CFLAGS	= @CFLAGS@ -Wall -std=gnu99 -rdynamic -O1 -ggdb \
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
