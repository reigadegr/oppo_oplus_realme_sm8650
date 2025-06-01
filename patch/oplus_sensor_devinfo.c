// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2015, Sony Mobile Communications AB.
 * Copyright (c) 2012-2013, 2020, The Linux Foundation. All rights reserved.
 * Copyright (c) 2023-2024 Qualcomm Innovation Center, Inc. All rights reserved.
 */

#include <linux/hwspinlock.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_reserved_mem.h>
#include <linux/platform_device.h>
#include <linux/soc/qcom/qcom_hwspinlock.h>
#include <linux/sizes.h>
#include <linux/slab.h>
// #include <linux/soc/qcom/smem.h>

/*
 * The Qualcomm shared memory system is a allocate only heap structure that
 * consists of one of more memory areas that can be accessed by the processors
 * in the SoC.
 *
 * All systems contains a global heap, accessible by all processors in the SoC,
 * with a table of contents data structure (@smem_header) at the beginning of
 * the main shared memory block.
 *
 * The global header contains meta data for allocations as well as a fixed list
 * of 512 entries (@smem_global_entry) that can be initialized to reference
 * parts of the shared memory space.
 *
 *
 * In addition to this global heap a set of "private" heaps can be set up at
 * boot time with access restrictions so that only certain processor pairs can
 * access the data.
 *
 * These partitions are referenced from an optional partition table
 * (@smem_ptable), that is found 4kB from the end of the main smem region. The
 * partition table entries (@smem_ptable_entry) lists the involved processors
 * (or hosts) and their location in the main shared memory region.
 *
 * Each partition starts with a header (@smem_partition_header) that identifies
 * the partition and holds properties for the two internal memory regions. The
 * two regions are cached and non-cached memory respectively. Each region
 * contain a link list of allocation headers (@smem_private_entry) followed by
 * their data.
 *
 * Items in the non-cached region are allocated from the start of the partition
 * while items in the cached region are allocated from the end. The free area
 * is hence the region between the cached and non-cached offsets. The header of
 * cached items comes after the data.
 *
 * Version 12 (SMEM_GLOBAL_PART_VERSION) changes the item alloc/get procedure
 * for the global heap. A new global partition is created from the global heap
 * region with partition type (SMEM_GLOBAL_HOST) and the max smem item count is
 * set by the bootloader.
 *
 * To synchronize allocations in the shared memory heaps a remote spinlock must
 * be held - currently lock number 3 of the sfpb or tcsr is used for this on all
 * platforms.
 *
 */

/*
 * The version member of the smem header contains an array of versions for the
 * various software components in the SoC. We verify that the boot loader
 * version is a valid version as a sanity check.
 */
#define SMEM_MASTER_SBL_VERSION_INDEX	7
#define SMEM_GLOBAL_HEAP_VERSION	11
#define SMEM_GLOBAL_PART_VERSION	12

/*
 * The first 8 items are only to be allocated by the boot loader while
 * initializing the heap.
 */
#define SMEM_ITEM_LAST_FIXED	8

/* Highest accepted item number, for both global and private heaps */
#define SMEM_ITEM_COUNT		512

/* Processor/host identifier for the application processor */
#define SMEM_HOST_APPS		0

/* Processor/host identifier for the global partition */
#define SMEM_GLOBAL_HOST	0xfffe

/* Max number of processors/hosts in a system */
#define SMEM_HOST_COUNT		25

/* Entry range check
 * ptr >= start : Checks if ptr is greater than the start of access region
 * ptr + size >= ptr: Check for integer overflow (On 32bit system where ptr
 * and size are 32bits, ptr + size can wrap around to be a small integer)
 * ptr + size <= end: Checks if ptr+size is less than the end of access region
 */
#define IN_PARTITION_RANGE(ptr, size, start, end)		\
	(((void *)(ptr) >= (void *)(start)) &&			\
	 (((void *)(ptr) + (size)) >= (void *)(ptr)) &&	\
	 (((void *)(ptr) + (size)) <= (void *)(end)))

/**
  * struct smem_proc_comm - proc_comm communication struct (legacy)
  * @command:	current command to be executed
  * @status:	status of the currently requested command
  * @params:	parameters to the command
  */
struct smem_proc_comm {
	__le32 command;
	__le32 status;
	__le32 params[2];
};

/**
 * struct smem_global_entry - entry to reference smem items on the heap
 * @allocated:	boolean to indicate if this entry is used
 * @offset:	offset to the allocated space
 * @size:	size of the allocated space, 8 byte aligned
 * @aux_base:	base address for the memory region used by this unit, or 0 for
 *		the default region. bits 0,1 are reserved
 */
struct smem_global_entry {
	__le32 allocated;
	__le32 offset;
	__le32 size;
	__le32 aux_base; /* bits 1:0 reserved */
};
#define AUX_BASE_MASK		0xfffffffc

/**
 * struct smem_header - header found in beginning of primary smem region
 * @proc_comm:		proc_comm communication interface (legacy)
 * @version:		array of versions for the various subsystems
 * @initialized:	boolean to indicate that smem is initialized
 * @free_offset:	index of the first unallocated byte in smem
 * @available:		number of bytes available for allocation
 * @reserved:		reserved field, must be 0
 * @toc:		array of references to items
 */
struct smem_header {
	struct smem_proc_comm proc_comm[4];
	__le32 version[32];
	__le32 initialized;
	__le32 free_offset;
	__le32 available;
	__le32 reserved;
	struct smem_global_entry toc[SMEM_ITEM_COUNT];
};

/**
 * struct smem_ptable_entry - one entry in the @smem_ptable list
 * @offset:	offset, within the main shared memory region, of the partition
 * @size:	size of the partition
 * @flags:	flags for the partition (currently unused)
 * @host0:	first processor/host with access to this partition
 * @host1:	second processor/host with access to this partition
 * @cacheline:	alignment for "cached" entries
 * @reserved:	reserved entries for later use
 */
struct smem_ptable_entry {
	__le32 offset;
	__le32 size;
	__le32 flags;
	__le16 host0;
	__le16 host1;
	__le32 cacheline;
	__le32 reserved[7];
};

/**
 * struct smem_ptable - partition table for the private partitions
 * @magic:	magic number, must be SMEM_PTABLE_MAGIC
 * @version:	version of the partition table
 * @num_entries: number of partitions in the table
 * @reserved:	for now reserved entries
 * @entry:	list of @smem_ptable_entry for the @num_entries partitions
 */
struct smem_ptable {
	u8 magic[4];
	__le32 version;
	__le32 num_entries;
	__le32 reserved[5];
	struct smem_ptable_entry entry[];
};

static const u8 SMEM_PTABLE_MAGIC[] = { 0x24, 0x54, 0x4f, 0x43 }; /* "$TOC" */

/**
 * struct smem_partition_header - header of the partitions
 * @magic:	magic number, must be SMEM_PART_MAGIC
 * @host0:	first processor/host with access to this partition
 * @host1:	second processor/host with access to this partition
 * @size:	size of the partition
 * @offset_free_uncached: offset to the first free byte of uncached memory in
 *		this partition
 * @offset_free_cached: offset to the first free byte of cached memory in this
 *		partition
 * @reserved:	for now reserved entries
 */
struct smem_partition_header {
	u8 magic[4];
	__le16 host0;
	__le16 host1;
	__le32 size;
	__le32 offset_free_uncached;
	__le32 offset_free_cached;
	__le32 reserved[3];
};

/**
 * struct smem_partition - describes smem partition
 * @virt_base:	starting virtual address of partition
 * @phys_base:	starting physical address of partition
 * @cacheline:	alignment for "cached" entries
 * @size:	size of partition
 */
struct smem_partition {
	void __iomem *virt_base;
	phys_addr_t phys_base;
	size_t cacheline;
	size_t size;
};

static const u8 SMEM_PART_MAGIC[] = { 0x24, 0x50, 0x52, 0x54 };

/**
 * struct smem_private_entry - header of each item in the private partition
 * @canary:	magic number, must be SMEM_PRIVATE_CANARY
 * @item:	identifying number of the smem item
 * @size:	size of the data, including padding bytes
 * @padding_data: number of bytes of padding of data
 * @padding_hdr: number of bytes of padding between the header and the data
 * @reserved:	for now reserved entry
 */
struct smem_private_entry {
	u16 canary; /* bytes are the same so no swapping needed */
	__le16 item;
	__le32 size; /* includes padding bytes */
	__le16 padding_data;
	__le16 padding_hdr;
	__le32 reserved;
};
#define SMEM_PRIVATE_CANARY	0xa5a5

/**
 * struct smem_info - smem region info located after the table of contents
 * @magic:	magic number, must be SMEM_INFO_MAGIC
 * @size:	size of the smem region
 * @base_addr:	base address of the smem region
 * @reserved:	for now reserved entry
 * @num_items:	highest accepted item number
 */
struct smem_info {
	u8 magic[4];
	__le32 size;
	__le32 base_addr;
	__le32 reserved;
	__le16 num_items;
};

static const u8 SMEM_INFO_MAGIC[] = { 0x53, 0x49, 0x49, 0x49 }; /* SIII */

/**
 * struct smem_region - representation of a chunk of memory used for smem
 * @aux_base:	identifier of aux_mem base
 * @virt_base:	virtual base address of memory with this aux_mem identifier
 * @size:	size of the memory region
 */
struct smem_region {
	phys_addr_t aux_base;
	void __iomem *virt_base;
	size_t size;
};

/**
 * struct qcom_smem - device data for the smem device
 * @dev:	device pointer
 * @hwlock:	reference to a hwspinlock
 * @ptable: virtual base of partition table
 * @global_partition: describes for global partition when in use
 * @partitions: list of partitions of current processor/host
 * @item_count: max accepted item number
 * @socinfo:	platform device pointer
 * @num_regions: number of @regions
 * @regions:	list of the memory regions defining the shared memory
 */
struct qcom_smem {
	struct device *dev;

	struct hwspinlock *hwlock;

	u32 item_count;
	struct platform_device *socinfo;
	struct smem_ptable *ptable;
	struct smem_partition global_partition;
	struct smem_partition partitions[SMEM_HOST_COUNT];

	unsigned num_regions;
	struct smem_region regions[];
};

static void *
phdr_to_last_uncached_entry(struct smem_partition_header *phdr)
{
	void *p = phdr;

	return p + le32_to_cpu(phdr->offset_free_uncached);
}

static struct smem_private_entry *
phdr_to_first_cached_entry(struct smem_partition_header *phdr,
					size_t cacheline)
{
	void *p = phdr;
	struct smem_private_entry *e;

	return p + le32_to_cpu(phdr->size) - ALIGN(sizeof(*e), cacheline);
}

static void *
phdr_to_last_cached_entry(struct smem_partition_header *phdr)
{
	void *p = phdr;

	return p + le32_to_cpu(phdr->offset_free_cached);
}

static struct smem_private_entry *
phdr_to_first_uncached_entry(struct smem_partition_header *phdr)
{
	void *p = phdr;

	return p + sizeof(*phdr);
}

static struct smem_private_entry *
uncached_entry_next(struct smem_private_entry *e)
{
	void *p = e;

	return p + sizeof(*e) + le16_to_cpu(e->padding_hdr) +
	       le32_to_cpu(e->size);
}

static struct smem_private_entry *
cached_entry_next(struct smem_private_entry *e, size_t cacheline)
{
	void *p = e;

	return p - le32_to_cpu(e->size) - ALIGN(sizeof(*e), cacheline);
}

static void *uncached_entry_to_item(struct smem_private_entry *e)
{
	void *p = e;

	return p + sizeof(*e) + le16_to_cpu(e->padding_hdr);
}

static void *cached_entry_to_item(struct smem_private_entry *e)
{
	void *p = e;

	return p - le32_to_cpu(e->size);
}

/* Pointer to the one and only smem handle */
static struct qcom_smem *__smem;

