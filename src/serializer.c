#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <stdarg.h>

#include "serializer.h"
#include "common.h"

cserializer *serializer_alloc()
{
    cserializer *serial;
    pthread_mutexattr_t attr;

    if ((serial=calloc(1, sizeof(cserializer))) == NULL)
        return NULL;

	serial->head=NULL;
    assert(pthread_mutexattr_init(&attr)==0);
    assert(pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE)==0);
	assert(pthread_mutex_init(&serial->mutex, &attr)==0);

	return serial;
}

int serializer_dump(cserializer *a, char *buffer, int bufsize)
{
	cserializeritem *cur;

	assert(a);
	assert(buffer);
	assert(bufsize>=0);
	
	memset(buffer, 0, bufsize);
	assert(pthread_mutex_lock(&a->mutex)==0);
	
	if (a->head==NULL) // empty
	{	assert(pthread_mutex_unlock(&a->mutex)==0);
		return 1;
	}
	
	for (cur=a->head; cur!=NULL; cur=cur->next)
		strlcatf(buffer, bufsize, "val(%s,%s)=(%s)\n", cur->key1, cur->key2, cur->data);
    
	assert(pthread_mutex_unlock(&a->mutex)==0);
	
	return 0;
}

int serializer_read(cserializer *a, char *buffer)
{
    char delims[]="\n";
    char *saveptr=0;
    char *result;
    char data[1024];
    char key1[512];
    char key2[512];
    int i;

	assert(a);
	assert(buffer);

	assert(pthread_mutex_lock(&a->mutex)==0);

    result=strtok_r(buffer, delims, &saveptr);
    for (i=0; result != NULL; i++)
    {
        key1[0]=key2[0]=data[0]=0;
        if (sscanf(result, "val(%[^,\n()],%[^,\n()])=(%[^,\n()])\n", key1, key2, data) >= 2)
            serializer_setbykeys_data(a, key1, key2, data, strlen(data)+1);
        result = strtok_r(NULL, delims, &saveptr);
    }

	assert(pthread_mutex_unlock(&a->mutex)==0);

	return 0;
}

int serializer_getbykeys_data(cserializer *a, char *key1, char *key2, void *data, int bufsize)
{
	cserializeritem *cur;
	
	assert(a);
	assert(key1);
	assert(strlen(key1)>0);
	assert(key2);
	assert(strlen(key2)>0);
	assert(data);
	assert(bufsize>=0);
	
	// init
	memset(data, 0, bufsize);
	
	assert(pthread_mutex_lock(&a->mutex)==0);
	
	if (a->head==NULL) // empty
	{
		assert(pthread_mutex_unlock(&a->mutex)==0);
		return -1;
	}
	
	for (cur=a->head; cur!=NULL; cur=cur->next)
	{
		if (strcmp(key1, cur->key1)==0 && strcmp(key2, cur->key2)==0)
		{
			if (bufsize < cur->size) // destination buffer too small
			{
                assert(pthread_mutex_unlock(&a->mutex)==0);
				return -2;
			}
			if ((cur->size>0) && (cur->data!=NULL))
			{
				memcpy(data, cur->data, cur->size);
			}
 			assert(pthread_mutex_unlock(&a->mutex)==0);
			return 0;
		}
	}
	
	assert(pthread_mutex_unlock(&a->mutex)==0);
	return -3;
}

int serializer_getbykeys_integer(cserializer *a, char *key1, char *key2, s64 *value)
{
    char buffer[512];
    int res;
    
	assert(a);
	assert(key1);
	assert(strlen(key1)>0);
	assert(key2);
	assert(strlen(key2)>0);
	assert(value);
    
    memset(buffer, 0, sizeof(buffer));
    *value = 0;
    
    if ( (res=serializer_getbykeys_data(a, key1, key2, buffer, sizeof(buffer))) != 0)
        return res;
    
    *value = atoll(buffer);
    
    return 0;
}

