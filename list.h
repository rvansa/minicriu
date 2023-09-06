#pragma once

// TODO: we could alloc a bigger chunk of memory in init, and don't have to
// deal with fragmentation on release since this list is meant as append-only
// In practice the performance doesn't matter too much, though.
#define DECLARE_LIST(type) \
typedef struct type##_list_item { \
	type value; \
	struct type##_list_item *next; \
} type##_list_item; \
typedef struct { \
	type##_list_item *first; \
	type##_list_item *last; \
	size_t size; \
} type##_list; \
static void type##_list_init(type##_list *list) { \
	list->size = 0; \
	list->first = NULL; \
	list->last = NULL; \
} \
static void type##_list_destroy(type##_list *list) { \
	for (type##_list_item *item = list->first; item != NULL; ) { \
		type##_list_item *next = item->next; \
		free(item); \
		item = next; \
	} \
	list->size = 0; \
	list->first = NULL; \
	list->last = NULL; \
} \
static type *type##_list_prepend(type##_list *list) { \
	type##_list_item *first = list->first; \
	list->size++; \
	list->first = malloc(sizeof(type##_list_item)); \
	list->first->next = first; \
	if (first == NULL) { \
		list->last = list->first; \
	} \
	return &list->first->value; \
} \
static type *type##_list_append(type##_list *list) { \
	type##_list_item *last = list->last; \
	list->size++; \
	list->last = malloc(sizeof(type##_list_item)); \
	list->last->next = NULL; \
	if (last == NULL) { \
		list->first = list->last; \
	} else { \
		last->next = list->last; \
	} \
	return &list->last->value; \
}

#define INIT_LIST(type, list) type##_list_init(list)
#define PREPEND_LIST(type, list) type##_list_prepend(list)
#define APPEND_LIST(type, list) type##_list_append(list)
#define DESTROY_LIST(type, list) type##_list_destroy(list)

#define FOREACH(type, list) \
	for (type##_list_item *item = (list)->first, *prev = NULL; item != NULL; prev = item, item = item->next)

