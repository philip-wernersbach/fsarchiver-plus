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

#ifndef __THREAD_QUEUE2IOBUF_H__
#define __THREAD_QUEUE2IOBUF_H__

#include <pthread.h>

void *thread_iobuf_to_queue_fct(void *args);
void *thread_queue_to_iobuf_fct(void *args);

#endif // __THREAD_QUEUE2IOBUF_H__