/* Timeout (ms) for the trylock of remote spinlocks */
#define HWSPINLOCK_TIMEOUT	1000

/* The qcom hwspinlock id is always plus one from the smem host id */
#define SMEM_HOST_ID_TO_HWSPINLOCK_ID(__x) ((__x) + 1)

/**
 * qcom_smem_bust_hwspin_lock_by_host() - bust the smem hwspinlock for a host
 * @host:	remote processor id
 *
 * Busts the hwspin_lock for the given smem host id. This helper is intended
 * for remoteproc drivers that manage remoteprocs with an equivalent smem
 * driver instance in the remote firmware. Drivers can force a release of the
 * smem hwspin_lock if the rproc unexpectedly goes into a bad state.
 *
 * Context: Process context.
 *
 * Returns: 0 on success, otherwise negative errno.
 */
int qcom_smem_bust_hwspin_lock_by_host(unsigned int host)
{
	/* This function is for remote procs, so ignore SMEM_HOST_APPS */
	if (host == SMEM_HOST_APPS || host >= SMEM_HOST_COUNT)
		return -EINVAL;

	return qcom_hwspinlock_bust(__smem->hwlock, SMEM_HOST_ID_TO_HWSPINLOCK_ID(host));
}
EXPORT_SYMBOL_GPL(qcom_smem_bust_hwspin_lock_by_host);

static int qcom_smem_alloc_private(struct qcom_smem *smem,
				   struct smem_partition *part,
				   unsigned item,
				   size_t size)
{
	struct smem_private_entry *hdr, *end;
	struct smem_private_entry *next_hdr;
	struct smem_partition_header *phdr;
	size_t alloc_size;
	void *cached;
	void *p_end;

	phdr = (struct smem_partition_header __force *)part->virt_base;
	p_end = (void *)phdr + part->size;

	hdr = phdr_to_first_uncached_entry(phdr);
	end = phdr_to_last_uncached_entry(phdr);
	cached = phdr_to_last_cached_entry(phdr);

	if (WARN_ON(!IN_PARTITION_RANGE(end, 0, phdr, cached) ||
						cached > p_end))
		return -EINVAL;

	while ((hdr < end) && ((hdr + 1) < end)) {
		if (hdr->canary != SMEM_PRIVATE_CANARY)
			goto bad_canary;
		if (le16_to_cpu(hdr->item) == item)
			return -EEXIST;

		next_hdr = uncached_entry_next(hdr);

		if (WARN_ON(next_hdr <= hdr))
			return -EINVAL;

		hdr = next_hdr;
	}

	if (WARN_ON((void *)hdr > (void *)end))
		return -EINVAL;

	/* Check that we don't grow into the cached region */
	alloc_size = sizeof(*hdr) + ALIGN(size, 8);
	if ((void *)hdr + alloc_size > cached) {
		dev_err(smem->dev, "Out of memory\n");
		return -ENOSPC;
	}

	hdr->canary = SMEM_PRIVATE_CANARY;
	hdr->item = cpu_to_le16(item);
	hdr->size = cpu_to_le32(ALIGN(size, 8));
	hdr->padding_data = cpu_to_le16(le32_to_cpu(hdr->size) - size);
	hdr->padding_hdr = 0;

	/*
	 * Ensure the header is written before we advance the free offset, so
	 * that remote processors that does not take the remote spinlock still
	 * gets a consistent view of the linked list.
	 */
	wmb();
	le32_add_cpu(&phdr->offset_free_uncached, alloc_size);

	return 0;
bad_canary:
	dev_err(smem->dev, "Found invalid canary in hosts %hu:%hu partition\n",
		le16_to_cpu(phdr->host0), le16_to_cpu(phdr->host1));

	return -EINVAL;
}

static int qcom_smem_alloc_global(struct qcom_smem *smem,
				  unsigned item,
				  size_t size)
{
	struct smem_global_entry *entry;
	struct smem_header *header;

	header = smem->regions[0].virt_base;
	entry = &header->toc[item];
	if (entry->allocated)
		return -EEXIST;

	size = ALIGN(size, 8);
	if (WARN_ON(size > le32_to_cpu(header->available)))
		return -ENOMEM;

	entry->offset = header->free_offset;
	entry->size = cpu_to_le32(size);

	/*
	 * Ensure the header is consistent before we mark the item allocated,
	 * so that remote processors will get a consistent view of the item
	 * even though they do not take the spinlock on read.
	 */
	wmb();
	entry->allocated = cpu_to_le32(1);

	le32_add_cpu(&header->free_offset, size);
	le32_add_cpu(&header->available, -size);

	return 0;
}

/**
 * qcom_smem_alloc() - allocate space for a smem item
 * @host:	remote processor id, or -1
 * @item:	smem item handle
 * @size:	number of bytes to be allocated
 *
 * Allocate space for a given smem item of size @size, given that the item is
 * not yet allocated.
 */
int qcom_smem_alloc(unsigned host, unsigned item, size_t size)
{
	struct smem_partition *part;
	unsigned long flags;
	int ret;

	if (!__smem)
		return -EPROBE_DEFER;

	if (item < SMEM_ITEM_LAST_FIXED) {
		dev_err(__smem->dev,
			"Rejecting allocation of static entry %d\n", item);
		return -EINVAL;
	}

	if (WARN_ON(item >= __smem->item_count))
		return -EINVAL;

	ret = hwspin_lock_timeout_irqsave(__smem->hwlock,
					  HWSPINLOCK_TIMEOUT,
					  &flags);
	if (ret)
		return ret;

	if (host < SMEM_HOST_COUNT && __smem->partitions[host].virt_base) {
		part = &__smem->partitions[host];
		ret = qcom_smem_alloc_private(__smem, part, item, size);
	} else if (__smem->global_partition.virt_base) {
		part = &__smem->global_partition;
		ret = qcom_smem_alloc_private(__smem, part, item, size);
	} else {
		ret = qcom_smem_alloc_global(__smem, item, size);
	}

	hwspin_unlock_irqrestore(__smem->hwlock, &flags);

	return ret;
}
EXPORT_SYMBOL(qcom_smem_alloc);

static void *qcom_smem_get_global(struct qcom_smem *smem,
				  unsigned item,
				  size_t *size)
{
	struct smem_header *header;
	struct smem_region *region;
	struct smem_global_entry *entry;
	u64 entry_offset;
	u32 e_size;
	u32 aux_base;
	unsigned i;

	header = smem->regions[0].virt_base;
	entry = &header->toc[item];
	if (!entry->allocated)
		return ERR_PTR(-ENXIO);

	aux_base = le32_to_cpu(entry->aux_base) & AUX_BASE_MASK;

	for (i = 0; i < smem->num_regions; i++) {
		region = &smem->regions[i];

		if ((u32)region->aux_base == aux_base || !aux_base) {
			e_size = le32_to_cpu(entry->size);
			entry_offset = le32_to_cpu(entry->offset);

			if (WARN_ON(e_size + entry_offset > region->size))
				return ERR_PTR(-EINVAL);

			if (size != NULL)
				*size = e_size;

			return region->virt_base + entry_offset;
		}
	}

	return ERR_PTR(-ENOENT);
}

static void *qcom_smem_get_private(struct qcom_smem *smem,
				   struct smem_partition *part,
				   unsigned item,
				   size_t *size)
{
	struct smem_private_entry *e, *uncached_end, *cached_end;
	struct smem_private_entry *next_e;
	struct smem_partition_header *phdr;
	void *item_ptr, *p_end;
	size_t entry_size = 0;
	u32 padding_data;
	u32 e_size;

	phdr = (struct smem_partition_header __force *)part->virt_base;
	p_end = (void *)phdr + part->size;

	e = phdr_to_first_uncached_entry(phdr);
	uncached_end = phdr_to_last_uncached_entry(phdr);
	cached_end = phdr_to_last_cached_entry(phdr);

	if (WARN_ON(!IN_PARTITION_RANGE(uncached_end, 0, phdr, cached_end)
					|| (void *)cached_end > p_end))
		return ERR_PTR(-EINVAL);

	while ((e < uncached_end) && ((e + 1) < uncached_end)) {
		if (e->canary != SMEM_PRIVATE_CANARY)
			goto invalid_canary;

		if (le16_to_cpu(e->item) == item) {
			e_size = le32_to_cpu(e->size);
			padding_data = le16_to_cpu(e->padding_data);

			if (e_size < part->size && padding_data < e_size)
				entry_size = e_size - padding_data;
			else
				return ERR_PTR(-EINVAL);

			item_ptr =  uncached_entry_to_item(e);

			if (WARN_ON(!IN_PARTITION_RANGE(item_ptr, entry_size, e, uncached_end)))
				return ERR_PTR(-EINVAL);

			if (size != NULL)
				*size = entry_size;

			return item_ptr;
		}

		next_e = uncached_entry_next(e);
		if (WARN_ON(next_e <= e))
			return ERR_PTR(-EINVAL);

		e = next_e;
	}
	if (WARN_ON((void *)e > (void *)uncached_end))
		return ERR_PTR(-EINVAL);

	/* Item was not found in the uncached list, search the cached list */

	if (cached_end == p_end)
		return ERR_PTR(-ENOENT);


	e = phdr_to_first_cached_entry(phdr, part->cacheline);

	if (WARN_ON(!IN_PARTITION_RANGE(cached_end, 0, uncached_end, p_end) ||
			!IN_PARTITION_RANGE(e, sizeof(*e), cached_end, p_end)))
		return ERR_PTR(-EINVAL);

	while (e > cached_end) {
		if (e->canary != SMEM_PRIVATE_CANARY)
			goto invalid_canary;

		if (le16_to_cpu(e->item) == item) {
			e_size = le32_to_cpu(e->size);
			padding_data = le16_to_cpu(e->padding_data);

			if (e_size < part->size && padding_data < e_size)
				entry_size  = e_size - padding_data;
			else
				return ERR_PTR(-EINVAL);

			item_ptr =  cached_entry_to_item(e);
			if (WARN_ON(!IN_PARTITION_RANGE(item_ptr, entry_size, cached_end, e)))
				return ERR_PTR(-EINVAL);

			if (size != NULL)
				*size = entry_size;

			return item_ptr;
		}

		next_e = cached_entry_next(e, part->cacheline);
		if (WARN_ON(next_e >= e))
			return ERR_PTR(-EINVAL);

		e = next_e;
	}

	if (WARN_ON((void *)e < (void *)phdr))
		return ERR_PTR(-EINVAL);

	return ERR_PTR(-ENOENT);

invalid_canary:
	dev_err(smem->dev, "Found invalid canary in hosts %hu:%hu partition\n",
			le16_to_cpu(phdr->host0), le16_to_cpu(phdr->host1));

	return ERR_PTR(-EINVAL);
}

/**
 * qcom_smem_get() - resolve ptr of size of a smem item
 * @host:	the remote processor, or -1
 * @item:	smem item handle
 * @size:	pointer to be filled out with size of the item
 *
 * Looks up smem item and returns pointer to it. Size of smem
 * item is returned in @size.
 */
void *qcom_smem_get(unsigned int host, unsigned int item, size_t *size)
{
	struct smem_partition *part;
	unsigned long flags;
	int ret;
	void *ptr = ERR_PTR(-EPROBE_DEFER);

	if (!__smem)
		return ptr;

	if (WARN_ON(item >= __smem->item_count))
		return ERR_PTR(-EINVAL);

	ret = hwspin_lock_timeout_irqsave(__smem->hwlock,
					  HWSPINLOCK_TIMEOUT,
					  &flags);
	if (ret)
		return ERR_PTR(ret);

	if (host < SMEM_HOST_COUNT && __smem->partitions[host].virt_base) {
		part = &__smem->partitions[host];
		ptr = qcom_smem_get_private(__smem, part, item, size);
	} else if (__smem->global_partition.virt_base) {
		part = &__smem->global_partition;
		ptr = qcom_smem_get_private(__smem, part, item, size);
	} else {
		ptr = qcom_smem_get_global(__smem, item, size);
	}

	hwspin_unlock_irqrestore(__smem->hwlock, &flags);

	return ptr;

}
EXPORT_SYMBOL(qcom_smem_get);

