/*-
 * Copyright (c) 2026 Zishun Yi, Aymeric Wibo
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */
#include <sys/param.h>
#include <sys/module.h>
#include <sys/kernel.h>
#include <sys/conf.h>
#include <sys/uio.h>
#include <sys/malloc.h>
#include <sys/fcntl.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sysproto.h>
#include <sys/capsicum.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/limits.h>
#include <sys/rangelock.h>
#include <sys/kassert.h>
#include <vm/vm.h>
#include <vm/vm_object.h>
#include <vm/vm_page.h>

#include <linux/dma-buf.h>
#include <uapi/linux/udmabuf.h>

#undef file
#undef fget

//TODO: make it configurable
#define ITEM_COUNT_LIMIT 1024U
#define TOTAL_SIZE_LIMIT_MB 64U

#define PAGE_LIMIT (((uint64_t)TOTAL_SIZE_LIMIT_MB * 1024 * 1024) >> PAGE_SHIFT)

// avoid polluted by linux PAGE_MASK
#define UDMABUF_PAGE_MASK (PAGE_SIZE - 1)

MALLOC_DEFINE(M_UDMABUF, "udmabuf", "udmabuf resources");

static struct cdev *udmabuf_dev;

struct udmabuf {
	vm_pindex_t count;	
	vm_page_t* pages;
};

struct udmabuf_args {
	uint32_t count;
	uint32_t flags;
	struct udmabuf_create_item* items;
};
#define make_udmabuf_args(_count, _flags, _items) \
	(struct udmabuf_args) { \
		.count = (_count), \
		.flags = (_flags), \
		.items = (_items) \
	}

static d_ioctl_t udmabuf_ioctl;

static struct cdevsw udmabuf_cdevsw = {
	.d_version = D_VERSION,
	.d_name = "udmabuf",
	.d_ioctl = udmabuf_ioctl,
};

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
	// sg_alloc_table_from_pages [linuxkpi]
	// dma_map_sgtable 	     [linuxkpi]
	// sg_free_table	     [linuxkpi]
	return NULL;
}

static void udmabuf_unmap(struct dma_buf_attachment *attachment,
 					struct sg_table *sg,
 					enum dma_data_direction dir)
{
	// dma_unmap_sgtable         [linuxkpi]
	// sg_free_table	     [linuxkpi]
}

static int udmabuf_mmap(struct dma_buf *dmabuf, struct vm_area_struct *vma)
{
	// vm_insert_pages           [linuxkpi] (not supported) (Optional)

	// vm_flags_set              [linuxkpi]
	// page_to_pfn               [linuxkpi]
	// vmf_insert_pfn            [linuxkpi] (not supported) (Optional)
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
		return err;

	ubuf = malloc(sizeof(struct udmabuf), M_UDMABUF,
	    M_WAITOK | M_ZERO);

	ubuf->pages = mallocarray(nr_pages, sizeof(vm_page_t), 
	    M_UDMABUF, M_WAITOK);

	for (i = 0; i < args.count; i++) {
		CAP_ALL(&rights); //TODO: consider it
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
udmabuf_ioctl(struct cdev *dev, u_long cmd, caddr_t data, int fflag,
    struct thread *td)
{
	int err = 0;
	int fd = -1;
	size_t list_size;
	caddr_t uptr;
	struct udmabuf_args args;
	struct udmabuf_create_list* create_list;
	struct udmabuf_create* create;
	struct udmabuf_create_item item_single; // single item in stack to speed up.
	struct udmabuf_create_item *items_list = NULL;
	
	/* copy the flexible array of udmabuf_create_list in userspace */
	if ((uptr = td->td_fpop->f_ioctl_uptr) == NULL)
		return (EINVAL);

	/* Both create and create list need write flag to be set */
	if ((fflag & FWRITE) == 0)
		return (EPERM);

	switch (cmd) {
	case UDMABUF_CREATE:
		create = (struct udmabuf_create *)data;

		item_single.memfd = create->memfd;
		item_single.offset = create->offset;
		item_single.size = create->size;

		args = make_udmabuf_args(1, create->flags, &item_single);

		err = udmabuf_export(args, &fd, td);
		break;
	case UDMABUF_CREATE_LIST:
		create_list = (struct udmabuf_create_list *)data;

		/* check count limit */
		if (create_list->count > ITEM_COUNT_LIMIT)
			return (EINVAL);
		
		list_size = sizeof(struct udmabuf_create_item) * create_list->count;
		items_list = mallocarray(create_list->count,
		    sizeof(struct udmabuf_create_item), M_UDMABUF, M_WAITOK);

		err = copyin(uptr + sizeof(struct udmabuf_create_list),
		    items_list, list_size);
		if (err != 0)
			goto err_free_list;

		args = make_udmabuf_args(create_list->count, create_list->flags,
		    items_list);

		err = udmabuf_export(args, &fd, td);
		break;
	default:
		return(ENOTTY);
	}
	
	/* Linux Compatible */
	td->td_retval[0] = fd;

err_free_list:
	if (items_list != NULL)
		free(items_list, M_UDMABUF);
	return (err);
}

static int
udmabuf_load(void)
{
	int err;
	struct make_dev_args args;

	printf("udmabuf load\n");
	make_dev_args_init(&args);
	args.mda_devsw = &udmabuf_cdevsw;
	args.mda_gid = GID_WHEEL;
	args.mda_uid = UID_ROOT;
	args.mda_mode = 0660;
	args.mda_flags = MAKEDEV_CHECKNAME | MAKEDEV_WAITOK;

	err = make_dev_s(&args, &udmabuf_dev, "udmabuf");
	if (err)
		printf("udmabuf: create udmabuf dev node fail with %d\n", err);

	return (err);
}

static int
udmabuf_unload(void)
{
	printf("udmabuf unload\n");
	if (udmabuf_dev != NULL)
		destroy_dev(udmabuf_dev);
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
