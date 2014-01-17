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

#ifndef HAVE_READWAIT_H

char *readwait_filename;
ssize_t readwait(int fildes, void *buf, size_t nbyte);

#define read(a, b, c) readwait(a, b, c)

#define HAVE_READWAIT_H
#endif