/**
 * qcom_smem_get_free_space() - retrieve amount of free space in a partition
 * @host:	the remote processor identifying a partition, or -1
 *
 * To be used by smem clients as a quick way to determine if any new
 * allocations has been made.
 */
int qcom_smem_get_free_space(unsigned host)
{
	struct smem_partition *part;
	struct smem_partition_header *phdr;
	struct smem_header *header;
	unsigned ret;

	if (!__smem)
		return -EPROBE_DEFER;

	if (host < SMEM_HOST_COUNT && __smem->partitions[host].virt_base) {
		part = &__smem->partitions[host];
		phdr = part->virt_base;
		ret = le32_to_cpu(phdr->offset_free_cached) -
		      le32_to_cpu(phdr->offset_free_uncached);

		if (ret > le32_to_cpu(part->size))
			return -EINVAL;
	} else if (__smem->global_partition.virt_base) {
		part = &__smem->global_partition;
		phdr = part->virt_base;
		ret = le32_to_cpu(phdr->offset_free_cached) -
		      le32_to_cpu(phdr->offset_free_uncached);

		if (ret > le32_to_cpu(part->size))
			return -EINVAL;
	} else {
		header = __smem->regions[0].virt_base;
		ret = le32_to_cpu(header->available);

		if (ret > __smem->regions[0].size)
			return -EINVAL;
	}

	return ret;
}
EXPORT_SYMBOL(qcom_smem_get_free_space);

static bool addr_in_range(void __iomem *base, size_t size, void *addr)
{
	return base && ((void __iomem *)addr >= base && (void __iomem *)addr < base + size);
}

/**
 * qcom_smem_virt_to_phys() - return the physical address associated
 * with an smem item pointer (previously returned by qcom_smem_get()
 * @p:	the virtual address to convert
 *
 * Returns 0 if the pointer provided is not within any smem region.
 */
phys_addr_t qcom_smem_virt_to_phys(void *p)
{
	struct smem_partition *part;
	struct smem_region *area;
	u64 offset;
	u32 i;

	for (i = 0; i < SMEM_HOST_COUNT; i++) {
		part = &__smem->partitions[i];

		if (addr_in_range(part->virt_base, part->size, p)) {
			offset = p - part->virt_base;

			return (phys_addr_t)part->phys_base + offset;
		}
	}

	part = &__smem->global_partition;

	if (addr_in_range(part->virt_base, part->size, p)) {
		offset = p - part->virt_base;

		return (phys_addr_t)part->phys_base + offset;
	}

	for (i = 0; i < __smem->num_regions; i++) {
		area = &__smem->regions[i];

		if (addr_in_range(area->virt_base, area->size, p)) {
			offset = p - area->virt_base;

			return (phys_addr_t)area->aux_base + offset;
		}
	}

	return 0;
}
EXPORT_SYMBOL(qcom_smem_virt_to_phys);

static int qcom_smem_get_sbl_version(struct qcom_smem *smem)
{
	struct smem_header *header;
	__le32 *versions;

	header = smem->regions[0].virt_base;
	versions = header->version;

	return le32_to_cpu(versions[SMEM_MASTER_SBL_VERSION_INDEX]);
}

static struct smem_ptable *qcom_smem_get_ptable(struct qcom_smem *smem)
{
	struct smem_ptable *ptable;
	u32 version;

	ptable = smem->ptable;
	if (memcmp(ptable->magic, SMEM_PTABLE_MAGIC, sizeof(ptable->magic)))
		return ERR_PTR(-ENOENT);

	version = le32_to_cpu(ptable->version);
	if (version != 1) {
		dev_err(smem->dev,
			"Unsupported partition header version %d\n", version);
		return ERR_PTR(-EINVAL);
	}
	return ptable;
}

static u32 qcom_smem_get_item_count(struct qcom_smem *smem)
{
	struct smem_ptable *ptable;
	struct smem_info *info;

	ptable = qcom_smem_get_ptable(smem);
	if (IS_ERR_OR_NULL(ptable))
		return SMEM_ITEM_COUNT;

	info = (struct smem_info *)&ptable->entry[ptable->num_entries];
	if (memcmp(info->magic, SMEM_INFO_MAGIC, sizeof(info->magic)))
		return SMEM_ITEM_COUNT;

	return le16_to_cpu(info->num_items);
}

/*
 * Validate the partition header for a partition whose partition
 * table entry is supplied.  Returns a pointer to its header if
 * valid, or a null pointer otherwise.
 */
static struct smem_partition_header *
qcom_smem_partition_header(struct qcom_smem *smem,
		struct smem_ptable_entry *entry, u16 host0, u16 host1)
{
	struct smem_partition_header *header;
	u32 phys_addr;
	u32 size;

	phys_addr = smem->regions[0].aux_base + le32_to_cpu(entry->offset);
	header = devm_ioremap_wc(smem->dev, phys_addr, le32_to_cpu(entry->size));

	if (!header)
		return NULL;

	if (memcmp(header->magic, SMEM_PART_MAGIC, sizeof(header->magic))) {
		dev_err(smem->dev, "bad partition magic %4ph\n", header->magic);
		return NULL;
	}

	if (host0 != le16_to_cpu(header->host0)) {
		dev_err(smem->dev, "bad host0 (%hu != %hu)\n",
				host0, le16_to_cpu(header->host0));
		return NULL;
	}
	if (host1 != le16_to_cpu(header->host1)) {
		dev_err(smem->dev, "bad host1 (%hu != %hu)\n",
				host1, le16_to_cpu(header->host1));
		return NULL;
	}

	size = le32_to_cpu(header->size);
	if (size != le32_to_cpu(entry->size)) {
		dev_err(smem->dev, "bad partition size (%u != %u)\n",
			size, le32_to_cpu(entry->size));
		return NULL;
	}

	if (le32_to_cpu(header->offset_free_uncached) > size) {
		dev_err(smem->dev, "bad partition free uncached (%u > %u)\n",
			le32_to_cpu(header->offset_free_uncached), size);
		return NULL;
	}

	return header;
}

static int qcom_smem_set_global_partition(struct qcom_smem *smem)
{
	struct smem_partition_header *header;
	struct smem_ptable_entry *entry;
	struct smem_ptable *ptable;
	bool found = false;
	int i;

	if (smem->global_partition.virt_base) {
		dev_err(smem->dev, "Already found the global partition\n");
		return -EINVAL;
	}

	ptable = qcom_smem_get_ptable(smem);
	if (IS_ERR(ptable))
		return PTR_ERR(ptable);

	for (i = 0; i < le32_to_cpu(ptable->num_entries); i++) {
		entry = &ptable->entry[i];
		if (!le32_to_cpu(entry->offset))
			continue;
		if (!le32_to_cpu(entry->size))
			continue;

		if (le16_to_cpu(entry->host0) != SMEM_GLOBAL_HOST)
			continue;

		if (le16_to_cpu(entry->host1) == SMEM_GLOBAL_HOST) {
			found = true;
			break;
		}
	}

	if (!found) {
		dev_err(smem->dev, "Missing entry for global partition\n");
		return -EINVAL;
	}

	header = qcom_smem_partition_header(smem, entry,
				SMEM_GLOBAL_HOST, SMEM_GLOBAL_HOST);
	if (!header)
		return -EINVAL;

	smem->global_partition.virt_base = (void __iomem *)header;
	smem->global_partition.phys_base = smem->regions[0].aux_base +
								le32_to_cpu(entry->offset);
	smem->global_partition.size = le32_to_cpu(entry->size);
	smem->global_partition.cacheline = le32_to_cpu(entry->cacheline);

	return 0;
}

static int
qcom_smem_enumerate_partitions(struct qcom_smem *smem, u16 local_host)
{
	struct smem_partition_header *header;
	struct smem_ptable_entry *entry;
	struct smem_ptable *ptable;
	u16 remote_host;
	u16 host0, host1;
	int i;

	ptable = qcom_smem_get_ptable(smem);
	if (IS_ERR(ptable))
		return PTR_ERR(ptable);

	for (i = 0; i < le32_to_cpu(ptable->num_entries); i++) {
		entry = &ptable->entry[i];
		if (!le32_to_cpu(entry->offset))
			continue;
		if (!le32_to_cpu(entry->size))
			continue;

		host0 = le16_to_cpu(entry->host0);
		host1 = le16_to_cpu(entry->host1);
		if (host0 == local_host)
			remote_host = host1;
		else if (host1 == local_host)
			remote_host = host0;
		else
			continue;

		if (remote_host >= SMEM_HOST_COUNT) {
			dev_err(smem->dev, "bad host %u\n", remote_host);
			return -EINVAL;
		}

		if (smem->partitions[remote_host].virt_base) {
			dev_err(smem->dev, "duplicate host %u\n", remote_host);
			return -EINVAL;
		}

		header = qcom_smem_partition_header(smem, entry, host0, host1);
		if (!header)
			return -EINVAL;

		smem->partitions[remote_host].virt_base = (void __iomem *)header;
		smem->partitions[remote_host].phys_base = smem->regions[0].aux_base +
										le32_to_cpu(entry->offset);
		smem->partitions[remote_host].size = le32_to_cpu(entry->size);
		smem->partitions[remote_host].cacheline = le32_to_cpu(entry->cacheline);
	}

	return 0;
}

static int qcom_smem_map_toc(struct qcom_smem *smem, struct smem_region *region)
{
	u32 ptable_start;

	/* map starting 4K for smem header */
	region->virt_base = devm_ioremap_wc(smem->dev, region->aux_base, SZ_4K);
	ptable_start = region->aux_base + region->size - SZ_4K;
	/* map last 4k for toc */
	smem->ptable = devm_ioremap_wc(smem->dev, ptable_start, SZ_4K);

	if (!region->virt_base || !smem->ptable)
		return -ENOMEM;

	return 0;
}

static int qcom_smem_map_global(struct qcom_smem *smem, u32 size)
{
	u32 phys_addr;

	phys_addr = smem->regions[0].aux_base;

	smem->regions[0].size = size;
	smem->regions[0].virt_base = devm_ioremap_wc(smem->dev, phys_addr, size);

	if (!smem->regions[0].virt_base)
		return -ENOMEM;

	return 0;
}

static int qcom_smem_resolve_mem(struct qcom_smem *smem, const char *name,
				 struct smem_region *region)
{
	struct device *dev = smem->dev;
	struct device_node *np;
	struct resource r;
	int ret;

	np = of_parse_phandle(dev->of_node, name, 0);
	if (!np) {
		dev_err(dev, "No %s specified\n", name);
		return -EINVAL;
	}

	ret = of_address_to_resource(np, 0, &r);
	of_node_put(np);
	if (ret)
		return ret;

	region->aux_base = r.start;
	region->size = resource_size(&r);

	return 0;
}

