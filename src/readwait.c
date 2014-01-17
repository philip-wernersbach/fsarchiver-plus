/*
 * fsarchiver: Filesystem Archiver
 *
 * Copyright (C) 2014 Philip Wernersbach & Jacobs Automation. All rights reserved.
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

#include <sys/inotify.h>
#include <errno.h>
#include <signal.h>

#include "fsarchiver.h"
#include "dico.h"
#include "common.h"
#include "options.h"
#include "archreader.h"
#include "queue.h"
#include "comp_gzip.h"
#include "comp_bzip2.h"
#include "error.h"

#include "syncthread.h"

#define INOTIFY_BUFFER_SIZE (sizeof(struct inotify_event) + PATH_MAX + 1)

char *readwait_filename = NULL;
static int inotify_fd = -1;
static int inotify_watch_desc = -1;

static __thread char *event_buffer = NULL;

ssize_t readwait(int fildes, void *buf, size_t nbyte) {
	s64 position = lseek64(fildes, 0, SEEK_CUR);
	s64 file_size = lseek64(fildes, 0, SEEK_END);
	
	s64 read_to = -1;
	ssize_t inotify_read_return = -1;
	
	int prior_errno;
	
	if (lseek64(fildes, position, SEEK_SET) != position) {
		errno = EIO;
		return -1;
	}
	
	read_to = position + nbyte;
	
	if (file_size < read_to) {
		if ((inotify_fd < 0) && (inotify_watch_desc < 0)) {
			if ((inotify_fd = inotify_init()) < 0) {
				inotify_fd = -1;
				
				errno = EIO;
				return -1;
			}
			
			if ((inotify_watch_desc = inotify_add_watch(inotify_fd, readwait_filename, IN_MODIFY)) < 0) {
				close(inotify_fd);
				
				inotify_fd = -1;
				inotify_watch_desc = -1;
				
				errno = EIO;
				return -1;
			}
		}
		
		if (event_buffer == NULL)
			event_buffer = malloc(INOTIFY_BUFFER_SIZE);
		
		prior_errno = errno;
		if ((inotify_read_return = read(inotify_fd, event_buffer, INOTIFY_BUFFER_SIZE)) < 1) {
			free(event_buffer);
			
			// If we read things, we need to set errno because
			// we're going to return -1. 
			if (inotify_read_return > -1)
				errno = EIO;
			
			// If the read was interrupted, simulate an EOF.
			if (errno == EINTR) {
				errno = prior_errno;
				return 0;
			}
				
			return -1;
		}
		
		return readwait(fildes, buf, nbyte);
	}
	
	return read(fildes, buf, nbyte);
}
