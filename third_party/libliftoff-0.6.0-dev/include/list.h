#ifndef LIST_H
#define LIST_H

#include <stdbool.h>
#include <stddef.h>

struct liftoff_list {
	struct liftoff_list *prev;
	struct liftoff_list *next;
};

void
liftoff_list_init(struct liftoff_list *list);

void
liftoff_list_insert(struct liftoff_list *list, struct liftoff_list *elm);

void
liftoff_list_remove(struct liftoff_list *elm);

void
liftoff_list_swap(struct liftoff_list *this, struct liftoff_list *other);

size_t
liftoff_list_length(const struct liftoff_list *list);

bool
liftoff_list_empty(const struct liftoff_list *list);

#define liftoff_container_of(ptr, sample, member)			\
	(__typeof__(sample))((char *)(ptr) -				\
			     offsetof(__typeof__(*sample), member))

#define liftoff_list_for_each(pos, head, member)			\
	for (pos = liftoff_container_of((head)->next, pos, member);	\
	     &pos->member != (head);					\
	     pos = liftoff_container_of(pos->member.next, pos, member))

#define liftoff_list_for_each_safe(pos, tmp, head, member)		\
	for (pos = liftoff_container_of((head)->next, pos, member),	\
	     tmp = liftoff_container_of(pos->member.next, tmp, member);	\
	     &pos->member != (head);					\
	     pos = tmp,							\
	     tmp = liftoff_container_of(pos->member.next, tmp, member))

#endif
