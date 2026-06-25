// TODO: License
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

//TODO: make it configurable
#define ITEM_COUNT_LIMIT 1024U
#define TOTAL_SIZE_LIMIT_MB 64U

#define PAGE_LIMIT (((uint64_t)TOTAL_SIZE_LIMIT_MB * 1024 * 1024) >> PAGE_SHIFT)

/* avoid polluted by linux PAGE_MASK */
#define UDMABUF_PAGE_MASK (PAGE_SIZE - 1)

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

struct udmabuf {
	vm_pindex_t count;
	vm_page_t* pages;
};

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

static void udmabuf_release(struct dma_buf *buf);
static struct sg_table * udmabuf_map(struct dma_buf_attachment *,
						enum dma_data_direction);
static void udmabuf_unmap(struct dma_buf_attachment *,
					struct sg_table *,
					enum dma_data_direction);
static int udmabuf_mmap(struct dma_buf *, struct vm_area_struct *vma);
static int udmabuf_vmap(struct dma_buf *dmabuf, struct iosys_map *map);
static void udmabuf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map);
static int udmabuf_begin_cpu(struct dma_buf *, enum dma_data_direction);
static int udmabuf_end_cpu(struct dma_buf *, enum dma_data_direction);

static void udmabuf_unwire_pages(struct udmabuf *ubuf);

static struct dma_buf_ops udmabuf_dmabuf_ops = {
	.map_dma_buf = udmabuf_map,
	.unmap_dma_buf = udmabuf_unmap,
	.release = udmabuf_release,
	.mmap = udmabuf_mmap,
	.vmap = udmabuf_vmap,
	.vunmap = udmabuf_vunmap,
	.begin_cpu_access = udmabuf_begin_cpu,
	.end_cpu_access = udmabuf_end_cpu,

};

static struct sg_table * udmabuf_map(struct dma_buf_attachment *attachment,
						enum dma_data_direction dir)
{
	int err = 0;
	struct sg_table *sgt;
	struct udmabuf *ubuf;
	
	ubuf = attachment->dmabuf->priv;
	if (ubuf == NULL)
		return (ERR_PTR(-EINVAL));

	sgt = malloc(sizeof(struct sg_table), M_UDMABUF, M_NOWAIT|M_ZERO);
	if (sgt == NULL)
		return (ERR_PTR(-ENOBUFS));

	err = sg_alloc_table_from_pages(sgt, ubuf->pages, ubuf->count, 0,
	    ubuf->count << PAGE_SHIFT, GFP_KERNEL);
	if (err != 0) {
		free(sgt, M_UDMABUF);
		return (ERR_PTR(err));
	}

	err = dma_map_sgtable(attachment->dev, sgt, dir, 0);
	if (err != 0) {
		sg_free_table(sgt);
		free(sgt, M_UDMABUF);
		return (ERR_PTR(err));
	}
	return sgt;
}

static void udmabuf_unmap(struct dma_buf_attachment *attachment,
					struct sg_table *sgt,
					enum dma_data_direction dir)
{
	dma_unmap_sgtable(attachment->dev, sgt, dir, 0);
	sg_free_table(sgt);
	free(sgt, M_UDMABUF);
}

static int udmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	// vm_insert_pages           [linuxkpi] (not supported) (Optional)

	// vm_flags_set              [linuxkpi]
	// page_to_pfn               [linuxkpi]
	// vmf_insert_pfn            [linuxkpi] (not supported)
	// vmf_insert_pfn_prot       [linuxkpi]

	return 0;
}

static int udmabuf_vmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	// dma_resv_assert_held      [linuxkpi]
	// vmap			     [linuxkpi] (Optional)
	// vm_map_ram                [linuxkpi] (not supported)
	// iosys_map_set_vaddr       [linuxkpi]
	return 0;
}

static void udmabuf_vunmap(struct dma_buf *dmabuf, struct iosys_map *map)
{
	// dma_resv_assert_held      [linuxkpi]
	// vunmap                    [linuxkpi] (Optional)
	// vm_unmap_ram              [linuxkpi] (not supported)
	
}

static int udmabuf_begin_cpu(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	// dma_sync_sgtable_for_cpu   [linuxkpi] (not supported)
	return 0;
}

static int udmabuf_end_cpu(struct dma_buf *dmabuf, enum dma_data_direction dir)
{
	// dma_sync_sgtable_for_device[linuxkpi] (not supported)
	return 0;
}

static void
udmabuf_release(struct dma_buf *buf)
{
	struct udmabuf *ubuf = buf->priv;

	udmabuf_unwire_pages(ubuf);
	free(ubuf->pages, M_UDMABUF);
	free(ubuf, M_UDMABUF);
}

