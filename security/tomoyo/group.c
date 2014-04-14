/*
 * security/tomoyo/group.c
 *
 * Copyright (C) 2005-2011  NTT DATA CORPORATION
 */

#include <linux/slab.h>
#include "common.h"

static bool tomoyo_same_path_group(const struct tomoyo_acl_head *a,
				   const struct tomoyo_acl_head *b)
{
	return container_of(a, struct tomoyo_path_group, head)->member_name ==
		container_of(b, struct tomoyo_path_group, head)->member_name;
}

static bool tomoyo_same_number_group(const struct tomoyo_acl_head *a,
				     const struct tomoyo_acl_head *b)
{
	return !memcmp(&container_of(a, struct tomoyo_number_group, head)
		       ->number,
		       &container_of(b, struct tomoyo_number_group, head)
		       ->number,
		       sizeof(container_of(a, struct tomoyo_number_group, head)
			      ->number));
}

static bool tomoyo_same_address_group(const struct tomoyo_acl_head *a,
				      const struct tomoyo_acl_head *b)
{
	const struct tomoyo_address_group *p1 = container_of(a, typeof(*p1),
							     head);
	const struct tomoyo_address_group *p2 = container_of(b, typeof(*p2),
							     head);

	return tomoyo_same_ipaddr_union(&p1->address, &p2->address);
}

int tomoyo_write_group(struct tomoyo_acl_param *param, const u8 type)
{
	struct tomoyo_group *group = tomoyo_get_group(param, type);
	int error = -EINVAL;
	if (!group)
		return -ENOMEM;
	param->list = &group->member_list;
	if (type == TOMOYO_PATH_GROUP) {
		struct tomoyo_path_group e = { };
		e.member_name = tomoyo_get_name(tomoyo_read_token(param));
		if (!e.member_name) {
			error = -ENOMEM;
			goto out;
		}
		error = tomoyo_update_policy(&e.head, sizeof(e), param,
					  tomoyo_same_path_group);
		tomoyo_put_name(e.member_name);
	} else if (type == TOMOYO_NUMBER_GROUP) {
		struct tomoyo_number_group e = { };
		if (param->data[0] == '@' ||
		    !tomoyo_parse_number_union(param, &e.number))
			goto out;
		error = tomoyo_update_policy(&e.head, sizeof(e), param,
					  tomoyo_same_number_group);
	} else {
		struct tomoyo_address_group e = { };

		if (param->data[0] == '@' ||
		    !tomoyo_parse_ipaddr_union(param, &e.address))
			goto out;
		error = tomoyo_update_policy(&e.head, sizeof(e), param,
					     tomoyo_same_address_group);
	}
out:
	tomoyo_put_group(group);
	return error;
}

const struct tomoyo_path_info *
tomoyo_path_matches_group(const struct tomoyo_path_info *pathname,
			  const struct tomoyo_group *group)
{
	struct tomoyo_path_group *member;
	list_for_each_entry_rcu(member, &group->member_list, head.list) {
		if (member->head.is_deleted)
			continue;
		if (!tomoyo_path_matches_pattern(pathname, member->member_name))
			continue;
		return member->member_name;
	}
	return NULL;
}

bool tomoyo_number_matches_group(const unsigned long min,
				 const unsigned long max,
				 const struct tomoyo_group *group)
{
	struct tomoyo_number_group *member;
	bool matched = false;
	list_for_each_entry_rcu(member, &group->member_list, head.list) {
		if (member->head.is_deleted)
			continue;
		if (min > member->number.values[1] ||
		    max < member->number.values[0])
			continue;
		matched = true;
		break;
	}
	return matched;
}

bool tomoyo_address_matches_group(const bool is_ipv6, const __be32 *address,
				  const struct tomoyo_group *group)
{
	struct tomoyo_address_group *member;
	bool matched = false;
	const u8 size = is_ipv6 ? 16 : 4;

	list_for_each_entry_rcu(member, &group->member_list, head.list) {
		if (member->head.is_deleted)
			continue;
		if (member->address.is_ipv6 != is_ipv6)
			continue;
		if (memcmp(&member->address.ip[0], address, size) > 0 ||
		    memcmp(address, &member->address.ip[1], size) > 0)
			continue;
		matched = true;
		break;
	}
	return matched;
}