static int qcom_smem_probe(struct platform_device *pdev)
{
	struct smem_header *header;
	struct reserved_mem *rmem;
	struct qcom_smem *smem;
	unsigned long flags;
	size_t array_size;
	int num_regions;
	int hwlock_id;
	u32 version;
	u32 size;
	int ret;
	int i;

	num_regions = 1;
	if (of_find_property(pdev->dev.of_node, "qcom,rpm-msg-ram", NULL))
		num_regions++;

	array_size = num_regions * sizeof(struct smem_region);
	smem = kzalloc(sizeof(*smem) + array_size, GFP_KERNEL);
	if (!smem)
		return -ENOMEM;

	smem->dev = &pdev->dev;
	smem->num_regions = num_regions;

	rmem = of_reserved_mem_lookup(pdev->dev.of_node);
	if (rmem) {
		smem->regions[0].aux_base = rmem->base;
		smem->regions[0].size = rmem->size;
	} else {
		/*
		 * Fall back to the memory-region reference, if we're not a
		 * reserved-memory node.
		 */
		ret = qcom_smem_resolve_mem(smem, "memory-region", &smem->regions[0]);
		if (ret)
			goto release;
	}

	if (num_regions > 1) {
		ret = qcom_smem_resolve_mem(smem, "qcom,rpm-msg-ram", &smem->regions[1]);
		if (ret)
			goto release;
	}


	ret = qcom_smem_map_toc(smem, &smem->regions[0]);
	if (ret)
		goto release;

	for (i = 1; i < num_regions; i++) {
		smem->regions[i].virt_base = devm_ioremap_wc(&pdev->dev,
							     smem->regions[i].aux_base,
							     smem->regions[i].size);
		if (!smem->regions[i].virt_base) {
			dev_err(&pdev->dev, "failed to remap %pa\n", &smem->regions[i].aux_base);
			ret = -ENOMEM;
			goto release;
		}
	}

	header = smem->regions[0].virt_base;
	if (le32_to_cpu(header->initialized) != 1 ||
	    le32_to_cpu(header->reserved)) {
		dev_err(&pdev->dev, "SMEM is not initialized by SBL\n");
		ret = -EINVAL;
		goto release;
	}

	hwlock_id = of_hwspin_lock_get_id(pdev->dev.of_node, 0);
	if (hwlock_id < 0) {
		if (hwlock_id != -EPROBE_DEFER)
			dev_err(&pdev->dev, "failed to retrieve hwlock\n");
			ret = hwlock_id;
			goto release;
	}

	smem->hwlock = hwspin_lock_request_specific(hwlock_id);
	if (!smem->hwlock) {
		ret = -ENXIO;
		goto release;
	}

	ret = hwspin_lock_timeout_irqsave(smem->hwlock, HWSPINLOCK_TIMEOUT, &flags);
	if (ret)
		goto release;
	size = readl_relaxed(&header->available) + readl_relaxed(&header->free_offset);
	hwspin_unlock_irqrestore(smem->hwlock, &flags);

	version = qcom_smem_get_sbl_version(smem);
	/*
	 * smem header mapping is required only in heap version scheme, so unmap
	 * it here. It will be remapped in qcom_smem_map_global() when whole
	 * partition is mapped again.
	 */
	devm_iounmap(smem->dev, smem->regions[0].virt_base);
	switch (version >> 16) {
	case SMEM_GLOBAL_PART_VERSION:
		ret = qcom_smem_set_global_partition(smem);
		if (ret < 0)
			goto release;
		smem->item_count = qcom_smem_get_item_count(smem);
		break;
	case SMEM_GLOBAL_HEAP_VERSION:
		qcom_smem_map_global(smem, size);
		smem->item_count = SMEM_ITEM_COUNT;
		break;
	default:
		dev_err(&pdev->dev, "Unsupported SMEM version 0x%x\n", version);
		ret = -EINVAL;
		goto release;
	}

	BUILD_BUG_ON(SMEM_HOST_APPS >= SMEM_HOST_COUNT);
	ret = qcom_smem_enumerate_partitions(smem, SMEM_HOST_APPS);
	if (ret < 0 && ret != -ENOENT)
		goto release;

	__smem = smem;

	smem->socinfo = platform_device_register_data(&pdev->dev, "qcom-socinfo",
						      PLATFORM_DEVID_NONE, NULL,
						      0);
	if (IS_ERR(smem->socinfo))
		dev_dbg(&pdev->dev, "failed to register socinfo device\n");

	return 0;

release:
	kfree(smem);
	return ret;
}

static int qcom_smem_remove(struct platform_device *pdev)
{
	platform_device_unregister(__smem->socinfo);

	hwspin_lock_free(__smem->hwlock);
	/*
	 * In case of Hibernation Restore __smem object is still valid
	 * and we call probe again so same object get allocated again
	 * that result into possible memory leak, hence explicitly freeing
	 * it here.
	 */
	kfree(__smem);
	__smem = NULL;

	return 0;
}

static int qcom_smem_freeze(struct device *dev)
{
	struct platform_device *pdev = container_of(dev, struct
					platform_device, dev);

	qcom_smem_remove(pdev);

	return 0;
}

static int qcom_smem_restore(struct device *dev)
{
	int ret = 0;
	struct platform_device *pdev = container_of(dev, struct
					platform_device, dev);

	/*
	 * SMEM related information has to fetched again
	 * during resuming from Hibernation, Hence call probe.
	 */
	ret = qcom_smem_probe(pdev);
	if (ret)
		dev_err(dev, "Error getting SMEM information\n");
	return ret;
}

static const struct dev_pm_ops qcom_smem_pm_ops = {
	.freeze_late = qcom_smem_freeze,
	.restore_early = qcom_smem_restore,
	.thaw_early = qcom_smem_restore,
};

static const struct of_device_id qcom_smem_of_match[] = {
	{ .compatible = "qcom,smem" },
	{}
};
MODULE_DEVICE_TABLE(of, qcom_smem_of_match);

static struct platform_driver qcom_smem_driver = {
	.probe = qcom_smem_probe,
	.remove = qcom_smem_remove,
	.driver  = {
		.name = "qcom-smem",
		.of_match_table = qcom_smem_of_match,
		.suppress_bind_attrs = true,
		.pm = &qcom_smem_pm_ops,
	},
};
//////-----------------------------------------//////
/* SPDX-License-Identifier: GPL-2.0-only  */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

/* SPDX-License-Identifier: GPL-2.0-only  */
/*
 * Copyright (C) 2018-2020 Oplus. All rights reserved.
 */

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/proc_fs.h>
#include <linux/fs.h>
#include <linux/io.h>
#include <linux/of.h>
#include <linux/platform_device.h>
// #include <linux/soc/qcom/smem.h>
#include <linux/sysfs.h>
#include <linux/syscalls.h>
#include <linux/unistd.h>

#include <linux/delay.h>
#include <linux/slab.h>
#include <soc/oplus/boot/oplus_project.h>
#define SENSOR_DEVINFO_SYNC_TIME  10000 //10s

//SMEM_SENSOR = SMEM_VERSION_FIRST + 23,
#define SMEM_SENSOR 130

#define REG_NUM 10
#define PARAMETER_NUM 31
#define FEATURE_NUM 13
#define SOURCE_NUM 2
#define PANEL_SOURCE_NUM 2
#define ALGO_PARAMETER_NUM 15
#define ALGO_FEATURE_NUM  5
#define DEFAULT_CONFIG 0xff