static int
udmabuf_grab_wire_pages(struct udmabuf *ubuf, struct shmfd *shmfd, vm_pindex_t pgstart, vm_pindex_t pgsize)
{
	int err = 0;
	vm_pindex_t i;
	vm_pindex_t count = ubuf->count;
	vm_page_t m;

	VM_OBJECT_WLOCK(shmfd->shm_object);
	if (pgstart + pgsize > shmfd->shm_object->size) {
		err = EINVAL;
		goto err_unlock;
	}

	for (i = pgstart; i < pgstart + pgsize; i++) {
		err = vm_page_grab_valid(&m, shmfd->shm_object, i, 
		    VM_ALLOC_WIRED | VM_ALLOC_NOBUSY | VM_ALLOC_WAITOK);
		if (err != 0)
			goto err_unlock;

		ubuf->pages[count] = m;
		count++;
	}

err_unlock:
	ubuf->count = count;
	VM_OBJECT_WUNLOCK(shmfd->shm_object);
	return (err);
}

static void
udmabuf_unwire_pages(struct udmabuf *ubuf)
{
	for (; ubuf->count > 0;) {
		vm_page_unwire(ubuf->pages[--ubuf->count],
		    PQ_INACTIVE);
	}
}

static int
udmabuf_args_check(struct udmabuf_args args, vm_pindex_t *nr_pages)
{
	int i;
	vm_pindex_t count = 0;

	if ((args.flags & ~UDMABUF_FLAGS_CLOEXEC) != 0)
		return (EINVAL);

	for (i = 0; i < args.count; i++) {
		if ((args.items[i].size & UDMABUF_PAGE_MASK) != 0)
			return (EINVAL);
		if ((args.items[i].offset & UDMABUF_PAGE_MASK) != 0)
			return (EINVAL);

		count += (args.items[i].size >> PAGE_SHIFT);
		if (count > PAGE_LIMIT)
			return (EINVAL);
	}

	if (count == 0)
		return (EINVAL);

	*nr_pages = count;
	return (0);
}

static long
udmabuf_export(struct udmabuf_args args, int *fd, struct thread *td)
{
	DEFINE_DMA_BUF_EXPORT_INFO(exp_info);
	int i;
	int err = 0;
	vm_pindex_t nr_pages = 0;
	vm_pindex_t pgstart, pgsize;
	struct udmabuf *ubuf;
	struct dma_buf *dmabuf;
	struct shmfd *shmfd;
	void *rl_cookie;
	struct file *memfd_file;
	cap_rights_t rights;

	/* args validation check. (args.count is checked in ioctl) */
	err = udmabuf_args_check(args, &nr_pages);
	if (err != 0)
		return (err);

	ubuf = malloc(sizeof(struct udmabuf), M_UDMABUF,
	    M_WAITOK | M_ZERO);

	ubuf->pages = mallocarray(nr_pages, sizeof(vm_page_t), 
	    M_UDMABUF, M_WAITOK);

	for (i = 0; i < args.count; i++) {
		CAP_ALL(&rights);
		err = fget(td, args.items[i].memfd, &rights, &memfd_file);
		if (err != 0)
			goto err;

		if (memfd_file->f_type != DTYPE_SHM) {
			fdrop(memfd_file, td);
			err = EINVAL;
			goto err;
		}
		shmfd = memfd_file->f_data;

		rl_cookie = rangelock_rlock(&shmfd->shm_rl, 0, OFF_MAX);

		if ((shmfd->shm_seals & F_SEAL_SHRINK) == 0 ||
		    (shmfd->shm_seals & F_SEAL_WRITE) != 0) {
			err = EINVAL;
			goto err_unlock;
		}

		pgstart = args.items[i].offset >> PAGE_SHIFT;
		pgsize = args.items[i].size >> PAGE_SHIFT;
		err = udmabuf_grab_wire_pages(ubuf, shmfd, pgstart, pgsize);
err_unlock:
		rangelock_unlock(&shmfd->shm_rl, rl_cookie);
		fdrop(memfd_file, td);
		if (err != 0)
			goto err;
	}
	KASSERT(nr_pages == ubuf->count, "nr_pages should equal to count if all work fine");

	exp_info.ops = &udmabuf_dmabuf_ops;
	exp_info.size = ubuf->count << PAGE_SHIFT;
	exp_info.flags = O_RDWR;
	exp_info.priv = ubuf;

	dmabuf = dma_buf_export(&exp_info);
	if (IS_ERR(dmabuf)) {
		err = PTR_ERR(dmabuf);
		goto err;
	}

	*fd = dma_buf_fd(dmabuf,
	    args.flags & UDMABUF_FLAGS_CLOEXEC ? O_CLOEXEC : 0);
	if (*fd < 0) {
		err = -*fd;
		dma_buf_put(dmabuf);
	}

	return (err);

err:
	udmabuf_unwire_pages(ubuf);
	free(ubuf->pages, M_UDMABUF);
	free(ubuf, M_UDMABUF);
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
