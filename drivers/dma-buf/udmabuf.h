/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef _UAPI_LINUX_UDMABUF_H
#define _UAPI_LINUX_UDMABUF_H

#define UDMABUF_FLAGS_CLOEXEC	0x01

/* genetlink definitions */
#define UDMABUF_FAMILY_NAME "udmabuf"

/* commands */
enum {
	UDMABUF_CMD_UNSPEC	= 0,
	UDMABUF_CMD_CREATE	= 1,
	UDMABUF_CMD_CREATE_LIST	= 2,
	__UDMABUF_CMD_MAX,
};
#define	UDMABUF_CMD_MAX	(__UDMABUF_CMD_MAX - 1)

enum udmabuf_attr_type_t {
	UDMABUF_ATTR_UNSPEC,
	UDMABUF_ATTR_MEMFD	= 1,	/* u32:  */
	UDMABUF_ATTR_FLAGS	= 2,	/* u32:  */
	UDMABUF_ATTR_OFFSET	= 3,	/* u64:  */
	UDMABUF_ATTR_SIZE	= 4,	/* u64:  */
	UDMABUF_ATTR_LISTS	= 5,	/* nested: */
	UDMABUF_ATTR_ITEM	= 6, 	/* nested: */
	UDMABUF_ATTR_DMABUF	= 7,	/* u32: reply dmabuf fd */
};

#endif /* _UAPI_LINUX_UDMABUF_H */