#define SENSOR_DEBUG
#ifdef SENSOR_DEBUG
#define SENSOR_DEVINFO_DEBUG(a, arg...) \
	do{\
		pr_err(a, ##arg);\
	}while(0)
#else
#define SENSOR_DEVINFO_DEBUG(a, arg...)
#endif
enum sensor_id {
    OPLUS_ACCEL,
    OPLUS_GYRO,
    OPLUS_MAG,
    OPLUS_LIGHT,
    OPLUS_PROXIMITY,
    OPLUS_SAR,
    OPLUS_CCT,
    OPLUS_CCT_REAR,
    OPLUS_BAROMETER,
    OPLUS_SARS,
    OPLUS_LIGHT_REAR,
    OPLUS_PAD_LIGHT,
    SENSORS_NUM
};

enum sensor_algo_id {
    OPLUS_PICKUP_DETECT,
    OPLUS_LUX_AOD,
    OPLUS_TP_GESTURE,
    OPLUS_FP_DISPLAY,
    OPLUS_FREE_FALL,
    OPLUS_CAMERA_PROTECT,
    OPLUS_MAG_FUSION,
    OPLUS_MEASUREMENT,
    SENSOR_ALGO_NUM
};

enum  {
    STK3A5X = 0x01,
    TCS3701 = 0x02,
    TCS3408 = 0x04,
    STK3A6X = 0x08,
    STK3329 = 0x10,
    TCS3720 = 0x40,
};

enum {
	PAD_LIGHT_TCS3720 = 0x01,
};

enum {
    LSM6DSM = 0x01,
    BMI160 = 0x02,
    LSM6DS3_C = 0x04,
    BMI260 = 0x08,
    LSM6DSO = 0x10,
    ICM4X6XX = 0x20,
    ICM4X607 = 0x40,
    BMI3X0 = 0x80,
    LIS2HH12 = 0x81,
    BMA420 = 0x82,
    SC7A20 = 0x84,
};

enum {
    AKM09918 = 0x01,
    MMC5603 = 0x02,
    MXG4300 = 0X04,
};

enum {
    LPS22HH = 0x01,
    BMP380 = 0x02,
};

enum {
    SX9324 = 0x01,
    SX9331 = 0x02
};

enum {
    CCT_TCS3408 = 0x01,
    CCT_STK37600 = 0x02
};

enum {
    TSL2540 = 0x01,
};

struct sensor_feature {
    int reg[REG_NUM];
    int parameter[PARAMETER_NUM];
    int feature[FEATURE_NUM];
};

struct sensor_hw {
    uint8_t sensor_name;
    uint8_t bus_number;
    uint8_t direction;
    uint8_t irq_number;
    struct sensor_feature feature;
};

struct sensor_vector {
    int sensor_id;
    struct sensor_hw hw[SOURCE_NUM];
};


struct sensor_algorithm {
    int sensor_id;
    int parameter[ALGO_PARAMETER_NUM];
    int feature[ALGO_FEATURE_NUM];
};

struct sensor_info {
    int magic_num;
    struct sensor_vector s_vector[SENSORS_NUM];
    struct sensor_algorithm a_vector[SENSOR_ALGO_NUM];
};

struct oplus_als_cali_data {
    int red_max_lux;
    int green_max_lux;
    int blue_max_lux;
    int white_max_lux;
    int cali_coe;
    int row_coe;
    struct proc_dir_entry   *proc_oplus_als;
};

struct sns_display_info {
	char *als_supt_cmdline[PANEL_SOURCE_NUM];
	char *pad_light_supt_cmdline[PANEL_SOURCE_NUM];
};

typedef enum {
    NORMAL = 0x01,
    UNDER_LCD = 0x02,
} alsps_position_type;

typedef enum {
    SOFTWARE_CAIL = 0x01,
    HARDWARE_CAIL = 0x02,
} ps_calibration_type;

#include <linux/version.h>
#include <linux/pinctrl/consumer.h>
#include <linux/gpio.h>
// #include <linux/soc/qcom/smem.h>

#define CLOSE_PD  1
#define CLOSE_PD_CONDITION 2
#define ALIGN4(s) ((sizeof(s) + 3)&(~0x3))
#define SAR_MAX_CH_NUM 5

#define MAX_CMDLINE_PARAM_LEN 512
char sns_dsi_display_primary[MAX_CMDLINE_PARAM_LEN];
char sns_dsi_display_secondary[MAX_CMDLINE_PARAM_LEN];

EXPORT_SYMBOL(sns_dsi_display_primary);
EXPORT_SYMBOL(sns_dsi_display_secondary);


extern int oplus_press_cali_data_init(void);
extern void oplus_press_cali_data_clean(void);

extern int pad_als_data_init(void);
extern void pad_als_data_clean(void);
extern int qcom_smem_alloc(unsigned host, unsigned item, size_t size);

struct sensor_info * g_chip = NULL;

struct proc_dir_entry *sensor_proc_dir = NULL;
static struct oplus_als_cali_data *gdata = NULL;
static uint32_t g_ldo_enable;
static bool g_fold_dev_supt = false;


static char* als_rear_feature[] = {
	"als-factor",
};


__attribute__((weak)) void oplus_device_dir_redirect(struct sensor_info * chip)
{
	pr_info("%s oplus_device_dir_redirect \n", __func__);
};

__attribute__((weak)) unsigned int get_serialID(void)
{
	return 0;
};
static void is_need_close_pd(struct sensor_hw* hw, struct device_node *ch_node)
{
	int rc = 0;
	int value = 0;
	int di = 0;
	int sn_size = 0;
	uint32_t *specific_sn = NULL;
	hw->feature.feature[2] = 0;
	rc = of_property_read_u32(ch_node, "is_need_close_pd", &value);

	if (!rc) {
		if (CLOSE_PD == value) {
			hw->feature.feature[2] = CLOSE_PD;
		} else if (CLOSE_PD_CONDITION == value) {
			sn_size = of_property_count_elems_of_size(ch_node, "sn_number", sizeof(uint32_t));
			pr_info("sn size %d\n", sn_size);
			specific_sn = (uint32_t *)kzalloc(sizeof(uint32_t) * sn_size, GFP_KERNEL);

			if (!specific_sn) {
				pr_err("%s kzalloc failed!\n", __func__);
				return;
			}

			of_property_read_u32_array(ch_node, "sn_number", specific_sn, sn_size);


			for (di = 0; di < sn_size; di++) {
				if (specific_sn[di] == get_serialID()) {
					hw->feature.feature[2] = CLOSE_PD;
					break;
				}
			}

			kfree(specific_sn);
		}
	}

}

static void parse_physical_sensor_common_dts(struct sensor_hw* hw, struct device_node *ch_node)
{
	int rc = 0;
	uint32_t chip_value = 0;
	rc = of_property_read_u32(ch_node, "sensor-name", &chip_value);

	if (rc) {
		hw->sensor_name = 0;
	} else {
		hw->sensor_name = chip_value;
	}

	rc = of_property_read_u32(ch_node, "bus-number", &chip_value);

	if (rc) {
		hw->bus_number = DEFAULT_CONFIG;/*read from registry */
	} else {
		hw->bus_number = chip_value;
	}

	rc = of_property_read_u32(ch_node, "sensor-direction", &chip_value);

	if (rc) {
		hw->direction = DEFAULT_CONFIG;/*read from registry */
	} else {
		hw->direction = chip_value;
	}

	rc = of_property_read_u32(ch_node, "irq-number", &chip_value);

	if (rc) {
		hw->irq_number = DEFAULT_CONFIG;/*read from registry */
	} else {
		hw->irq_number = chip_value;
	}

}

static void parse_magnetic_sensor_dts(struct sensor_hw* hw, struct device_node *ch_node)
{
	int value = 0;
	int rc = 0;
	int di = 0;
	int soft_default_para[18] = {10000, 0, 0, 0, 0, 0, 0, 0, 10000, 0, 0, 0, 0, 0, 0, 0, 10000, 0};
	/*set default defaut mag */
	memcpy((void *)&hw->feature.parameter[0], (void *)&soft_default_para[0], sizeof(soft_default_para));
	rc = of_property_read_u32(ch_node, "parameter-number", &value);
	if (!rc && value > 0 && value < PARAMETER_NUM) {
		rc = of_property_read_u32_array(ch_node,
				"soft-mag-parameter", &hw->feature.parameter[0], value);
		for (di = 0; di < value; di++) {
			SENSOR_DEVINFO_DEBUG("soft magnetic parameter[%d] : %d\n", di,
				hw->feature.parameter[di]);
			}
		return;
	} else if (rc) {
		int prj_id = 0;
		int prj_dir[8];
		struct device_node *node = ch_node;
		struct device_node *ch_node_mag = NULL;
		// prj_id = get_project();
		for_each_child_of_node(node, ch_node_mag) {
			if (ch_node_mag == NULL) {
				SENSOR_DEVINFO_DEBUG(" the mag_para use default parametyers");
				return;
				}
			rc = of_property_read_u32(ch_node_mag, "projects-num", &value);
			SENSOR_DEVINFO_DEBUG("get that project is %d", prj_id);
			rc = of_property_read_u32_array(ch_node_mag,
						"match-projects", &prj_dir[0], value);
			for (di = 0; di < value; di++) {
				SENSOR_DEVINFO_DEBUG(" which get there are %d projects", prj_dir[di]);
				if (prj_dir[di] == prj_id) {
				rc = of_property_read_u32(ch_node_mag, "parameter-number", &value);
					if (!rc && value > 0 && value < PARAMETER_NUM) {
					rc = of_property_read_u32_array(ch_node_mag,
						"soft-mag-parameter", &hw->feature.parameter[0], value);
					for (di = 0; di < value; di++) {
						SENSOR_DEVINFO_DEBUG("soft magnetic parameter[%d] : %d\n", di,
							hw->feature.parameter[di]);
						}
						return;
					} else {
						pr_info("parse soft magnetic parameter failed!\n");
					}
				}
				else
					continue;
			}
		}
	} else {
		pr_info("parse soft magnetic parameter failed!\n");
	}
}

static void parse_proximity_sensor_dts(struct sensor_hw* hw, struct device_node *ch_node)
{
	int value = 0;
	int rc = 0;
	int di = 0;
	char *param[] = {
		"low_step",
		"high_step",
		"low_limit",
		"high_limit",
		"dirty_low_step",
		"dirty_high_step",
		"ps_dirty_limit",
		"ps_ir_limit",
		"ps_adjust_min",
		"ps_adjust_max",
		"sampling_count",
		"step_max",
		"step_min",
		"step_div",
		"anti_shake_delta",
		"dynamic_cali_max",
		"raw2offset_radio",
		"offset_max",
		"offset_range_min",
		"offset_range_max",
		"force_cali_limit",
		"cali_jitter_limit",
		"cal_offset_margin",
                "is_ps_dri",
	};
	rc = of_property_read_u32(ch_node, "ps-type", &value);

	if (!rc) {
		hw->feature.feature[0] = value;
	}

	rc = of_property_read_u32(ch_node, "ps_saturation", &value);

	if (!rc) {
		hw->feature.feature[1] = value;
	}

	is_need_close_pd(hw, ch_node);
	rc = of_property_read_u32(ch_node, "ps_factory_cali_max", &value);

	if (!rc) {
		hw->feature.feature[3] = value;
	}
	rc = of_property_read_u32(ch_node, "alsps_off_to_idle_ms", &value);

	if (!rc) {
		hw->feature.feature[4] = value;
	}

	for (di = 0; di < ARRAY_SIZE(param); di++) {
		rc = of_property_read_u32(ch_node, param[di], &value);

		if (!rc) {
			hw->feature.parameter[di] = value;
		}

		SENSOR_DEVINFO_DEBUG("parameter[%d] : %d\n", di, hw->feature.parameter[di]);
	}

	rc = of_property_read_u32(ch_node, "parameter-number", &value);

	if (!rc && value > 0 && value < REG_NUM - 1) {
		hw->feature.reg[0] = value;
		rc = of_property_read_u32_array(ch_node,
				"sensor-reg", &hw->feature.reg[1], value);

		for (di = 0; di < value / 2; di++) {
			SENSOR_DEVINFO_DEBUG("sensor reg 0x%x = 0x%x\n", hw->feature.reg[di * 2 + 1],
				hw->feature.reg[2 * di + 2]);
		}
	} else {
		pr_info("parse alsps sensor reg failed\n");
	}

	SENSOR_DEVINFO_DEBUG("ps-type:%d ps_saturation:%d is_need_close_pd:%d alsps_off_to_idle_ms:%d\n",
		hw->feature.feature[0], hw->feature.feature[1], hw->feature.feature[2], hw->feature.feature[4]);
}

static void parse_light_sensor_dts(struct sensor_hw* hw, struct device_node *ch_node)
{
	int rc = 0;
	int value = 0;
	int di = 0;

	char *als_feature[] = {
		"als-type",
		"is-unit-device",
		"is-als-dri",
		"als-factor",
		"is_als_initialed",
		"als_buffer_length",
		"normalization_value",
		"use_lb_algo",
		"para-matrix",
		"als_ratio_type",
		"sup_remote_proc",
		"als_polling_timer",
		"is_distinguish_screens"
	};

	char *light_para[] = {
		"coef_a",
		"coef_b",
		"coef_c",
		"coef_d",
		"coef_e",
		"coef_ratio",
		"gold-reset-scale", /*gold scale value after sale*/
		/*lb para begin*/
		"lcd_type",
		"linear_comp_scope",
		"linear_comp_intercept",
		"temp_lux",
		"coef",
		"bal_coef",
		"ir_coef_stage_0",
		"ir_coef_stage_1",
		"ir_coef_stage_2",
		"ir_coef_stage_3",
		"ir_coef_val_0",
		"ir_coef_val_1",
		"ir_coef_val_2",
		"ir_coef_val_3",
		"ir_coef_val_4",
		/*lb para end*/
		"polling_use_majority",
		"k43",
		"k51",
		"k52",
		"k53",
		"k61",
		"k62",
		"k63",
		"lcd_name"
	};
	for (di = 0; di < ARRAY_SIZE(als_feature); di++) {
		rc = of_property_read_u32(ch_node, als_feature[di], &value);

		if (!rc) {
			hw->feature.feature[di] = value;
		} else if (0 == strncmp(als_feature[di], "norm", 4)) {
			hw->feature.feature[di] = 1057;
		} else if (0 == strncmp(als_feature[di], "als_ratio_type", strlen("als_ratio_type"))) {
			hw->feature.feature[di] = 0; /*set defaut zero*/
		} else {
			pr_info("parse %s failed!", als_feature[di]);
		}

		SENSOR_DEVINFO_DEBUG("light feature[%s] : %d\n", als_feature[di], hw->feature.feature[di]);
	}
	if (of_property_read_u32(ch_node, "is_distinguish_screens", &value)) {
		for (di = 0; di < ARRAY_SIZE(light_para); di++) {
			rc = of_property_read_u32(ch_node, light_para[di], &value);

			if (!rc) {
				hw->feature.parameter[di] = value;
			} else if (0 == strncmp(light_para[di], "gold-reset-scale", strlen("gold-reset-scale"))) {
				hw->feature.parameter[di] = 1001; /*set defaut value 1001*/
			} else {
				hw->feature.parameter[di] = 0; /*set defaut param*/
				pr_info("parse %s failed!", light_para[di]);
			}
			SENSOR_DEVINFO_DEBUG("light_para[%s] : %d\n", light_para[di], hw->feature.parameter[di]);
		}
	} else {
		struct device_node *node = ch_node;
		struct device_node *ch_node_lcd_num = NULL;
		char *lcd_name = NULL;
		for_each_child_of_node(node, ch_node_lcd_num) {
				if (ch_node_lcd_num == NULL) {
					SENSOR_DEVINFO_DEBUG(" not have ch_node_lcd_num");
					return;
				}
				rc = of_property_read_string(ch_node_lcd_num, "lcd_name", (const char **)&lcd_name);
				pr_err("enter child_node lcd_name = %s\n", lcd_name);
				if (strstr(sns_dsi_display_primary, lcd_name)) {
					for (di = 0; di < ARRAY_SIZE(light_para); di++) {
						rc = of_property_read_u32(ch_node_lcd_num, light_para[di], &value);

						if (!rc) {
							hw->feature.parameter[di] = value;
						} else if (0 == strncmp(light_para[di], "gold-reset-scale", strlen("gold-reset-scale"))) {
							hw->feature.parameter[di] = 1001; /*set defaut value 1001*/
						} else {
							hw->feature.parameter[di] = 0; /*set defaut param*/
							pr_info("parse %s failed!", light_para[di]);
						}
						SENSOR_DEVINFO_DEBUG("light_para[%s] : %d\n", light_para[di], hw->feature.parameter[di]);
					}
				}
		}
	}
}
static void parse_light_rear_sensor_dts(struct sensor_hw* hw, struct device_node *ch_node)
{
	int rc = 0;
	int value = 0;
	int di = 0;

	for (di = 0; di < ARRAY_SIZE(als_rear_feature); di++) {
		rc = of_property_read_u32(ch_node, als_rear_feature[di], &value);

		if (!rc) {
			hw->feature.feature[di] = value;
		} else {
			pr_info("parse %s failed!", als_rear_feature[di]);
		}

		SENSOR_DEVINFO_DEBUG("parse_light_rear_sensor_dts-feature[%d] : %d\n", di, hw->feature.feature[di]);
	}
}

static void parse_sar_sensor_dts(struct sensor_hw* hw, struct device_node *ch_node)
{
	int di = 0;
	int rc = 0;
	int value = 0;
	int dc_offset_default[SAR_MAX_CH_NUM * 2] = {0, 0, 0, 0, 0, 30000, 30000, 30000, 30000, 30000};
	rc = of_property_read_u32(ch_node, "parameter-number", &value);
	if (!rc && value > 0 && value < PARAMETER_NUM) {
		rc = of_property_read_u32_array(ch_node,
			"sensor-reg", &hw->feature.parameter[0], value);
		for (di = 0; di < value / 2; di++) {
			SENSOR_DEVINFO_DEBUG("sensor reg 0x%x = 0x%x\n", hw->feature.parameter[di * 2],
				hw->feature.parameter[2 * di + 1]);
		}
	} else {
			pr_info("parse sar sensor reg failed\n");
	}
	/*channel-num*/
	rc = of_property_read_u32(ch_node, "channel-num", &value);
	if (!rc && value < SAR_MAX_CH_NUM) {
		hw->feature.feature[di] = value;
		SENSOR_DEVINFO_DEBUG("sar channel-num: %d\n", value);
	} else {
		pr_info("parse sar sensor channel-num failed, rc %d, value %d", rc, value);
	}
	/*reg->dc_offset*/
	rc = of_property_read_u32(ch_node, "is-dc-offset", &value);
	if (!rc && value == 1) {
		memcpy((void *)&hw->feature.reg[0], (void *)&dc_offset_default[0], SAR_MAX_CH_NUM * 2);
		for (di = 0; di < SAR_MAX_CH_NUM; di++) {
			SENSOR_DEVINFO_DEBUG("sar dc_offset_l[%d] = %d, dc_offset_H[%d] = %d",
				di, hw->feature.reg[di], di + SAR_MAX_CH_NUM, hw->feature.reg[di + SAR_MAX_CH_NUM]);
		}
		rc = of_property_read_u32_array(ch_node, "dc-offset", &hw->feature.reg[0], SAR_MAX_CH_NUM * 2);
		for (di = 0; di < SAR_MAX_CH_NUM; di++) {
			SENSOR_DEVINFO_DEBUG("sar dc_offset_l[%d] = %d, dc_offset_H[%d] = %d",
				di, hw->feature.reg[di], di + SAR_MAX_CH_NUM, hw->feature.reg[di + SAR_MAX_CH_NUM]);
		}
	} else {
			pr_info("parse sar sensor dc_offset failed, rc %d, value %d", rc, value);
	}

}

static void parse_down_sar_sensor_dts(struct sensor_hw* hw, struct device_node *ch_node)
{
	int di = 0;
	int rc = 0;
	int value = 0;
	rc = of_property_read_u32(ch_node, "parameter-number", &value);

	if (!rc && value > 0 && value < PARAMETER_NUM) {
		rc = of_property_read_u32_array(ch_node,
				"sensor-reg", &hw->feature.parameter[0], value);

		for (di = 0; di < value / 2; di++) {
			SENSOR_DEVINFO_DEBUG("sensor reg 0x%x = 0x%x\n", hw->feature.parameter[di * 2],
				hw->feature.parameter[2 * di + 1]);
		}

	} else {
		pr_info("parse down-sar sensor reg failed\n");
	}

}

static int parse_cct_sensor_dts(struct sensor_hw *hw, struct device_node *ch_node)
{
	int value = 0;
	int rc = 0;
	int di = 0;
	int als_supt_cmdline_di = 0;
	struct sns_display_info *dsi_info = NULL;
	char *feature[] = {
		"decoupled-driver",
		"publish-sensors",
		"is-ch-dri",
		"timer-size",
		"fac-cali-sensor",
		"first-source",
		"second-source"
	};

	char *para[] = {
		"para-matrix",
		"atime",
		"first-atime",
		"fac-cali-atime",
		"first-again",
		"fac-cali-again",
		"fd-time",
		"fac-cali-fd-time",
		"first-fd-gain",
		"fac-cali-fd-gain"
	};

	hw->feature.feature[0] = 1;/*default use decoupled driver oplus_cct */
	hw->feature.feature[5] = 0;
	dsi_info = kzalloc(sizeof(struct sns_display_info), GFP_KERNEL);
	if (dsi_info == NULL) {
		rc = -ENOMEM;
		printk("[SNS] %s:kzalloc fail %d\n", __func__, rc);
		return rc;
	}

	for (di = 0; di < ARRAY_SIZE(feature); di++) {
		if (strstr(feature[di], "first-source") || strstr(feature[di], "second-source")) {
			rc = of_property_read_string(ch_node, feature[di], (const char **)&dsi_info->als_supt_cmdline[als_supt_cmdline_di]);
			if(!rc && strstr(sns_dsi_display_primary, dsi_info->als_supt_cmdline[als_supt_cmdline_di])) {
				hw->feature.feature[5] = als_supt_cmdline_di;
				SENSOR_DEVINFO_DEBUG("[SNS] %d panel source: %s\n", di, dsi_info->als_supt_cmdline[als_supt_cmdline_di]);
			}
			als_supt_cmdline_di++;
			SENSOR_DEVINFO_DEBUG("[SNS] cct panel_idx: %d\n", hw->feature.feature[5]);
		} else {
			rc = of_property_read_u32(ch_node, feature[di], &value);
			if (!rc) {
				hw->feature.feature[di] = value;
			}
			SENSOR_DEVINFO_DEBUG("cct_feature[%d] : %d\n", di, hw->feature.feature[di]);
		}
	}
	for (di = 0; di < ARRAY_SIZE(para); di++) {
		rc = of_property_read_u32(ch_node, para[di], &value);

		if (!rc) {
			hw->feature.parameter[di] = value;
		}

		SENSOR_DEVINFO_DEBUG("cct_parameter[%d] : %d\n", di, hw->feature.parameter[di]);
	}
	kfree(dsi_info);
	dsi_info = NULL;
	return rc;
}

static void parse_cct_rear_sensor_dts(struct sensor_hw *hw, struct device_node *ch_node)
{
	int value = 0;
	int rc = 0;
	int di = 0;
	char *feature[] = {
		"decoupled-driver",
		"publish-sensors",
		"is-ch-dri",
		"timer-size",
		"fac-cali-sensor"
	};

	char *para[] = {
		"para-matrix",
		"atime",
		"first-atime",
		"fac-cali-atime",
		"first-again",
		"fac-cali-again",
		"fd-time",
		"fac-cali-fd-time",
		"first-fd-gain",
		"fac-cali-fd-gain"
	};

	hw->feature.feature[0] = 1;/*default use decoupled driver oplus_cct */

	for (di = 0; di < ARRAY_SIZE(feature); di++) {
		rc = of_property_read_u32(ch_node, feature[di], &value);

		if (!rc) {
			hw->feature.feature[di] = value;
		}

		SENSOR_DEVINFO_DEBUG("cct_feature[%d] : %d\n", di, hw->feature.feature[di]);
	}

	for (di = 0; di < ARRAY_SIZE(para); di++) {
		rc = of_property_read_u32(ch_node, para[di], &value);

		if (!rc) {
			hw->feature.parameter[di] = value;
		}

		SENSOR_DEVINFO_DEBUG("cct_parameter[%d] : %d\n", di, hw->feature.parameter[di]);
	}
}

static void parse_accelerometer_sensor_dts(struct sensor_hw *hw, struct device_node *ch_node)
{
	int value = 0;
	int rc = 0;
	int di = 0;
	char *feature[] = {
		"use-sois"
	};

	hw->feature.feature[0] = 0;/*default not use s-ois */

	for (di = 0; di < ARRAY_SIZE(feature); di++) {
		rc = of_property_read_u32(ch_node, feature[di], &value);

		if (!rc) {
			hw->feature.feature[di] = value;
		}

		SENSOR_DEVINFO_DEBUG("gsensor_feature[%d] : %d\n", di, hw->feature.feature[di]);
	}
}

static int parse_pad_light_sensor_dts(struct sensor_hw *hw, struct device_node *ch_node)
{
	int rc = 0;
	int di = 0;
	char *feature[] = {
		"first-source",
		"second-source"
	};
	struct sns_display_info *dsi_info = NULL;

	dsi_info = kzalloc(sizeof(struct sns_display_info), GFP_KERNEL);
	if (dsi_info == NULL) {
		rc = -ENOMEM;
		printk("[SNS] %s:kzalloc fail %d\n", __func__, rc);
		return rc;
	}

	for (di = 0; di < ARRAY_SIZE(feature); di++) {
		rc = of_property_read_string(ch_node, feature[di], (const char **)&dsi_info->pad_light_supt_cmdline[di]);

		if(!rc && strstr(sns_dsi_display_primary, dsi_info->pad_light_supt_cmdline[di])) {
			hw->feature.feature[0] = di;
			pr_info("[SNS] %d panel source: %s\n", di, dsi_info->pad_light_supt_cmdline[di]);
		}
	}
	pr_info("[SNS] pad_light panel_idx: %d\n", hw->feature.feature[0]);

	kfree(dsi_info);
	dsi_info = NULL;

	return rc;
}

static void parse_each_physical_sensor_dts(struct sensor_hw* hw, struct device_node *ch_node)
{
	if (0 == strncmp(ch_node->name, "msensor", 7)) {
		parse_magnetic_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "psensor", 7)) {
		parse_proximity_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "lsensor", 7)) {
		parse_light_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "ssensor", 7)) {
		parse_sar_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "cctsensor", 7)) {
		parse_cct_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "cctrsensor", 7)) {
		parse_cct_rear_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "sdsensor", 7)) {
		parse_down_sar_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "lrsensor", 7)) {
		parse_light_rear_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "gsensor", 7)) {
		parse_accelerometer_sensor_dts(hw, ch_node);
	} else if (0 == strncmp(ch_node->name, "pad_light", 9)) {
		parse_pad_light_sensor_dts(hw, ch_node);
	} else {
		/*do nothing */
	}

}

