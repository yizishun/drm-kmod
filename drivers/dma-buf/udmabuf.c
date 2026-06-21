/*-
 * Copyright (c) 2026 Zishun Yi, Aymeric Wibo
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sys/param.h>
#include <sys/mman.h>
#include <sys/rwlock.h>

#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <linux/dma-buf.h>
#include "udmabuf.h" /* XXX: where should this header be placed? */

#include <netlink/netlink.h>
#include <netlink/netlink_ctl.h>
#include <netlink/netlink_generic.h>
#include <netlink/netlink_message_parser.h>

#undef file
#undef fget

MALLOC_DEFINE(M_UDMABUF, "udmabuf", "udmabuf resources");

struct nl_udmabuf_create_parsed {
	uint32_t memfd;
	uint32_t flags;
	uint64_t offset;
	uint64_t size;
};
#define	_OUT(_field)	offsetof(struct nl_udmabuf_create_parsed, _field)

static const struct nlattr_parser nla_p_create[] = {
	{ .type = UDMABUF_ATTR_MEMFD, .off = _OUT(memfd), .cb = nlattr_get_uint32 },
	{ .type = UDMABUF_ATTR_FLAGS, .off = _OUT(flags), .cb = nlattr_get_uint32 },
	{ .type = UDMABUF_ATTR_OFFSET, .off = _OUT(offset), .cb = nlattr_get_uint64 },
	{ .type = UDMABUF_ATTR_SIZE, .off = _OUT(size), .cb = nlattr_get_uint64 },
};
#undef _OUT
NL_DECLARE_PARSER(udmabuf_create_parser, struct genlmsghdr, nlf_p_empty, nla_p_create);

struct nl_udmabuf_list_parsed {
	uint32_t flags;
	struct nlattr *list;
};
#define	_OUT(_field)	offsetof(struct nl_udmabuf_list_parsed, _field)

static const struct nlattr_parser nla_p_list[] = {
	{ .type = UDMABUF_ATTR_FLAGS, .off = _OUT(flags), .cb = nlattr_get_uint32 },
	{ .type = UDMABUF_ATTR_LISTS, .off = _OUT(list), .cb = nlattr_get_nla },
};
#undef _OUT
NL_DECLARE_PARSER(udmabuf_list_parser, struct genlmsghdr, nlf_p_empty, nla_p_list);

struct nl_udmabuf_item_parse {
	uint32_t memfd;
	uint64_t offset;
	uint64_t size;
};
#define	_OUT(_field)	offsetof(struct nl_udmabuf_item_parse, _field)

static const struct nlattr_parser nla_p_item[] = {
	{ .type = UDMABUF_ATTR_MEMFD, .off = _OUT(memfd), .cb = nlattr_get_uint32 },
	{ .type = UDMABUF_ATTR_OFFSET, .off = _OUT(offset), .cb = nlattr_get_uint64 },
	{ .type = UDMABUF_ATTR_SIZE, .off = _OUT(size), .cb = nlattr_get_uint64 },
};
#undef _OUT
NL_DECLARE_PARSER(udmabuf_item_parser, struct genlmsghdr, nlf_p_empty, nla_p_item);

struct udmabuf_args {
	uint32_t count;
	uint32_t flags;
	struct nl_udmabuf_item_parse* items;
};
#define make_udmabuf_args(_count, _flags, _items) \
	(struct udmabuf_args) { \
		.count = (_count), \
		.flags = (_flags), \
		.items = (_items) \
	}

static long
udmabuf_export(struct udmabuf_args args, int *fd, struct thread *td)
{
	int err = 0;
	return (err);
}

static int
udmabuf_ret_fd(struct nlmsghdr *hdr, struct nl_pstate *npt,
	int cmd, int fd)
{
	bool ok;
	struct genlmsghdr *ghdr_new;
	struct nl_writer *nw = npt->nw;

	/* Create the reply and the genl header */
	ok = nlmsg_reply(nw, hdr, sizeof(struct genlmsghdr));
	if (!ok)
		goto err_nomem;
	
	ghdr_new = nlmsg_reserve_object(nw, struct genlmsghdr);
	ghdr_new->cmd = cmd;
	ghdr_new->version = 0;
	ghdr_new->reserved = 0;

	/* and add fd payload */
	ok = nlattr_add_u32(nw, UDMABUF_ATTR_DMABUF, fd);
	if (!ok)
		goto err_nomem;

	/* then end */
	ok = nlmsg_end(nw);
	if (!ok)
		goto err_nomem;

	return (0);

err_nomem:
	nlmsg_abort(nw);
	return (ENOMEM);
}

