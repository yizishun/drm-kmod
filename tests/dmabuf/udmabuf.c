// TODO: add license

/*
 * The FreeBSD kernel udmabuf driver now uses Generic Netlink instead of
 * ioctl()
 */

#include <stdio.h>
#include <fcntl.h>

#include <sys/mman.h>

#include <atf-c.h>
#include <udmabuf.h>

#include <netlink/netlink.h>
#include <netlink/netlink_generic.h>
#include <netlink/netlink_snl.h>
#include <netlink/netlink_snl_generic.h>

#define NUM_PAGES       4
#define NUM_ENTRIES     4
#define MEMFD_SIZE      1024

unsigned long page_size;

struct udmabuf_handler {
	struct snl_state ss;
	uint16_t family;
};

struct nl_parsed_reply {
	uint32_t dmabuf_fd;
};

static const struct snl_field_parser nlf_p_empty[] = {};

#define _OUT(_field) offsetof(struct nl_parsed_reply, _field)
static const struct snl_attr_parser ap_reply[] = {
	{ .type = UDMABUF_ATTR_DMABUF, .off = _OUT(dmabuf_fd), .cb = snl_attr_get_uint32 },
};
#undef _OUT
SNL_DECLARE_PARSER(reply_parser, struct genlmsghdr, nlf_p_empty, ap_reply);

struct udmabuf_uapi_item {
	int memfd;
	uint64_t offset;
	uint64_t size;
};

static inline void
init_udmabuf_dev(struct udmabuf_handler *hdl)
{
	int val = 1;
	socklen_t optlen = sizeof(val);

	ATF_REQUIRE(snl_init(&hdl->ss, NETLINK_GENERIC));
	ATF_REQUIRE(setsockopt(hdl->ss.fd, SOL_NETLINK, NETLINK_SND_SYNC, &val, optlen) != -1);
	ATF_REQUIRE((hdl->family = snl_get_genl_family(&hdl->ss, UDMABUF_FAMILY_NAME)) != 0);
}

static int
init_memfd(off_t size, bool hpage)
{
	int memfd;
	unsigned int flags = MFD_ALLOW_SEALING;

	if (hpage)
		flags |= MFD_HUGETLB;

	memfd = memfd_create("udmabuf-nl-test", flags);
	if (memfd == -1){
		if (errno == ENOTTY && hpage)
			atf_tc_skip("large page requests are not supported on the current platform");
		else
			atf_tc_fail("memfd_create failed: %d%s", errno, strerror(errno));
	}

	ATF_REQUIRE(fcntl(memfd, F_ADD_SEALS, F_SEAL_SHRINK) >= 0);
	ATF_REQUIRE(ftruncate(memfd, size) != -1);

	return (memfd);
}

static void
write_to_memfd(void *addr, off_t size, char chr)
{
	char *p = addr;
	for (off_t i = 0; i < size; i += page_size) {
		p[i] = chr;
	}
}

static int
compare_chunks(void *addr1, void *addr2, off_t memfd_size)
{
	char *p1 = addr1;
	char *p2 = addr2;
	off_t memfd_chunk_size = memfd_size / NUM_ENTRIES;
	off_t ubuf_chunk_size = NUM_PAGES * getpagesize();

	for (int i = 0; i < NUM_ENTRIES; i++) {
		for (int j = 0; j < NUM_PAGES; j++) {
			off_t off1 = (i * memfd_chunk_size) + (j * getpagesize());
			off_t off2 = (i * ubuf_chunk_size) + (j * getpagesize());
			
			if (p1[off1] != p2[off2]) {
				return (-1);
			}
		}
	}
	return (0);
}

/* Helper to wait for and parse the Netlink ACK/Reply */
static int
nl_get_reply_fd(struct udmabuf_handler *hdl, uint32_t seq)
{
	struct nlmsghdr *hdr;
	int out_fd = -1;
	int out_err = 0;
	struct nl_parsed_reply reply = { .dmabuf_fd = -1 };

	while ((hdr = snl_read_message(&hdl->ss)) != NULL) {
		if (hdr->nlmsg_seq != seq)
			continue;

		if (hdr->nlmsg_type == NLMSG_ERROR) {
			struct nlmsgerr *err = (struct nlmsgerr *)NLMSG_DATA(hdr);
			if (err->error != 0) {
				out_err = err->error;
			}
		} else if (hdr->nlmsg_type == hdl->family) {
			ATF_REQUIRE(snl_parse_nlmsg(&hdl->ss, hdr, &reply_parser, &reply));
			out_fd = reply.dmabuf_fd;
		}

		if (hdr->nlmsg_type == NLMSG_DONE || hdr->nlmsg_type == NLMSG_ERROR)
			break;
	}

	if (out_err != 0) {
		return (-1);
	}
	return (out_fd);
}

