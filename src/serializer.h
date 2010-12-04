#ifndef __SERIALIZER_H__
#define __SERIALIZER_H__

#include <pthread.h>
#include "types.h"

struct s_serializer;
struct s_serializeritem;

typedef struct s_serializer cserializer;
typedef struct s_serializeritem cserializeritem;

struct s_serializeritem
{
	char *key1;
	char *key2;
	char *data;
	int	size;
	cserializeritem	*next;
};

struct s_serializer
{
	cserializeritem *head;
	pthread_mutex_t	mutex;
};

cserializer *serializer_alloc();
int serializer_setbykeys_data(cserializer *a, char *key1, char *key2, void *data, int datsize);
int serializer_setbykeys_format(cserializer *a, char *key1, char *key2, char *fmt, ...) __attribute__ ((format (printf, 4, 5)));
int serializer_setbykeys_integer(cserializer *a, char *key1, char *key2, s64 value);
int serializer_getbykeys_data(cserializer *a, char *key1, char *key2, void *data, int bufsize);
int serializer_getbykeys_integer(cserializer *a, char *key1, char *key2, s64 *value);
int serializer_dump(cserializer *a, char *buffer, int bufsize);
int serializer_read(cserializer *a, char *buffer);
int serializer_destroy(cserializer *a);
int serializer_count(cserializer *a);

#endif // __SERIALIZER_H__