int serializer_setbykeys_data(cserializer *a, char *key1, char *key2, void *data, int datsize)
{
	cserializeritem *cur, *newitem;
	int len1, len2;

	assert(a);
	assert(key1);
	assert(strlen(key1)>0);
	assert(key2);
	assert(strlen(key2)>0);
	assert(data);
	assert(datsize>=0);
    
	// create new item in memory
	newitem=calloc(1, sizeof(cserializeritem));
	if (!newitem)
		return -1;
	newitem->next=NULL;
	newitem->size=datsize;
	len1=strlen(key1)+1;
	len2=strlen(key2)+1;
    
    if ((newitem->key1=malloc(len1))==NULL || (newitem->key2=malloc(len2))==NULL)
    {
        free(newitem->key1);
        free(newitem->key2);
        return -1;
    }
    
	memcpy(newitem->key1, key1, len1);
	memcpy(newitem->key2, key2, len2);
	
	if (datsize>0)
	{
		newitem->data=malloc(datsize);
		if (!newitem->data)
			return -1;
		memcpy(newitem->data, data, datsize);
	}
	else // datsize==0
	{
		newitem->data=NULL;
	}
	   
	assert(pthread_mutex_lock(&a->mutex)==0);
    
	// put a link to the new item
	if (a->head==NULL) // empty
	{	
		a->head=newitem;
		assert(pthread_mutex_unlock(&a->mutex)==0);
		return 0;
	}
	else // list is not empty
	{
		// overwrite the current value if it exists
		for (cur=a->head; cur!=NULL; cur=cur->next)
		{
            // replace existing item found in list
			if (strcmp(key1, cur->key1)==0 && strcmp(key2, cur->key2)==0)
			{
                // free memory with the old data
				free(cur->data);
				cur->data=newitem->data;
				cur->size=newitem->size;
                // don't use newitem, reuse existing one
				free(newitem->key1);
				free(newitem->key2);
				free(newitem);
				assert(pthread_mutex_unlock(&a->mutex)==0);
				return 0;
			}
		}
		// if key not found, add newitem to the end
		newitem->next=NULL;
		for (cur=a->head; cur->next!=NULL; cur=cur->next);
		assert(cur); // list is not empty at that stage so must work
		cur->next=newitem;
		assert(pthread_mutex_unlock(&a->mutex)==0);
		return 0;
	}
}

int serializer_count(cserializer *a)
{
	cserializeritem *cur;
	int count=0;
	
	assert(a);
	
	assert(pthread_mutex_lock(&a->mutex)==0);
	
	if (a->head==NULL) // empty
	{	assert(pthread_mutex_unlock(&a->mutex)==0);
		return 0;
	}
	
	for (cur=a->head; cur!=NULL; cur=cur->next)
		count++;
	
	assert(pthread_mutex_unlock(&a->mutex)==0);
	return count;
}

// use this function when you want the data to be a formatted string
int serializer_setbykeys_format(cserializer *a, char *key1, char *key2, char *fmt, ...)
{
	char temp[4096];
	va_list ap;
	
	va_start(ap, fmt);
	vsnprintf(temp, sizeof(temp), fmt, ap);
	va_end(ap);
	
	return serializer_setbykeys_data(a, key1, key2, temp, strlen(temp)+1);
}

int serializer_setbykeys_integer(cserializer *a, char *key1, char *key2, s64 value)
{
    return serializer_setbykeys_format(a, key1, key2, "%lld", (long long)value);
}

int serializer_destroy(cserializer *a)
{
	cserializeritem *item;
	cserializeritem *next;

	if (a==NULL)
        return 0;

	assert(pthread_mutex_lock(&a->mutex)==0);	

	item=a->head;
	while (item!=NULL)
	{
		next=item->next;
		free(item->key1);
		free(item->key2);
		free(item->data);
		free(item);
		item=next;
	}
	a->head=NULL;

	assert(pthread_mutex_unlock(&a->mutex)==0);	
	pthread_mutex_destroy(&a->mutex);
	memset(a, 0, sizeof(cserializer));

	return 0;
}