static void parse_pickup_sensor_dts(struct sensor_algorithm *algo, struct device_node *ch_node)
{
	int rc = 0;
	int value = 0;
	rc = of_property_read_u32(ch_node, "is-need-prox", &value);

	if (!rc) {
		algo->feature[0] = value;
	}

	rc = of_property_read_u32(ch_node, "prox-type", &value);

	if (!rc) {
		algo->parameter[0] = value;
	}

	SENSOR_DEVINFO_DEBUG("is-need-prox: %d, prox-type: %d\n",
		algo->feature[0], algo->parameter[0]);
}

static void parse_lux_aod_sensor_dts(struct sensor_algorithm *algo, struct device_node *ch_node)
{
	int rc = 0;
	int value = 0;
	rc = of_property_read_u32(ch_node, "thrd-low", &value);

	if (!rc) {
		algo->parameter[0] = value;
	}

	rc = of_property_read_u32(ch_node, "thrd-high", &value);

	if (!rc) {
		algo->parameter[1] = value;
	}

	rc = of_property_read_u32(ch_node, "als-type", &value);

	if (!rc) {
		algo->parameter[2] = value;
	}

	SENSOR_DEVINFO_DEBUG("thrd-low: %d, thrd-high: %d, als-type: %d\n",
		algo->parameter[0], algo->parameter[1], algo->parameter[2]);

}

