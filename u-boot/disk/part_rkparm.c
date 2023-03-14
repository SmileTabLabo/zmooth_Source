/*
 * (C) Copyright 2017 rkparm Electronics Co., Ltd
 *
 * SPDX-License-Identifier:     GPL-2.0+
 */

#include <common.h>
#include <malloc.h>
#ifdef CONFIG_ROCKCHIP_PRELOADER_ATAGS
#include <asm/arch/rk_atags.h>
#endif

#ifdef HAVE_BLOCK_DEVICE
#define RK_PARAM_OFFSET			0x2000
#define MAX_PARAM_SIZE			(1024 * 64)

struct rkparm_param {
	u32 tag;
	u32 length;
	char params[1];
	u32 crc;
};

struct rkparm_part {
	char name[PART_NAME_LEN];
	unsigned long start;
	unsigned long size;
	struct list_head node;
};


static LIST_HEAD(parts_head);

/*
 * What's this?
 *
 * There maybe two different storage media need to use this partition driver,
 * e.g. rkand with SD card. So we need a flag info to recognize it.
 */
static int dev_num = -1;

static int rkparm_param_parse(char *param, struct list_head *parts_head,
			      struct blk_desc *dev_desc)
{
	struct rkparm_part *part;
	const char *cmdline = strstr(param, "CMDLINE:");
	const char *blkdev_parts;
	char *cmdline_end, *next, *pend;
	int len, offset = 0;
	unsigned long size, start;

	if (!cmdline) {
		debug("RKPARM: Invalid parameter part table from storage\n");
		return -EINVAL;
	}

	blkdev_parts = strstr(cmdline, "mtdparts");
	next = strchr(blkdev_parts, ':');
	cmdline_end = strstr(cmdline, "\n"); /* end by '\n' */
	*cmdline_end = '\0';
	/*
	 * 1. skip "CMDLINE:"
	 * 2. Initrd fixup: remove unused "initrd=0x...,0x...", this for
	 *    compatible with legacy parameter.txt
	 */
	env_update_filter("bootargs", cmdline + strlen("CMDLINE:"), "initrd=");

	INIT_LIST_HEAD(parts_head);
	while (next) {
		/* Skip ':' and ',' */
		next++;
		if (*next == '-') {
			size = (~0UL);
			next++;
		} else {
			size = simple_strtoul(next, &next, 16);
		}
		/* Skip '@' */
		next++;
		start = simple_strtoul(next, &next, 16);
		next++;
		pend =  strchr(next, ')');
		if (!pend)
			break;
		len = min_t(int, pend - next, PART_NAME_LEN);
		part = malloc(sizeof(*part));
		if (!part) {
			printf("out of memory\n");
			break;
		}
		if (dev_desc->if_type != IF_TYPE_RKNAND)
			offset = RK_PARAM_OFFSET;
		part->start = start + offset;
		/* Last partition use all remain space */
		if (size == (~0UL))
			size = dev_desc->lba - part->start;
		part->size = size;
		strncpy(part->name, next, len);
		part->name[len] = '\0';
		list_add_tail(&part->node, parts_head);
		next = strchr(next, ',');
	}

	dev_num = ((dev_desc->if_type << 8) + dev_desc->devnum);

	return 0;
}

static int rkparm_init_param_from_storage(struct blk_desc *dev_desc,
					  struct list_head *parts_head)
{
	struct rkparm_param *param;
	int offset = 0;
	int ret;

	param = memalign(ARCH_DMA_MINALIGN, MAX_PARAM_SIZE);
	if (!param) {
		printf("out of memory\n");
		return -ENOMEM;
	}

	if (dev_desc->if_type != IF_TYPE_RKNAND)
		offset = RK_PARAM_OFFSET;

	ret = blk_dread(dev_desc, offset, MAX_PARAM_SIZE >> 9, (ulong *)param);
	if (ret != (MAX_PARAM_SIZE >> 9)) {
		printf("%s param read fail\n", __func__);
		return -EINVAL;
	}

	return rkparm_param_parse(param->params, parts_head, dev_desc);
}

#if defined(CONFIG_ROCKCHIP_PRELOADER_ATAGS) && defined(CONFIG_DM_RAMDISK)
static int rkparm_init_param_from_atags(struct blk_desc *dev_desc,
					struct list_head *parts_head)
{
	struct rkparm_part *part;
	struct tag *t;
	u64 start, size;
	int i, len;

	if (!atags_is_available()) {
		debug("%s: can't find ATAGS\n", __func__);
		return -ENODATA;
	}

	t = atags_get_tag(ATAG_RAM_PARTITION);
	if (!t) {
		debug("%s: can't find ATAGS ramdisk partition\n", __func__);
		return -ENODATA;
	}

	INIT_LIST_HEAD(parts_head);

