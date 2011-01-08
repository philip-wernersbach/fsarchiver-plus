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

#ifndef __THREAD_IOBUF2ARCHIO_H__
#define __THREAD_IOBUF2ARCHIO_H__

#include <pthread.h>

void *thread_iobuf_to_archio_fct(void *args);
void *thread_archio_to_iobuf_fct(void *args);

#endif // __THREAD_IOBUF2ARCHIO_H__