static void parse_fp_display_sensor_dts(struct sensor_algorithm *algo, struct device_node *ch_node)
{
	int rc = 0;
	int value = 0;
	rc = of_property_read_u32(ch_node, "prox-type", &value);

	if (!rc) {
		algo->parameter[0] = value;
	}

	SENSOR_DEVINFO_DEBUG("prox-type :%d\n", algo->parameter[0]);
}

static void parse_mag_fusion_sensor_dts(struct sensor_algorithm *algo, struct device_node *ch_node)
{
	int rc = 0;
	int value = 0;
	int di = 0;

	char *para[] = {
		"track_trigger",
		"absolute_trigger",
	};

	rc = of_property_read_u32(ch_node, "fusion-type", &value);
	if (!rc) {
		algo->feature[0] = value;
	}

	rc = of_property_read_u32(ch_node, "fold-feature", &value);
	if (!rc) {
		algo->feature[1] = value;
	}

	rc = of_property_read_u32(ch_node, "drop_zero", &value);
	if (!rc) {
		algo->feature[2] = value;
	}

	for (di = 0; di < ARRAY_SIZE(para); di++) {
		rc = of_property_read_u32(ch_node, para[di], &value);
		if (!rc) {
			algo->parameter[di] = value;
		}
	}

	SENSOR_DEVINFO_DEBUG("fusion-type:%d, fold-feature:%d, drop_zero:%d\n", algo->feature[0], algo->feature[1], algo->feature[2]);
	SENSOR_DEVINFO_DEBUG("track_trigger:%d, absolute_trigger:%d\n", algo->parameter[0], algo->parameter[1]);
}

static void parse_oplus_measurement_sensor_dts(struct sensor_algorithm *algo, struct device_node *ch_node)
{
	int rc = 0;
	int value = 0;

	rc = of_property_read_u32(ch_node, "acc-range", &value);

	if (!rc) {
		algo->parameter[0] = value;
	}

	rc = of_property_read_u32(ch_node, "gyro-range", &value);

	if (!rc) {
		algo->parameter[1] = value;
	}

	SENSOR_DEVINFO_DEBUG("acc-range: %d, gyro-range: %d\n",
		algo->parameter[0], algo->parameter[1]);
}

static void parse_each_virtual_sensor_dts(struct sensor_algorithm *algo, struct device_node * ch_node)
{
	if (0 == strncmp(ch_node->name, "pickup", 6)) {
		parse_pickup_sensor_dts(algo, ch_node);
	} else if (0 == strncmp(ch_node->name, "lux_aod", 6)) {
		parse_lux_aod_sensor_dts(algo, ch_node);
	} else if (0 == strncmp(ch_node->name, "fp_display", 6)) {
		parse_fp_display_sensor_dts(algo, ch_node);
	} else if (0 == strncmp(ch_node->name, "mag_fusion", 10)) {
		parse_mag_fusion_sensor_dts(algo, ch_node);
	} else if (0 == strncmp(ch_node->name, "oplus_measurement", 17)) {
		parse_oplus_measurement_sensor_dts(algo, ch_node);
	} else {
		/* do nothing */
	}

}

static void oplus_sensor_parse_dts(struct platform_device *pdev)
{
	struct device_node *node = pdev->dev.of_node;
	struct sensor_info * chip = platform_get_drvdata(pdev);
	int rc = 0;
	int value = 0;
	bool is_virtual_sensor = false;
	struct device_node *ch_node = NULL;
	int sensor_type = 0;
	int sensor_index = 0;
	struct sensor_hw *hw = NULL;
	struct sensor_algorithm *algo = NULL;
	pr_info("start \n");
	pr_info("[SNS] sns_dsi_display_primary = %s \n", sns_dsi_display_primary);
	pr_info("[SNS] sns_dsi_display_secondary = %s \n", sns_dsi_display_secondary);

	if (of_property_read_bool(node, "is-folding-device")) {
		g_fold_dev_supt = true;
	} else {
		g_fold_dev_supt = false;
	}

	for_each_child_of_node(node, ch_node) {
		is_virtual_sensor = false;

		if (of_property_read_bool(ch_node, "is-virtual-sensor")) {
			is_virtual_sensor = true;
		}

		rc = of_property_read_u32(ch_node, "sensor-type", &value);

		if (rc || (is_virtual_sensor && value >= SENSOR_ALGO_NUM)
			|| value >= SENSORS_NUM) {
			pr_info("parse sensor type failed!\n");
			continue;
		} else {
			sensor_type = value;
		}

		if (!is_virtual_sensor) {
			chip->s_vector[sensor_type].sensor_id = sensor_type;
			rc = of_property_read_u32(ch_node, "sensor-index", &value);

			if (rc || value >= SOURCE_NUM) {
				pr_info("parse sensor index failed!\n");
				continue;
			} else {
				sensor_index = value;
			}

			hw = &chip->s_vector[sensor_type].hw[sensor_index];
			parse_physical_sensor_common_dts(hw, ch_node);
			SENSOR_DEVINFO_DEBUG("chip->s_vector[%d].hw[%d] : sensor-name %d, \
					bus-number %d, sensor-direction %d, \
					irq-number %d\n",
				sensor_type, sensor_index,
				chip->s_vector[sensor_type].hw[sensor_index].sensor_name,
				chip->s_vector[sensor_type].hw[sensor_index].bus_number,
				chip->s_vector[sensor_type].hw[sensor_index].direction,
				chip->s_vector[sensor_type].hw[sensor_index].irq_number);
			parse_each_physical_sensor_dts(hw, ch_node);
		} else {
			chip->a_vector[sensor_type].sensor_id = sensor_type;
			SENSOR_DEVINFO_DEBUG("chip->a_vector[%d].sensor_id %d: sensor_type %d",
				sensor_type, chip->a_vector[sensor_type].sensor_id, sensor_type);
			algo = &chip->a_vector[sensor_type];
			parse_each_virtual_sensor_dts(algo, ch_node);
		}
	}/*for_each_child_of_node */

	rc = of_property_read_u32(node, "als-row-coe", &value);

	if (rc) {
		gdata->row_coe = 1000;
	} else {
		gdata->row_coe = value;
	}
	rc = of_property_read_u32(node, "ldo_enable", &g_ldo_enable);

	oplus_device_dir_redirect(chip);
}

static ssize_t als_type_read_proc(struct file *file, char __user *buf,
	size_t count, loff_t *off)
{
	char page[256] = {0};
	int len = 0;

	if (!g_chip) {
		return -ENOMEM;
	}

	len = sprintf(page, "%d", g_chip->s_vector[OPLUS_LIGHT].hw[0].feature.feature[0]);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}

	if (copy_to_user(buf, page, (len < count ? len : count))) {
		return -EFAULT;
	}

	*off += len < count ? len : count;
	return (len < count ? len : count);
}

static ssize_t red_max_lux_read_proc(struct file *file, char __user *buf,
	size_t count, loff_t *off)
{
	char page[256] = {0};
	int len = 0;

	if (!gdata) {
		return -ENOMEM;
	}

	len = sprintf(page, "%d", gdata->red_max_lux);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}

	if (copy_to_user(buf, page, (len < count ? len : count))) {
		return -EFAULT;
	}

	*off += len < count ? len : count;
	return (len < count ? len : count);
}
static ssize_t red_max_lux_write_proc(struct file *file, const char __user *buf,
	size_t count, loff_t *off)

{
	char page[256] = {0};
	unsigned int input = 0;

	if (!gdata) {
		return -ENOMEM;
	}


	if (count > 256) {
		count = 256;
	}

	if (count > *off) {
		count -= *off;
	} else {
		count = 0;
	}

	if (copy_from_user(page, buf, count)) {
		return -EFAULT;
	}

	*off += count;

	if (sscanf(page, "%u", &input) != 1) {
		count = -EINVAL;
		return count;
	}

	if (input != gdata->red_max_lux) {
		gdata->red_max_lux = input;
	}

	return count;
}

static ssize_t white_max_lux_read_proc(struct file *file, char __user *buf,
	size_t count, loff_t *off)
{
	char page[256] = {0};
	int len = 0;

	if (!gdata) {
		return -ENOMEM;
	}

	len = sprintf(page, "%d", gdata->white_max_lux);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}

	if (copy_to_user(buf, page, (len < count ? len : count))) {
		return -EFAULT;
	}

	*off += len < count ? len : count;
	return (len < count ? len : count);
}
static ssize_t white_max_lux_write_proc(struct file *file, const char __user *buf,
	size_t count, loff_t *off)

{
	char page[256] = {0};
	unsigned int input = 0;

	if (!gdata) {
		return -ENOMEM;
	}


	if (count > 256) {
		count = 256;
	}

	if (count > *off) {
		count -= *off;
	} else {
		count = 0;
	}

	if (copy_from_user(page, buf, count)) {
		return -EFAULT;
	}

	*off += count;

	if (sscanf(page, "%u", &input) != 1) {
		count = -EINVAL;
		return count;
	}

	if (input != gdata->white_max_lux) {
		gdata->white_max_lux = input;
	}

	return count;
}

static ssize_t blue_max_lux_read_proc(struct file *file, char __user *buf,
	size_t count, loff_t *off)
{
	char page[256] = {0};
	int len = 0;

	if (!gdata) {
		return -ENOMEM;
	}

	len = sprintf(page, "%d", gdata->blue_max_lux);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}

	if (copy_to_user(buf, page, (len < count ? len : count))) {
		return -EFAULT;
	}

	*off += len < count ? len : count;
	return (len < count ? len : count);
}
static ssize_t blue_max_lux_write_proc(struct file *file, const char __user *buf,
	size_t count, loff_t *off)

{
	char page[256] = {0};
	unsigned int input = 0;

	if (!gdata) {
		return -ENOMEM;
	}


	if (count > 256) {
		count = 256;
	}

	if (count > *off) {
		count -= *off;
	} else {
		count = 0;
	}

	if (copy_from_user(page, buf, count)) {
		return -EFAULT;
	}

	*off += count;

	if (sscanf(page, "%u", &input) != 1) {
		count = -EINVAL;
		return count;
	}

	if (input != gdata->blue_max_lux) {
		gdata->blue_max_lux = input;
	}

	return count;
}

static ssize_t green_max_lux_read_proc(struct file *file, char __user *buf,
	size_t count, loff_t *off)
{
	char page[256] = {0};
	int len = 0;

	if (!gdata) {
		return -ENOMEM;
	}

	len = sprintf(page, "%d", gdata->green_max_lux);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}

	if (copy_to_user(buf, page, (len < count ? len : count))) {
		return -EFAULT;
	}

	*off += len < count ? len : count;
	return (len < count ? len : count);
}
static ssize_t green_max_lux_write_proc(struct file *file, const char __user *buf,
	size_t count, loff_t *off)

{
	char page[256] = {0};
	unsigned int input = 0;

	if (!gdata) {
		return -ENOMEM;
	}


	if (count > 256) {
		count = 256;
	}

	if (count > *off) {
		count -= *off;
	} else {
		count = 0;
	}

	if (copy_from_user(page, buf, count)) {
		return -EFAULT;
	}

	*off += count;

	if (sscanf(page, "%u", &input) != 1) {
		count = -EINVAL;
		return count;
	}

	if (input != gdata->green_max_lux) {
		gdata->green_max_lux = input;
	}

	return count;
}

static ssize_t cali_coe_read_proc(struct file *file, char __user *buf,
	size_t count, loff_t *off)
{
	char page[256] = {0};
	int len = 0;

	if (!gdata) {
		return -ENOMEM;
	}

	len = sprintf(page, "%d", gdata->cali_coe);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}

	if (copy_to_user(buf, page, (len < count ? len : count))) {
		return -EFAULT;
	}

	*off += len < count ? len : count;
	return (len < count ? len : count);
}

static ssize_t cali_coe_write_proc(struct file *file, const char __user *buf,
	size_t count, loff_t *off)