static int
create_udmabuf_list(struct udmabuf_handler *hdl, struct udmabuf_uapi_item *items,
    int nitems, uint32_t flags)
{
	struct snl_writer nw;
	struct nlmsghdr *hdr;
	int fd, off_list, off_item;

	snl_init_writer(&hdl->ss, &nw);
	ATF_REQUIRE(snl_create_genl_msg_request(&nw, hdl->family, UDMABUF_CMD_CREATE_LIST) != NULL);
	ATF_REQUIRE(snl_add_msg_attr_u32(&nw, UDMABUF_ATTR_FLAGS, flags));

	/* Start outer list container */
	ATF_REQUIRE((off_list = snl_add_msg_attr_nested(&nw, UDMABUF_ATTR_LISTS)) != 0);
	for (int i = 0; i < nitems; i++) {
		ATF_REQUIRE((off_item = snl_add_msg_attr_nested(&nw, UDMABUF_ATTR_ITEM)) != 0);
		ATF_REQUIRE(snl_add_msg_attr_u32(&nw, UDMABUF_ATTR_MEMFD, items[i].memfd));
		ATF_REQUIRE(snl_add_msg_attr_u64(&nw, UDMABUF_ATTR_OFFSET, items[i].offset));
		ATF_REQUIRE(snl_add_msg_attr_u64(&nw, UDMABUF_ATTR_SIZE, items[i].size));
		snl_end_attr_nested(&nw, off_item);
	}
	snl_end_attr_nested(&nw, off_list);
	ATF_REQUIRE((hdr = snl_finalize_msg(&nw)) != NULL);
	ATF_REQUIRE(snl_send_msgs(&nw));

	uint32_t seq = hdr->nlmsg_seq;

	fd = nl_get_reply_fd(hdl, seq);
	return fd;
}

static int
create_udmabuf(struct udmabuf_handler *hdl, struct udmabuf_uapi_item *items, uint32_t flags)
{
	struct snl_writer nw;
	struct nlmsghdr *hdr;
	int fd, off_list, off_item;

	snl_init_writer(&hdl->ss, &nw);
	ATF_REQUIRE(snl_create_genl_msg_request(&nw, hdl->family, UDMABUF_CMD_CREATE) != NULL);

	/* Start outer list container */
	ATF_REQUIRE(snl_add_msg_attr_u32(&nw, UDMABUF_ATTR_FLAGS, flags));
	ATF_REQUIRE(snl_add_msg_attr_u32(&nw, UDMABUF_ATTR_MEMFD, items->memfd));
	ATF_REQUIRE(snl_add_msg_attr_u64(&nw, UDMABUF_ATTR_OFFSET, items->offset));
	ATF_REQUIRE(snl_add_msg_attr_u64(&nw, UDMABUF_ATTR_SIZE, items->size));
	ATF_REQUIRE((hdr = snl_finalize_msg(&nw)) != NULL);
	ATF_REQUIRE(snl_send_msgs(&nw));

	uint32_t seq = hdr->nlmsg_seq;

	fd = nl_get_reply_fd(hdl, seq);
	printf("user get dmabuf fd: %d\n", fd);
	return fd;
}

ATF_TC(basic);
ATF_TC_HEAD(basic, tc)
{
	atf_tc_set_md_var(tc, "require.kmods", "dmabuf udmabuf");
}
ATF_TC_BODY(basic, tc)
{
	struct udmabuf_handler hdl;
	int memfd, flag;
	size_t memfd_size = getpagesize() * 4;
	struct udmabuf_uapi_item item;

	init_udmabuf_dev(&hdl);
	memfd = init_memfd(memfd_size, false);
	flag = UDMABUF_FLAGS_CLOEXEC;

	item.memfd = memfd;
	item.offset = getpagesize()/2;
	item.size = memfd_size;
	ATF_REQUIRE(create_udmabuf(&hdl, &item, flag) < 0);

	item.memfd = memfd;
	item.offset = 0;
	item.size = getpagesize()/2;
	ATF_REQUIRE(create_udmabuf(&hdl, &item, flag) < 0);

	item.memfd = 0;
	item.offset = 0;
	item.size = memfd_size;
	ATF_REQUIRE(create_udmabuf(&hdl, &item, flag) < 0);

	item.memfd = memfd;
	item.offset = 0;
	item.size = memfd_size;
	ATF_REQUIRE(create_udmabuf(&hdl, &item, flag) > 0);
}