static int
udmabuf_create(struct nlmsghdr *hdr, struct nl_pstate *npt)
{
	int err = 0;
	int fd = -1;
	struct udmabuf_args args;
	struct nl_udmabuf_create_parsed create;
	struct nl_udmabuf_item_parse item_single; // single item in stack to speed up.

	/* We need to give a default value. */
	memset(&create, 0, sizeof(create));
	
	err = nl_parse_nlmsg(hdr, &udmabuf_create_parser, npt, &create);
	if (err != 0)
		return err;

	item_single.memfd = create.memfd;
	item_single.offset = create.offset;
	item_single.size = create.size;

	args = make_udmabuf_args(1, create.flags, &item_single);

	err = udmabuf_export(args, &fd, curthread);
	if (err != 0)
		return (err);

	err = udmabuf_ret_fd(hdr, npt, UDMABUF_CMD_CREATE_LIST, fd);
	if (err != 0){
		kern_close(curthread, fd);
		return (err);
	}

	return (err);
}

static int
udmabuf_create_list(struct nlmsghdr *hdr, struct nl_pstate *npt)
{
	int err = 0;
	int fd = -1;
	size_t list_size_with_hdr, rem;
	size_t list_size;
	uint32_t count = 0;
	struct nlattr *attr;
	struct udmabuf_args args;
	struct nl_udmabuf_list_parsed create_list;
	struct nl_udmabuf_item_parse *items;

	/* First, parse the outer list*/
	err = nl_parse_nlmsg(hdr, &udmabuf_list_parser, npt, &create_list);
	if (err != 0)
		return err;
	if (create_list.list == NULL) {
		nlmsg_report_err_msg(npt, "List is empty");
		return (EINVAL);
	}

	/* Get the payload list count and size */
	list_size_with_hdr = NLA_DATA_LEN(create_list.list);
	rem = list_size_with_hdr;
	NLA_FOREACH(attr, NLA_DATA(create_list.list), rem) {
		if (NLA_TYPE(attr) != UDMABUF_ATTR_ITEM)
			return (EINVAL);
		count ++;
	}
	if (count == 0 || count > ITEM_COUNT_LIMIT) {
		nlmsg_report_err_msg(npt, "Invalid item count");
		return (EINVAL);
	}
	list_size = count * sizeof(struct nl_udmabuf_item_parse);

	items = malloc(list_size, M_UDMABUF, M_WAITOK | M_ZERO);

	/* Parse every single item */
	int i = 0;
	rem = list_size_with_hdr;
	NLA_FOREACH(attr, NLA_DATA(create_list.list), rem) {
		err = nl_parse_nested(attr, &udmabuf_item_parser, npt, &items[i]);
		if (err)
			goto err_free_list;
		i++;
	}

	args = make_udmabuf_args(count, create_list.flags, items);

	err = udmabuf_export(args, &fd, curthread);
	if (err != 0)
		goto err_free_list;

	err = udmabuf_ret_fd(hdr, npt, UDMABUF_CMD_CREATE_LIST, fd);
	if (err != 0){
		kern_close(curthread, fd);
		goto err_free_list;
	}

	free(items, M_UDMABUF);
	return (err);

err_free_list:
	if (items != NULL)
		free(items, M_UDMABUF);
	return (err);
}

static const struct nlhdr_parser *all_parsers[] = {
	&udmabuf_create_parser,
	&udmabuf_list_parser,
	&udmabuf_item_parser
};

static const struct genl_cmd udmabuf_cmds[] = {
	{
		.cmd_num = UDMABUF_CMD_CREATE,
		.cmd_name = "UDMABUF_CMD_CREATE",
		.cmd_cb = udmabuf_create,
		.cmd_flags = GENL_CMD_CAP_DO,
	},
	{
		.cmd_num = UDMABUF_CMD_CREATE_LIST,
		.cmd_name = "UDMABUF_CMD_CREATE_LIST",
		.cmd_cb = udmabuf_create_list,
		.cmd_flags = GENL_CMD_CAP_DO,
	},
};

static int family_id;
static int
udmabuf_load(void)
{
	bool ret __diagused;

	NL_VERIFY_PARSERS(all_parsers);
	family_id = genl_register_family(UDMABUF_FAMILY_NAME, 0, 1, UDMABUF_CMD_MAX);
	MPASS(family_id != 0);

	ret = genl_register_cmds(family_id, udmabuf_cmds, nitems(udmabuf_cmds));
	MPASS(ret);
	return 0;
}

static int
udmabuf_unload(void)
{
	genl_unregister_family(family_id);
	return (0);
}

static int
udmabuf_modevent(module_t mod, int cmd, void *arg __unused)
{
	switch (cmd) {
	case MOD_LOAD:
		return(udmabuf_load());
	case MOD_UNLOAD:
		return(udmabuf_unload());
	default:
		return(EOPNOTSUPP);
	}
}

static moduledata_t udmabuf_mod = {
	.name = "udmabuf",
	.evhand = udmabuf_modevent,
	.priv = NULL
};

DECLARE_MODULE(udmabuf, udmabuf_mod, SI_SUB_DRIVERS, SI_ORDER_ANY);
MODULE_VERSION(udmabuf, 1);
MODULE_DEPEND(udmabuf, dmabuf, 1, 1, 1);
MODULE_DEPEND(udmabuf, linuxkpi, 1, 1, 1);
MODULE_DEPEND(udmabuf, netlink, 1, 1, 1);