{
	char page[256] = {0};
	unsigned int input = 0;

	if (!gdata) {
		return -ENOMEM;
	}


	if (count > 256) {
		count = 256;
	}

	if (count > *off) {
		count -= *off;
	} else {
		count = 0;
	}

	if (copy_from_user(page, buf, count)) {
		return -EFAULT;
	}

	*off += count;

	if (sscanf(page, "%u", &input) != 1) {
		count = -EINVAL;
		return count;
	}

	if (input != gdata->cali_coe) {
		gdata->cali_coe = input;
	}

	return count;
}

static ssize_t row_coe_read_proc(struct file *file, char __user *buf,
	size_t count, loff_t *off)
{
	char page[256] = {0};
	int len = 0;

	if (!gdata) {
		return -ENOMEM;
	}

	len = sprintf(page, "%d", gdata->row_coe);

	if (len > *off) {
		len -= *off;
	} else {
		len = 0;
	}

	if (copy_to_user(buf, page, (len < count ? len : count))) {
		return -EFAULT;
	}

	*off += len < count ? len : count;
	return (len < count ? len : count);
}

static ssize_t row_coe_write_proc(struct file *file, const char __user *buf,
	size_t count, loff_t *off)

{
	char page[256] = {0};
	unsigned int input = 0;

	if (!gdata) {
		return -ENOMEM;
	}


	if (count > 256) {
		count = 256;
	}

	if (count > *off) {
		count -= *off;
	} else {
		count = 0;
	}

	if (copy_from_user(page, buf, count)) {
		return -EFAULT;
	}

	*off += count;

	if (sscanf(page, "%u", &input) != 1) {
		count = -EINVAL;
		return count;
	}

	if (input != gdata->row_coe) {
		gdata->row_coe = input;
	}

	return count;
}

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
static const struct proc_ops als_type_fops = {
	.proc_read = als_type_read_proc,
	.proc_lseek = default_llseek,
};

static const struct proc_ops red_max_lux_fops = {
	.proc_read = red_max_lux_read_proc,
	.proc_write = red_max_lux_write_proc,
	.proc_lseek = default_llseek,
};

static const struct proc_ops white_max_lux_fops = {
	.proc_read = white_max_lux_read_proc,
	.proc_write = white_max_lux_write_proc,
	.proc_lseek = default_llseek,
};

static const struct proc_ops blue_max_lux_fops = {
	.proc_read = blue_max_lux_read_proc,
	.proc_write = blue_max_lux_write_proc,
	.proc_lseek = default_llseek,
};

static const struct proc_ops green_max_lux_fops = {
	.proc_read = green_max_lux_read_proc,
	.proc_write = green_max_lux_write_proc,
	.proc_lseek = default_llseek,
};

static const struct proc_ops cali_coe_fops = {
	.proc_read = cali_coe_read_proc,
	.proc_write = cali_coe_write_proc,
	.proc_lseek = default_llseek,
};

static const struct proc_ops row_coe_fops = {
	.proc_read = row_coe_read_proc,
	.proc_write = row_coe_write_proc,
	.proc_lseek = default_llseek,
};
#else
static struct file_operations als_type_fops = {
	.read = als_type_read_proc,
};

static struct file_operations red_max_lux_fops = {
	.read = red_max_lux_read_proc,
	.write = red_max_lux_write_proc,
};

static struct file_operations white_max_lux_fops = {
	.read = white_max_lux_read_proc,
	.write = white_max_lux_write_proc,
};

static struct file_operations blue_max_lux_fops = {
	.read = blue_max_lux_read_proc,
	.write = blue_max_lux_write_proc,
};

static struct file_operations green_max_lux_fops = {
	.read = green_max_lux_read_proc,
	.write = green_max_lux_write_proc,
};

static struct file_operations cali_coe_fops = {
	.read = cali_coe_read_proc,
	.write = cali_coe_write_proc,
};

static struct file_operations row_coe_fops = {
	.read = row_coe_read_proc,
	.write = row_coe_write_proc,
};
#endif

static int oplus_als_cali_data_init(void)
{
	int rc = 0;
	struct proc_dir_entry *pentry;

	pr_info("%s call\n", __func__);

	if (gdata->proc_oplus_als) {
		printk("proc_oplus_als has alread inited\n");
		return 0;
	}

	gdata->proc_oplus_als =  proc_mkdir("als_cali", sensor_proc_dir);

	if (!gdata->proc_oplus_als) {
		pr_err("can't create proc_oplus_als proc\n");
		rc = -EFAULT;
		return rc;
	}

	pentry = proc_create("red_max_lux", 0666, gdata->proc_oplus_als,
			&red_max_lux_fops);

	if (!pentry) {
		pr_err("create red_max_lux proc failed.\n");
		rc = -EFAULT;
		return rc;
	}

	pentry = proc_create("green_max_lux", 0666, gdata->proc_oplus_als,
			&green_max_lux_fops);

	if (!pentry) {
		pr_err("create green_max_lux proc failed.\n");
		rc = -EFAULT;
		return rc;
	}

	pentry = proc_create("blue_max_lux", 0666, gdata->proc_oplus_als,
			&blue_max_lux_fops);

	if (!pentry) {
		pr_err("create blue_max_lux proc failed.\n");
		rc = -EFAULT;
		return rc;
	}

	pentry = proc_create("white_max_lux", 0666, gdata->proc_oplus_als,
			&white_max_lux_fops);

	if (!pentry) {
		pr_err("create white_max_lux proc failed.\n");
		rc = -EFAULT;
		return rc;
	}

	pentry = proc_create("cali_coe", 0666, gdata->proc_oplus_als,
			&cali_coe_fops);

	if (!pentry) {
		pr_err("create cali_coe proc failed.\n");
		rc = -EFAULT;
		return rc;
	}

	pentry = proc_create("row_coe", 0666, gdata->proc_oplus_als,
			&row_coe_fops);

	if (!pentry) {
		pr_err("create row_coe proc failed.\n");
		rc = -EFAULT;
		return rc;
	}

	pentry = proc_create("als_type", 0666, gdata->proc_oplus_als,
			&als_type_fops);

	if (!pentry) {
		pr_err("create als_type_fops proc failed.\n");
		rc = -EFAULT;
		return rc;
	}

	return 0;
}

static int sensor_ldo_init(struct device *dev)
{
	int ldo;
	struct device_node *np = dev->of_node;
	struct pinctrl	   *gpio_pinctrl;
	struct pinctrl_state *gpio_active;
	struct pinctrl_state *gpio_sleep;
	char * active = "sensor_ldo_active";
	char * sleep  = "sensor_ldo_sleep";

	if (of_property_read_u32(np, "ldo-gpio", &ldo)) {
		dev_err(dev, "%s: ldo-gpio init fail\n", __func__);
		return -EINVAL;
	}
	gpio_pinctrl = devm_pinctrl_get(dev);
	if (gpio_pinctrl == NULL) {
		dev_err(dev, "%s: ldo_pinctrl init fail\n", __func__);
		return -EINVAL;
	}

	gpio_active = pinctrl_lookup_state(gpio_pinctrl, active);
	if (gpio_active == NULL) {
		dev_err(dev, "%s: ldo_active init fail\n", __func__);
		return -EINVAL;
	}

	gpio_sleep = pinctrl_lookup_state(gpio_pinctrl, sleep);
	if (gpio_sleep == NULL) {
		dev_err(dev, "%s: ldo_sleep init fail\n", __func__);
		return -EINVAL;
	}
	gpio_direction_output(ldo, 1);
	pinctrl_select_state(gpio_pinctrl, gpio_active);
	return 0;
}

static int oplus_devinfo_probe(struct platform_device *pdev)
{
	struct sensor_info * chip = NULL;
	size_t smem_size = 0;
	void *smem_addr = NULL;
	int rc = 0;
	struct oplus_als_cali_data *data = NULL;

	pr_info("%s call\n", __func__);

#if (LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0))
		smem_size = ALIGN4(struct sensor_info);
		rc = qcom_smem_alloc(-1, SMEM_SENSOR, smem_size);
		if (rc < 0 && rc != -EEXIST) {
				pr_err("%s smem_alloc fail\n", __func__);
				rc = -EFAULT;
				return rc;
		}

		smem_size = 0;
#endif
	smem_addr = qcom_smem_get(-1,
			SMEM_SENSOR,
			&smem_size);

	if (IS_ERR(smem_addr)) {
		pr_err("unable to acquire smem SMEM_SENSOR entry, smem_addr %p\n", smem_addr);
		return -EPROBE_DEFER; /*return -EPROBE_DEFER if smem not ready*/
	}

	chip = (struct sensor_info *)(smem_addr);

	memset(chip, 0, sizeof(struct sensor_info));

	if (gdata) {
		printk("%s:just can be call one time\n", __func__);
		return 0;
	}

	data = kzalloc(sizeof(struct oplus_als_cali_data), GFP_KERNEL);

	if (data == NULL) {
		rc = -ENOMEM;
		printk("%s:kzalloc fail %d\n", __func__, rc);
		return rc;
	}

	gdata = data;

	platform_set_drvdata(pdev, chip);

	oplus_sensor_parse_dts(pdev);

	g_chip = chip;

	pr_info("%s success\n", __func__);

	sensor_proc_dir = proc_mkdir("sensor", NULL);
	if (!sensor_proc_dir) {
		pr_err("can't create proc_sensor proc\n");
		rc = -EFAULT;
		kfree(gdata);
		gdata = NULL;
		g_chip = NULL;
		return rc;
	}
	oplus_press_cali_data_init();
	rc = oplus_als_cali_data_init();

	if (rc < 0) {
		proc_remove(sensor_proc_dir);
		sensor_proc_dir = NULL;
		kfree(gdata);
		gdata = NULL;
		g_chip = NULL;
	}
	if (g_ldo_enable) {
		sensor_ldo_init(&pdev->dev);
	}
	return 0;
}

static int oplus_devinfo_remove(struct platform_device *pdev)
{
	if (g_chip) {
		g_chip = NULL;
	}

	if (sensor_proc_dir) {
		proc_remove(sensor_proc_dir);
		sensor_proc_dir = NULL;
	}

	if (gdata) {
		kfree(gdata);
		gdata = NULL;
	}

	oplus_press_cali_data_clean();

	return 0;
}

static const struct of_device_id of_drv_match[] = {
	{ .compatible = "oplus,sensor-devinfo"},
	{},
};
MODULE_DEVICE_TABLE(of, of_drv_match);

static struct platform_driver _driver = {
	.probe	  = oplus_devinfo_probe,
	.remove	 = oplus_devinfo_remove,
	.driver	 = {
	.name	   = "sensor_devinfo",
	.of_match_table = of_drv_match,
	},
};

static int __init oplus_devinfo_init(void)
{
	pr_info("oplus_devinfo_init call\n");

	platform_driver_register(&_driver);
	return 0;
}

arch_initcall(oplus_devinfo_init);

module_param_string(dsi_display0, sns_dsi_display_primary, MAX_CMDLINE_PARAM_LEN, 0600);
MODULE_PARM_DESC(dsi_display0,
	"oplus_sensor.dsi_display0=<display node> for primary dsi display node name");

MODULE_PARM_DESC(dsi_display0,
	"oplus_sensor_deviceinfo.dsi_display0=<display node> for primary dsi display node name");

module_param_string(dsi_display1, sns_dsi_display_secondary, MAX_CMDLINE_PARAM_LEN, 0600);
MODULE_PARM_DESC(dsi_display1,
	"oplus_sensor.dsi_display1=<display node> for secondary dsi display node name");

MODULE_PARM_DESC(dsi_display1,
	"oplus_sensor_deviceinfo.dsi_display1=<display node> for secondary dsi display node name");

MODULE_DESCRIPTION("sensor devinfo");
MODULE_LICENSE("GPL");