ATF_TC(list);
ATF_TC_HEAD(list, tc)
{
	atf_tc_set_md_var(tc, "require.kmods", "dmabuf udmabuf");
}
ATF_TC_BODY(list, tc)
{
	struct udmabuf_handler hdl;
	int memfd, ubuf_fd, i;
	size_t memfd_size;
	struct udmabuf_uapi_item *items;
	void *addr1, *addr2;

	init_udmabuf_dev(&hdl);
	items = malloc(NUM_ENTRIES * sizeof(struct udmabuf_uapi_item));

	/* migration of 4k pages */
	page_size = getpagesize();
	memfd_size = MEMFD_SIZE * page_size;
	memfd = init_memfd(memfd_size, false);
	ATF_REQUIRE((addr1 = mmap(NULL, memfd_size,
	    PROT_READ|PROT_WRITE, MAP_SHARED, memfd, 0)) != MAP_FAILED);
	write_to_memfd(addr1, memfd_size, 'a');
	for (i = 0; i < NUM_ENTRIES; i++) {
		items[i].memfd = memfd;
		items[i].size = getpagesize() * NUM_PAGES;
		items[i].offset = i * (memfd_size / NUM_ENTRIES);
	}
	ATF_REQUIRE((ubuf_fd = create_udmabuf_list(&hdl, items, NUM_ENTRIES, 
	    UDMABUF_FLAGS_CLOEXEC)) > 0);
	ATF_REQUIRE((addr2 = mmap(NULL, NUM_ENTRIES * NUM_PAGES * getpagesize(),
	    PROT_READ|PROT_WRITE, MAP_SHARED, ubuf_fd, 0)) != MAP_FAILED);
	write_to_memfd(addr1, memfd_size, 'b');
	ATF_REQUIRE(compare_chunks(addr1, addr2, memfd_size) == 0);
	munmap(addr1, memfd_size);
	munmap(addr2, NUM_ENTRIES * NUM_PAGES * getpagesize());
	close(ubuf_fd);
	close(memfd);

	/* migration of 2M pages */
	page_size = getpagesize() * 512;
	memfd_size = MEMFD_SIZE * page_size;
	memfd = init_memfd(memfd_size, true);
	ATF_REQUIRE((addr1 = mmap(NULL, memfd_size,
	    PROT_READ|PROT_WRITE, MAP_SHARED, memfd, 0)) != MAP_FAILED);
	write_to_memfd(addr1, memfd_size, 'a');
	for (i = 0; i < NUM_ENTRIES; i++) {
		items[i].memfd = memfd;
		items[i].size = getpagesize() * NUM_PAGES;
		items[i].offset = i * (memfd_size / NUM_ENTRIES);
	}
	ATF_REQUIRE((ubuf_fd = create_udmabuf_list(&hdl, items, NUM_ENTRIES, 
	    UDMABUF_FLAGS_CLOEXEC)) > 0);
	ATF_REQUIRE((addr2 = mmap(NULL, NUM_ENTRIES * NUM_PAGES * getpagesize(),
	    PROT_READ|PROT_WRITE, MAP_SHARED, ubuf_fd, 0)) != MAP_FAILED);
	write_to_memfd(addr1, memfd_size, 'b');
	ATF_REQUIRE(compare_chunks(addr1, addr2, memfd_size) == 0);
	munmap(addr1, memfd_size);
	munmap(addr2, NUM_ENTRIES * NUM_PAGES * getpagesize());
	close(ubuf_fd);
	close(memfd);

	/* migration of 2M pages, we pin first before writing to memfd */
	page_size = getpagesize() * 512;
	memfd_size = MEMFD_SIZE * page_size;
	memfd = init_memfd(memfd_size, true);
	for (i = 0; i < NUM_ENTRIES; i++) {
		items[i].memfd = memfd;
		items[i].size = getpagesize() * NUM_PAGES;
		items[i].offset = i * (memfd_size / NUM_ENTRIES);
	}
	ATF_REQUIRE((ubuf_fd = create_udmabuf_list(&hdl, items, NUM_ENTRIES, 
	    UDMABUF_FLAGS_CLOEXEC)) > 0);
	ATF_REQUIRE((addr2 = mmap(NULL, NUM_ENTRIES * NUM_PAGES * getpagesize(),
	    PROT_READ|PROT_WRITE, MAP_SHARED, ubuf_fd, 0)) != MAP_FAILED);
	ATF_REQUIRE((addr1 = mmap(NULL, memfd_size,
	    PROT_READ|PROT_WRITE, MAP_SHARED, memfd, 0)) != MAP_FAILED);
	write_to_memfd(addr1, memfd_size, 'a');
	write_to_memfd(addr1, memfd_size, 'b');
	ATF_REQUIRE(compare_chunks(addr1, addr2, memfd_size) == 0);
	munmap(addr1, memfd_size);
	munmap(addr2, NUM_ENTRIES * NUM_PAGES * getpagesize());
	close(ubuf_fd);
	close(memfd);
}

ATF_TP_ADD_TCS(tp)
{
	ATF_TP_ADD_TC(tp, basic);
	ATF_TP_ADD_TC(tp, list);

	return (atf_no_error());
}