	for (i = 0; i < t->u.ram_part.count; i++) {
		part = malloc(sizeof(*part));
		if (!part) {
			printf("%s: out of memory\n", __func__);
			break;
		}

		len = strlen(t->u.ram_part.part[i].name) + 1;
		memcpy((char *)&part->name,
		       (char *)&t->u.ram_part.part[i].name, len);

		start = t->u.ram_part.part[i].start;
		size = t->u.ram_part.part[i].size;

		if (!IS_ALIGNED(start, dev_desc->blksz)) {
			printf("%s: '%s' addr(0x%llx) is not %ld byte aligned\n",
			       __func__, part->name, start, dev_desc->blksz);
			return -EINVAL;
		} else if (!IS_ALIGNED(size, dev_desc->blksz)) {
			printf("%s: '%s' size(0x%llx) is not %ld byte aligned\n",
			       __func__, part->name, size, dev_desc->blksz);
			return -EINVAL;
		}

		/* Convert bytes to blksz */
		part->start = start / dev_desc->blksz;
		part->size = size / dev_desc->blksz;
		list_add_tail(&part->node, parts_head);

		debug("%s: name=%s, start=0x%lx, size=0x%lx, blksz=0x%lx\n",
		      __func__, part->name, part->start,
		      part->size, dev_desc->blksz);
	}

	dev_num = ((dev_desc->if_type << 8) + dev_desc->devnum);

	return 0;
}
#endif

static int rkparm_init_param(struct blk_desc *dev_desc,
			     struct list_head *parts_head)
{
	int ret;

	ret = rkparm_init_param_from_storage(dev_desc, parts_head);
	if (ret) {
		debug("%s: failed to init param from storage\n", __func__);
#if defined(CONFIG_ROCKCHIP_PRELOADER_ATAGS) && defined(CONFIG_DM_RAMDISK)
		ret = rkparm_init_param_from_atags(dev_desc, parts_head);
		if (ret) {
			debug("%s: failed to init param from ram\n", __func__);
			return ret;
		}
#endif
	}

	if (ret)
		printf("RKPARM: Invalid parameter part table\n");

	return ret;
}

static void part_print_rkparm(struct blk_desc *dev_desc)
{
	int ret = 0;
	struct list_head *node;
	struct rkparm_part *p = NULL;
	int i = 0;

	if (list_empty(&parts_head) ||
	    (dev_num != ((dev_desc->if_type << 8) + dev_desc->devnum)))
		ret = rkparm_init_param(dev_desc, &parts_head);

	if (ret) {
		printf("%s Invalid rkparm parameter\n", __func__);
		return;
	}

	printf("Part\tStart LBA\tSize\t\tName\n");
	list_for_each(node, &parts_head) {
		p = list_entry(node, struct rkparm_part, node);
		printf("%3d\t0x%08lx\t0x%08lx\t%s\n", (i++ + 1),
		       p->start, p->size, p->name);
	}

	return;
}

static int part_get_info_rkparm(struct blk_desc *dev_desc, int idx,
		      disk_partition_t *info)
{
	struct list_head *node;
	struct rkparm_part *p = NULL;
	int part_num = 1;
	int ret = 0;

	if (idx < 1) {
		printf("%s Invalid partition no.%d\n", __func__, idx);
		return -EINVAL;
	}

	if (list_empty(&parts_head) ||
	    (dev_num != ((dev_desc->if_type << 8) + dev_desc->devnum)))
		ret = rkparm_init_param(dev_desc, &parts_head);

	if (ret) {
		printf("%s Invalid rkparm partition\n", __func__);
		return -1;
	}

	list_for_each(node, &parts_head) {
		p = list_entry(node, struct rkparm_part, node);
		if (idx == part_num)
			break;
		part_num ++;
	}

	if (part_num > idx) {
		printf("%s Invalid partition no.%d\n", __func__, idx);
		return -EINVAL;
	}

	info->start = p->start;
	info->size = p->size;
	info->blksz = dev_desc->blksz;

	sprintf((char *)info->name, "%s", p->name);
	strcpy((char *)info->type, "U-Boot");
	info->bootable = 0;

	return 0;
}

static int part_test_rkparm(struct blk_desc *dev_desc)
{
	int ret = 0;

	if (list_empty(&parts_head) ||
	    (dev_num != ((dev_desc->if_type << 8) + dev_desc->devnum)))
		ret = rkparm_init_param(dev_desc, &parts_head);
	if (ret)
		ret = -1;

	return ret;
}
/*
 * Add an 'b_' prefix so it comes before 'dos' and after 'a_efi' in the linker
 * list. We need to check EFI first, and then rkparm partition
 */
U_BOOT_PART_TYPE(b_rkparm) = {
	.name		= "RKPARM",
	.part_type	= PART_TYPE_RKPARM,
	.max_entries	= RKPARM_ENTRY_NUMBERS,
	.get_info	= part_get_info_ptr(part_get_info_rkparm),
	.print		= part_print_ptr(part_print_rkparm),
	.test		= part_test_rkparm,
};
#endif
