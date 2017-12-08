/*
 * Copyright 2014-2017, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of the copyright holder nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * pmem.c -- pmem entry points for libpmem
 *
 *
 * PERSISTENT MEMORY INSTRUCTIONS ON X86
 *
 * The primary feature of this library is to provide a way to flush
 * changes to persistent memory as outlined below (note that many
 * of the decisions below are made at initialization time, and not
 * repeated every time a flush is requested).
 *
 * To flush a range to pmem when CLWB is available:
 *
 *	CLWB for each cache line in the given range.
 *
 *	SFENCE to ensure the CLWBs above have completed.
 *
 * To flush a range to pmem when CLFLUSHOPT is available and CLWB is not
 * (same as above but issue CLFLUSHOPT instead of CLWB):
 *
 *	CLFLUSHOPT for each cache line in the given range.
 *
 *	SFENCE to ensure the CLWBs above have completed.
 *
 * To flush a range to pmem when neither CLFLUSHOPT or CLWB are available
 * (same as above but fences surrounding CLFLUSH are not required):
 *
 *	CLFLUSH for each cache line in the given range.
 *
 * To memcpy a range of memory to pmem when MOVNT is available:
 *
 *	Copy any non-64-byte portion of the destination using MOV.
 *
 *	Use the flush flow above without the fence for the copied portion.
 *
 *	Copy using MOVNTDQ, up to any non-64-byte aligned end portion.
 *	(The MOVNT instructions bypass the cache, so no flush is required.)
 *
 *	Copy any unaligned end portion using MOV.
 *
 *	Use the flush flow above for the copied portion (including fence).
 *
 * To memcpy a range of memory to pmem when MOVNT is not available:
 *
 *	Just pass the call to the normal memcpy() followed by pmem_persist().
 *
 * To memset a non-trivial sized range of memory to pmem:
 *
 *	Same as the memcpy cases above but store the given value instead
 *	of reading values from the source.
 *
 *
 * INTERFACES FOR FLUSHING TO PERSISTENT MEMORY
 *
 * Given the flows above, three interfaces are provided for flushing a range
 * so that the caller has the ability to separate the steps when necessary,
 * but otherwise leaves the detection of available instructions to the libpmem:
 *
 * pmem_persist(addr, len)
 *
 *	This is the common case, which just calls the two other functions:
 *
 *		pmem_flush(addr, len);
 *		pmem_drain();
 *
 * pmem_flush(addr, len)
 *
 *	CLWB or CLFLUSHOPT or CLFLUSH for each cache line
 *
 * pmem_drain()
 *
 *	SFENCE unless using CLFLUSH
 *
 *
 * INTERFACES FOR COPYING/SETTING RANGES OF MEMORY
 *
 * Given the flows above, the following interfaces are provided for the
 * memmove/memcpy/memset operations to persistent memory:
 *
 * pmem_memmove_nodrain()
 *
 *	Checks for overlapped ranges to determine whether to copy from
 *	the beginning of the range or from the end.  If MOVNT instructions
 *	are available, uses the memory copy flow described above, otherwise
 *	calls the libc memmove() followed by pmem_flush(). Since no conditional
 *	compilation and/or architecture specific CFLAGS are in use at the
 *	moment, SSE2 ( thus movnt ) is just assumed to be available.
 *
 * pmem_memcpy_nodrain()
 *
 *	Just calls pmem_memmove_nodrain().
 *
 * pmem_memset_nodrain()
 *
 *	If MOVNT instructions are available, uses the memset flow described
 *	above, otherwise calls the libc memset() followed by pmem_flush().
 *
 * pmem_memmove_persist()
 * pmem_memcpy_persist()
 * pmem_memset_persist()
 *
 *	Calls the appropriate _nodrain() function followed by pmem_drain().
 *
 *
 * DECISIONS MADE AT INITIALIZATION TIME
 *
 * As much as possible, all decisions described above are made at library
 * initialization time.  This is achieved using function pointers that are
 * setup by pmem_init() when the library loads.
 *
 *	Func_predrain_fence is used by pmem_drain() to call one of:
 *		predrain_fence_empty()
 *		predrain_fence_sfence()
 *
 *	Func_flush is used by pmem_flush() to call one of:
 *		flush_clwb()
 *		flush_clflushopt()
 *		flush_clflush()
 *
 *	Func_memmove_nodrain is used by memmove_nodrain() to call one of:
 *		memmove_nodrain_normal()
 *		memmove_nodrain_movnt()
 *
 *	Func_memset_nodrain is used by memset_nodrain() to call one of:
 *		memset_nodrain_normal()
 *		memset_nodrain_movnt()
 *
 * DEBUG LOGGING
 *
 * Many of the functions here get called hundreds of times from loops
 * iterating over ranges, making the usual LOG() calls at level 3
 * impractical.  The call tracing log for those functions is set at 15.
 */

#include <sys/mman.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <emmintrin.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <inttypes.h>

#ifdef _WIN32
#include <memoryapi.h>
#endif

#include "libpmem.h"
#include "pmem.h"
#include "cpu.h"
#include "out.h"
#include "util.h"
#include "os.h"
#include "os_dimm.h"
#include "mmap.h"
#include "sys_util.h"
#include "file.h"
#include "valgrind_internal.h"
#include "ddax_deep_flush.h"

#ifndef _MSC_VER
/*
 * The x86 memory instructions are new enough that the compiler
 * intrinsic functions are not always available.  The intrinsic
 * functions are defined here in terms of asm statements for now.
 */
#define _mm_clflushopt(addr)\
	asm volatile(".byte 0x66; clflush %0" : "+m" (*(volatile char *)addr));
#define _mm_clwb(addr)\
	asm volatile(".byte 0x66; xsaveopt %0" : "+m" (*(volatile char *)addr));

#endif /* _MSC_VER */

#define FLUSH_ALIGN ((uintptr_t)64)

#define ALIGN_MASK	(FLUSH_ALIGN - 1)

#define CHUNK_SIZE	128 /* 16*8 */
#define CHUNK_SHIFT	7
#define CHUNK_MASK	(CHUNK_SIZE - 1)

#define DWORD_SIZE	4
#define DWORD_SHIFT	2
#define DWORD_MASK	(DWORD_SIZE - 1)

#define MOVNT_SIZE	16
#define MOVNT_MASK	(MOVNT_SIZE - 1)
#define MOVNT_SHIFT	4

#define MOVNT_THRESHOLD	256

static size_t Movnt_threshold = MOVNT_THRESHOLD;

enum map_tracker_flag {
	MTF_DIRECT_MAPPED = 0x0001,

	/* this should hold the value of all flags ORed for debug purpose */
	MTF_MASK = MTF_DIRECT_MAPPED
};

/*
 * this structure tracks the file mappings outstanding per file handle
 */
struct map_tracker {
	SORTEDQ_ENTRY(map_tracker) entry;
	uintptr_t base_addr;
	uintptr_t end_addr;
	enum map_tracker_flag flags;

#ifdef _WIN32
	/* Windows-specific data */
	HANDLE FileHandle;
	HANDLE FileMappingHandle;
	DWORD Access;
	os_off_t Offset;
	size_t FileLen;
#else
	dev_t dev_id;
	int region_id;
#endif
};

static SORTEDQ_HEAD(map_list_head, map_tracker) Mmap_list =
		SORTEDQ_HEAD_INITIALIZER(Mmap_list);

/*
 * pmem_has_hw_drain -- return whether or not HW drain was found
 *
 * Always false for x86: HW drain is done by HW with no SW involvement.
 */
int
pmem_has_hw_drain(void)
{
	LOG(3, NULL);

	return 0;
}

/*
 * predrain_fence_empty -- (internal) issue the pre-drain fence instruction
 */
static void
predrain_fence_empty(void)
{
	LOG(15, NULL);

	VALGRIND_DO_FENCE;
	/* nothing to do (because CLFLUSH did it for us) */
}

/*
 * predrain_fence_sfence -- (internal) issue the pre-drain fence instruction
 */
static void
predrain_fence_sfence(void)
{
	LOG(15, NULL);

	_mm_sfence();	/* ensure CLWB or CLFLUSHOPT completes */
}

/*
 * pmem_drain() calls through Func_predrain_fence to do the fence.  Although
 * initialized to predrain_fence_empty(), once the existence of the CLWB or
 * CLFLUSHOPT feature is confirmed by pmem_init() at library initialization
 * time, Func_predrain_fence is set to predrain_fence_sfence().  That's the
 * most common case on modern hardware that supports persistent memory.
 */
static void (*Func_predrain_fence)(void) = predrain_fence_empty;

/*
 * pmem_drain -- wait for any PM stores to drain from HW buffers
 */
void
pmem_drain(void)
{
	LOG(15, NULL);

	Func_predrain_fence();

	VALGRIND_DO_COMMIT;
	VALGRIND_DO_FENCE;
}

/*
 * flush_clflush -- (internal) flush the CPU cache, using clflush
 */
static void
flush_clflush(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	uintptr_t uptr;

	/*
	 * Loop through cache-line-size (typically 64B) aligned chunks
	 * covering the given range.
	 */
	for (uptr = (uintptr_t)addr & ~(FLUSH_ALIGN - 1);
		uptr < (uintptr_t)addr + len; uptr += FLUSH_ALIGN)
		_mm_clflush((char *)uptr);
}

/*
 * flush_clwb -- (internal) flush the CPU cache, using clwb
 */
static void
flush_clwb(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	uintptr_t uptr;

	/*
	 * Loop through cache-line-size (typically 64B) aligned chunks
	 * covering the given range.
	 */
	for (uptr = (uintptr_t)addr & ~(FLUSH_ALIGN - 1);
		uptr < (uintptr_t)addr + len; uptr += FLUSH_ALIGN) {
		_mm_clwb((char *)uptr);
	}
}

/*
 * flush_clflushopt -- (internal) flush the CPU cache, using clflushopt
 */
static void
flush_clflushopt(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	uintptr_t uptr;

	/*
	 * Loop through cache-line-size (typically 64B) aligned chunks
	 * covering the given range.
	 */
	for (uptr = (uintptr_t)addr & ~(FLUSH_ALIGN - 1);
		uptr < (uintptr_t)addr + len; uptr += FLUSH_ALIGN) {
		_mm_clflushopt((char *)uptr);
	}
}

/*
 * flush_empty -- (internal) do not flush the CPU cache
 */
static void
flush_empty(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	/* NOP */
}

/*
 * pmem_flush() calls through Func_flush to do the work.  Although
 * initialized to flush_clflush(), once the existence of the clflushopt
 * feature is confirmed by pmem_init() at library initialization time,
 * Func_flush is set to flush_clflushopt().  That's the most common case
 * on modern hardware that supports persistent memory.
 */
static void (*Func_flush)(const void *, size_t) = flush_clflush;

/*
 * pmem_flush -- flush processor cache for the given range
 */
void
pmem_flush(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	VALGRIND_DO_CHECK_MEM_IS_ADDRESSABLE(addr, len);

	Func_flush(addr, len);
}

/*
 * pmem_persist -- make any cached changes to a range of pmem persistent
 */
void
pmem_persist(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	pmem_flush(addr, len);
	pmem_drain();
}

/*
 * pmem_msync -- flush to persistence via msync
 *
 * Using msync() means this routine is less optimal for pmem (but it
 * still works) but it also works for any memory mapped file, unlike
 * pmem_persist() which is only safe where pmem_is_pmem() returns true.
 */
int
pmem_msync(const void *addr, size_t len)
{
	LOG(15, "addr %p len %zu", addr, len);

	VALGRIND_DO_CHECK_MEM_IS_ADDRESSABLE(addr, len);

	/*
	 * msync requires len to be a multiple of pagesize, so
	 * adjust addr and len to represent the full 4k chunks
	 * covering the given range.
	 */

	/* increase len by the amount we gain when we round addr down */
	len += (uintptr_t)addr & (Pagesize - 1);

	/* round addr down to page boundary */
	uintptr_t uptr = (uintptr_t)addr & ~((uintptr_t)Pagesize - 1);

	/*
	 * msync accepts addresses aligned to page boundary, so we may sync
	 * more and part of it may have been marked as undefined/inaccessible
	 * Msyncing such memory is not a bug, so as a workaround temporarily
	 * disable error reporting.
	 */
	VALGRIND_DO_DISABLE_ERROR_REPORTING;

	int ret;
	if ((ret = msync((void *)uptr, len, MS_SYNC)) < 0)
		ERR("!msync");

	VALGRIND_DO_ENABLE_ERROR_REPORTING;

	/* full flush */
	VALGRIND_DO_PERSIST(uptr, len);

	return ret;
}

/*
 * is_pmem_always -- (internal) always true version of pmem_is_pmem()
 */
static int
is_pmem_always(const void *addr, size_t len)
{
	LOG(3, "addr %p len %zu", addr, len);

	return 1;
}

/*
 * is_pmem_never -- (internal) never true version of pmem_is_pmem()
 */
static int
is_pmem_never(const void *addr, size_t len)
{
	LOG(3, "addr %p len %zu", addr, len);

	return 0;
}

/*
 * pmem_is_pmem() calls through Func_is_pmem to do the work.  Although
 * initialized to is_pmem_never(), once the existence of the clflush
 * feature is confirmed by pmem_init() at library initialization time,
 * Func_is_pmem is set to is_pmem_detect().  That's the most common case
 * on modern hardware.
 */
static int (*Func_is_pmem)(const void *addr, size_t len) = is_pmem_never;

/*
 * pmem_is_pmem_init -- (internal) initialize Func_is_pmem pointer
 *
 * This should be done only once - on the first call to pmem_is_pmem().
 * If PMEM_IS_PMEM_FORCE is set, it would override the default behavior
 * of pmem_is_pmem().
 */
static void
pmem_is_pmem_init(void)
{
	LOG(3, NULL);

	static volatile unsigned init;

	while (init != 2) {
		if (!util_bool_compare_and_swap32(&init, 0, 1))
			continue;

		/*
		 * For debugging/testing, allow pmem_is_pmem() to be forced
		 * to always true or never true using environment variable
		 * PMEM_IS_PMEM_FORCE values of zero or one.
		 *
		 * This isn't #ifdef DEBUG because it has a trivial performance
		 * impact and it may turn out to be useful as a "chicken bit"
		 * for systems where pmem_is_pmem() isn't correctly detecting
		 * true persistent memory.
		 */
		char *ptr = os_getenv("PMEM_IS_PMEM_FORCE");
		if (ptr) {
			int val = atoi(ptr);

			if (val == 0)
				Func_is_pmem = is_pmem_never;
			else if (val == 1)
				Func_is_pmem = is_pmem_always;

			VALGRIND_ANNOTATE_HAPPENS_BEFORE(&Func_is_pmem);

			LOG(4, "PMEM_IS_PMEM_FORCE=%d", val);
		}

		if (!util_bool_compare_and_swap32(&init, 1, 2))
			FATAL("util_bool_compare_and_swap32");
	}
}

/*
 * pmem_is_pmem -- return true if entire range is persistent memory
 */
int
pmem_is_pmem(const void *addr, size_t len)
{
	LOG(10, "addr %p len %zu", addr, len);

	static int once;

	/* This is not thread-safe, but pmem_is_pmem_init() is. */
	if (once == 0) {
		pmem_is_pmem_init();
		util_fetch_and_add32(&once, 1);
	}

	VALGRIND_ANNOTATE_HAPPENS_AFTER(&Func_is_pmem);
	return Func_is_pmem(addr, len);
}


/*
 * map_range_comparer -- (internal) compares the two mapping trackers
 */
static intptr_t
map_range_comparer(struct map_tracker *a, struct map_tracker *b)
{
	return ((intptr_t)a->base_addr - (intptr_t)b->base_addr);
}

/*
 * map_range_find --(internal) find the map tracker for given address range
 *
 * Returns the first entry at least partially overlapping given range.
 * It's up to the caller to check whether the entry exactly matches the range,
 * or if the range spans multiple entries.
 * The caller is also responsible for acquiring/releasing a lock on
 * the map tracking list.
 */
static struct map_tracker *
map_range_find(uintptr_t addr, size_t len)
{
	LOG(10, "addr 0x%016" PRIxPTR " len %zu", addr, len);

	uintptr_t end = addr + len;

	struct map_tracker *mt;
	SORTEDQ_FOREACH(mt, &Mmap_list, entry) {
		if (addr < mt->end_addr &&
		    (addr >= mt->base_addr || end > mt->base_addr))
			return mt;

		/* break if there is no chance to find matching entry */
		if (addr < mt->base_addr)
			break;
	}

	return NULL;
}

/*
 * map_range_register -- (internal) add a memory range into a map tracking list
 */
int
map_range_register(const void *addr, size_t len, int fd)
{
	LOG(3, "addr %p len %zu fd %d", addr, len, fd);

	int ret = 0;

	ASSERT(fd >= 0);

	if (os_rwlock_wrlock(&Mmap_list_lock)) {
		errno = EBUSY;
		ERR("!cannot lock map tracking list");
		return -1;
	}

	/* check if not tracked already */
	struct map_tracker *mt = map_range_find((uintptr_t)addr, len);
	ASSERTeq(mt, NULL);

	mt = Malloc(sizeof(struct map_tracker));
	if (mt == NULL) {
		ERR("!Malloc");
		ret = -1;
		goto err;
	}


	mt->base_addr = (uintptr_t)addr;
	mt->end_addr = mt->base_addr + len;
	mt->flags = MTF_DIRECT_MAPPED;

#ifndef _WIN32
	os_stat_t fd_stat;
	if (os_fstat(fd, &fd_stat) != 0) {
		ERR("!os_fstat");
		ret = -1;
		goto err;
	}
	mt->dev_id = fd_stat.st_rdev;
	mt->region_id = ddax_region_find(mt->dev_id);
#endif

	SORTEDQ_INSERT(&Mmap_list, mt, entry, struct map_tracker,
			map_range_comparer);

err:
	if (ret != 0 && mt != NULL)
		Free(mt);
	util_rwlock_unlock(&Mmap_list_lock);
	return ret;
}

/*
 * map_range_split -- (internal) remove or split a map tracking entry
 */
static int
map_range_split(struct map_tracker *mt, const void *addrp, const void *endp)
{
	LOG(3, "begin %p end %p", addrp, endp);

	uintptr_t addr = (uintptr_t)addrp;
	uintptr_t end = (uintptr_t)endp;
	ASSERTne(mt, NULL);
	ASSERTeq(addr % Mmap_align, 0);
	ASSERTeq(end % Mmap_align, 0);

	struct map_tracker *mtb = NULL;
	struct map_tracker *mte = NULL;

	/*
	 * 1)    b    e           b     e
	 *    xxxxxxxxxxxxx => xxx.......xxxx  -  mtb+mte
	 * 2)       b     e           b     e
	 *    xxxxxxxxxxxxx => xxxxxxx.......  -  mtb
	 * 3) b     e          b      e
	 *    xxxxxxxxxxxxx => ........xxxxxx  -  mte
	 * 4) b           e    b            e
	 *    xxxxxxxxxxxxx => ..............  -  <none>
	 */

	if (addr > mt->base_addr) {
		/* case #1/2 */
		/* new mapping at the beginning */
		mtb = Malloc(sizeof(struct map_tracker));
		if (mtb == NULL) {
			ERR("!Malloc");
			goto err;
		}

		mtb->flags = mt->flags;
		mtb->base_addr = mt->base_addr;
		mtb->end_addr = (uintptr_t)addr;
#ifndef _WIN32
		mtb->dev_id = mt->dev_id;
		mtb->region_id = mt->region_id;
#endif
	}

	if (end < mt->end_addr) {
		/* case #1/3 */
		/* new mapping at the end */
		mte = Malloc(sizeof(struct map_tracker));
		if (mte == NULL) {
			ERR("!Malloc");
			goto err;
		}

		mte->flags = mt->flags;
		mte->base_addr = end;
		mte->end_addr = mt->end_addr;
#ifndef _WIN32
		mte->dev_id = mt->dev_id;
		mte->region_id = mt->region_id;
#endif
	}

	SORTEDQ_REMOVE(&Mmap_list, mt, entry);

	if (mtb) {
		SORTEDQ_INSERT(&Mmap_list, mtb, entry,
				struct map_tracker, map_range_comparer);
	}

	if (mte) {
		SORTEDQ_INSERT(&Mmap_list, mte, entry,
				struct map_tracker, map_range_comparer);
	}

	/* free entry for the original mapping */
	Free(mt);
	return 0;

err:
	Free(mtb);
	Free(mte);
	return -1;
}

/*
 * map_range_unregister -- (internal) remove a memory range
 * from map tracking list
 *
 * Remove the region between [begin,end].  If it's in a middle of the existing
 * mapping, it results in two new map trackers.
 */
int
map_range_unregister(const void *addr, size_t len)
{
	LOG(3, "addr %p len %zu", addr, len);

	int ret = 0;

	if (os_rwlock_wrlock(&Mmap_list_lock)) {
		errno = EBUSY;
		ERR("!cannot lock map tracking list");
		return -1;
	}

	void *end = (char *)addr + len;

	/* XXX optimize the loop */
	struct map_tracker *mt;
	while ((mt = map_range_find((uintptr_t)addr, len)) != NULL) {
		if (map_range_split(mt, addr, end) != 0) {
			ret = -1;
			break;
		}
	}

	util_rwlock_unlock(&Mmap_list_lock);
	return ret;
}

/*
 * map_range_is_pmem -- (internal) return true if entire range
 * is persistent memory
 *
 * XXX Once the Linux and Windows implementation is unified, this function
 * would just become a new is_pmem_detect().
 */
int
map_range_is_pmem(const void *addrp, size_t len)
{
	LOG(10, "addr %p len %zu", addrp, len);

	uintptr_t addr = (uintptr_t)addrp;
	int retval = 1;

	if (os_rwlock_rdlock(&Mmap_list_lock)) {
		errno = EBUSY;
		ERR("!cannot lock map tracking list");
		return 0;
	}

	do {
		struct map_tracker *mt = map_range_find(addr, len);
		if (mt == NULL) {
			LOG(4, "address not found 0x%016" PRIxPTR, addr);
			retval = 0;
			break;
		}

		LOG(10, "range found - begin 0x%016" PRIxPTR
				" end 0x%016" PRIxPTR
				" flags %x",
				mt->base_addr, mt->end_addr, mt->flags);

		if (mt->base_addr > addr) {
			LOG(10, "base address doesn't match: "
				"0x%" PRIxPTR " > 0x%" PRIxPTR,
					mt->base_addr, addr);
			retval = 0;
			break;
		}

		retval &= ((mt->flags & MTF_DIRECT_MAPPED) != 0);

		uintptr_t map_len = mt->end_addr - addr;
		if (map_len > len)
			map_len = len;
		len -= map_len;
		addr += map_len;
	} while (len > 0);

	util_rwlock_unlock(&Mmap_list_lock);

	return retval;
}


#define PMEM_FILE_ALL_FLAGS\
	(PMEM_FILE_CREATE|PMEM_FILE_EXCL|PMEM_FILE_SPARSE|PMEM_FILE_TMPFILE)

#define PMEM_DAX_VALID_FLAGS\
	(PMEM_FILE_CREATE|PMEM_FILE_SPARSE)

/*
 * pmem_map_fileU -- create or open the file and map it to memory
 */
#ifndef _WIN32
static inline
#endif
void *
pmem_map_fileU(const char *path, size_t len, int flags,
	mode_t mode, size_t *mapped_lenp, int *is_pmemp)
{
	LOG(3, "path \"%s\" size %zu flags %x mode %o mapped_lenp %p "
		"is_pmemp %p", path, len, flags, mode, mapped_lenp, is_pmemp);

	int oerrno;
	int fd;
	int open_flags = O_RDWR;
	int delete_on_err = 0;
	int is_dev_dax = util_file_is_device_dax(path);

	if (flags & ~(PMEM_FILE_ALL_FLAGS)) {
		ERR("invalid flag specified %x", flags);
		errno = EINVAL;
		return NULL;
	}

	if (is_dev_dax) {
		if (flags & ~(PMEM_DAX_VALID_FLAGS)) {
			ERR("flag unsupported for Device DAX %x", flags);
			errno = EINVAL;
			return NULL;
		} else {
			/* we are ignoring all of the flags */
			flags = 0;
			ssize_t actual_len = util_file_get_size(path);
			if (actual_len < 0) {
				ERR("unable to read Device DAX size");
				errno = EINVAL;
				return NULL;
			}
			if (len != 0 && len != (size_t)actual_len) {
				ERR("Device DAX length must be either 0 or "
					"the exact size of the device %zu",
					len);
				errno = EINVAL;
				return NULL;
			}
			len = 0;
		}
	}

	if (flags & PMEM_FILE_CREATE) {
		if ((os_off_t)len < 0) {
			ERR("invalid file length %zu", len);
			errno = EINVAL;
			return NULL;
		}
		open_flags |= O_CREAT;
	}

	if (flags & PMEM_FILE_EXCL)
		open_flags |= O_EXCL;

	if ((len != 0) && !(flags & PMEM_FILE_CREATE)) {
		ERR("non-zero 'len' not allowed without PMEM_FILE_CREATE");
		errno = EINVAL;
		return NULL;
	}

	if ((len == 0) && (flags & PMEM_FILE_CREATE)) {
		ERR("zero 'len' not allowed with PMEM_FILE_CREATE");
		errno = EINVAL;
		return NULL;
	}

	if ((flags & PMEM_FILE_TMPFILE) && !(flags & PMEM_FILE_CREATE)) {
		ERR("PMEM_FILE_TMPFILE not allowed without PMEM_FILE_CREATE");
		errno = EINVAL;
		return NULL;
	}

	if (flags & PMEM_FILE_TMPFILE) {
		if ((fd = util_tmpfile(path,
					OS_DIR_SEP_STR"pmem.XXXXXX",
					open_flags & O_EXCL)) < 0) {
			LOG(2, "failed to create temporary file at \"%s\"",
				path);
			return NULL;
		}
	} else {
		if ((fd = os_open(path, open_flags, mode)) < 0) {
			ERR("!open %s", path);
			return NULL;
		}
		if ((flags & PMEM_FILE_CREATE) && (flags & PMEM_FILE_EXCL))
			delete_on_err = 1;
	}

	if (flags & PMEM_FILE_CREATE) {
		/*
		 * Always set length of file to 'len'.
		 * (May either extend or truncate existing file.)
		 */
		if (os_ftruncate(fd, (os_off_t)len) != 0) {
			ERR("!ftruncate");
			goto err;
		}
		if ((flags & PMEM_FILE_SPARSE) == 0) {
			if ((errno = os_posix_fallocate(fd, 0,
							(os_off_t)len)) != 0) {
				ERR("!posix_fallocate");
				goto err;
			}
		}
	} else {
		ssize_t actual_size = util_file_get_size(path);
		if (actual_size < 0) {
			ERR("stat %s: negative size", path);
			errno = EINVAL;
			goto err;
		}

		len = (size_t)actual_size;
	}

	void *addr;
	if ((addr = util_map(fd, len, MAP_SHARED, 0, 0)) == NULL)
		goto err;    /* util_map() set errno, called LOG */

#ifndef _WIN32
	/* XXX only Device DAX regions (PMEM) are tracked so far */
	if (is_dev_dax && map_range_register(addr, len, fd) != 0) {
		LOG(2, "can't track mapped region");
	}
#endif

	if (mapped_lenp != NULL)
		*mapped_lenp = len;

	if (is_pmemp != NULL)
		*is_pmemp = is_dev_dax || pmem_is_pmem(addr, len);

	LOG(3, "returning %p", addr);

	VALGRIND_REGISTER_PMEM_MAPPING(addr, len);
	VALGRIND_REGISTER_PMEM_FILE(fd, addr, len, 0);

	(void) os_close(fd);

	return addr;

err:
	oerrno = errno;
	(void) os_close(fd);
	if (delete_on_err)
		(void) os_unlink(path);
	errno = oerrno;
	return NULL;
}

#ifndef _WIN32
/*
 * pmem_map_file -- create or open the file and map it to memory
 */
void *
pmem_map_file(const char *path, size_t len, int flags,
	mode_t mode, size_t *mapped_lenp, int *is_pmemp)
{
	return pmem_map_fileU(path, len, flags, mode, mapped_lenp, is_pmemp);
}
#else
/*
 * pmem_map_fileW -- create or open the file and map it to memory
 */
void *
pmem_map_fileW(const wchar_t *path, size_t len, int flags, mode_t mode,
		size_t *mapped_lenp, int *is_pmemp) {
	char *upath = util_toUTF8(path);
	if (upath == NULL)
		return NULL;

	void *ret = pmem_map_fileU(upath, len, flags, mode, mapped_lenp,
					is_pmemp);

	util_free_UTF8(upath);
	return ret;
}
#endif

/*
 * pmem_unmap -- unmap the specified region
 */
int
pmem_unmap(void *addr, size_t len)
{
	LOG(3, "addr %p len %zu", addr, len);

#ifndef _WIN32
	map_range_unregister(addr, len);
#endif
	VALGRIND_REMOVE_PMEM_MAPPING(addr, len);
	return util_unmap(addr, len);
}

/*
 * memmove_nodrain_normal -- (internal) memmove to pmem without hw drain
 */
static void *
memmove_nodrain_normal(void *pmemdest, const void *src, size_t len)
{
	LOG(15, "pmemdest %p src %p len %zu", pmemdest, src, len);

	memmove(pmemdest, src, len);
	pmem_flush(pmemdest, len);
	return pmemdest;
}

/*
 * memmove_nodrain_movnt -- (internal) memmove to pmem without hw drain, movnt
 */
static void *
memmove_nodrain_movnt(void *pmemdest, const void *src, size_t len)
{
	LOG(15, "pmemdest %p src %p len %zu", pmemdest, src, len);

	__m128i xmm0, xmm1, xmm2, xmm3, xmm4, xmm5, xmm6, xmm7;
	size_t i;
	__m128i *d;
	__m128i *s;
	void *dest1 = pmemdest;
	size_t cnt;

	if (len == 0 || src == pmemdest)
		return pmemdest;

	if (len < Movnt_threshold) {
		memmove(pmemdest, src, len);
		pmem_flush(pmemdest, len);
		return pmemdest;
	}

	if ((uintptr_t)dest1 - (uintptr_t)src >= len) {
		/*
		 * Copy the range in the forward direction.
		 *
		 * This is the most common, most optimized case, used unless
		 * the overlap specifically prevents it.
		 */

		/* copy up to FLUSH_ALIGN boundary */
		cnt = (uint64_t)dest1 & ALIGN_MASK;
		if (cnt > 0) {
			cnt = FLUSH_ALIGN - cnt;

			/* never try to copy more the len bytes */
			if (cnt > len)
				cnt = len;

			uint8_t *d8 = (uint8_t *)dest1;
			const uint8_t *s8 = (uint8_t *)src;
			for (i = 0; i < cnt; i++) {
				*d8 = *s8;
				d8++;
				s8++;
			}
			pmem_flush(dest1, cnt);
			dest1 = (char *)dest1 + cnt;
			src = (char *)src + cnt;
			len -= cnt;
		}

		d = (__m128i *)dest1;
		s = (__m128i *)src;

		cnt = len >> CHUNK_SHIFT;
		for (i = 0; i < cnt; i++) {
			xmm0 = _mm_loadu_si128(s);
			xmm1 = _mm_loadu_si128(s + 1);
			xmm2 = _mm_loadu_si128(s + 2);
			xmm3 = _mm_loadu_si128(s + 3);
			xmm4 = _mm_loadu_si128(s + 4);
			xmm5 = _mm_loadu_si128(s + 5);
			xmm6 = _mm_loadu_si128(s + 6);
			xmm7 = _mm_loadu_si128(s + 7);
			s += 8;
			_mm_stream_si128(d,	xmm0);
			_mm_stream_si128(d + 1,	xmm1);
			_mm_stream_si128(d + 2,	xmm2);
			_mm_stream_si128(d + 3,	xmm3);
			_mm_stream_si128(d + 4,	xmm4);
			_mm_stream_si128(d + 5, xmm5);
			_mm_stream_si128(d + 6,	xmm6);
			_mm_stream_si128(d + 7,	xmm7);
			VALGRIND_DO_FLUSH(d, 8 * sizeof(*d));
			d += 8;
		}

		/* copy the tail (<128 bytes) in 16 bytes chunks */
		len &= CHUNK_MASK;
		if (len != 0) {
			cnt = len >> MOVNT_SHIFT;
			for (i = 0; i < cnt; i++) {
				xmm0 = _mm_loadu_si128(s);
				_mm_stream_si128(d, xmm0);
				VALGRIND_DO_FLUSH(d, sizeof(*d));
				s++;
				d++;
			}
		}

		/* copy the last bytes (<16), first dwords then bytes */
		len &= MOVNT_MASK;
		if (len != 0) {
			cnt = len >> DWORD_SHIFT;
			int32_t *d32 = (int32_t *)d;
			int32_t *s32 = (int32_t *)s;
			for (i = 0; i < cnt; i++) {
				_mm_stream_si32(d32, *s32);
				VALGRIND_DO_FLUSH(d32, sizeof(*d32));
				d32++;
				s32++;
			}
			cnt = len & DWORD_MASK;
			uint8_t *d8 = (uint8_t *)d32;
			const uint8_t *s8 = (uint8_t *)s32;

			for (i = 0; i < cnt; i++) {
				*d8 = *s8;
				d8++;
				s8++;
			}
			pmem_flush(d32, cnt);
		}
	} else {
		/*
		 * Copy the range in the backward direction.
		 *
		 * This prevents overwriting source data due to an
		 * overlapped destination range.
		 */

		dest1 = (char *)dest1 + len;
		src = (char *)src + len;

		cnt = (uint64_t)dest1 & ALIGN_MASK;
		if (cnt > 0) {
			/* never try to copy more the len bytes */
			if (cnt > len)
				cnt = len;

			uint8_t *d8 = (uint8_t *)dest1;
			const uint8_t *s8 = (uint8_t *)src;
			for (i = 0; i < cnt; i++) {
				d8--;
				s8--;
				*d8 = *s8;
			}
			pmem_flush(d8, cnt);
			dest1 = (char *)dest1 - cnt;
			src = (char *)src - cnt;
			len -= cnt;
		}

		d = (__m128i *)dest1;
		s = (__m128i *)src;

		cnt = len >> CHUNK_SHIFT;
		for (i = 0; i < cnt; i++) {
			xmm0 = _mm_loadu_si128(s - 1);
			xmm1 = _mm_loadu_si128(s - 2);
			xmm2 = _mm_loadu_si128(s - 3);
			xmm3 = _mm_loadu_si128(s - 4);
			xmm4 = _mm_loadu_si128(s - 5);
			xmm5 = _mm_loadu_si128(s - 6);
			xmm6 = _mm_loadu_si128(s - 7);
			xmm7 = _mm_loadu_si128(s - 8);
			s -= 8;
			_mm_stream_si128(d - 1, xmm0);
			_mm_stream_si128(d - 2, xmm1);
			_mm_stream_si128(d - 3, xmm2);
			_mm_stream_si128(d - 4, xmm3);
			_mm_stream_si128(d - 5, xmm4);
			_mm_stream_si128(d - 6, xmm5);
			_mm_stream_si128(d - 7, xmm6);
			_mm_stream_si128(d - 8, xmm7);
			d -= 8;
			VALGRIND_DO_FLUSH(d, 8 * sizeof(*d));
		}

		/* copy the tail (<128 bytes) in 16 bytes chunks */
		len &= CHUNK_MASK;
		if (len != 0) {
			cnt = len >> MOVNT_SHIFT;
			for (i = 0; i < cnt; i++) {
				d--;
				s--;
				xmm0 = _mm_loadu_si128(s);
				_mm_stream_si128(d, xmm0);
				VALGRIND_DO_FLUSH(d, sizeof(*d));
			}
		}

		/* copy the last bytes (<16), first dwords then bytes */
		len &= MOVNT_MASK;
		if (len != 0) {
			cnt = len >> DWORD_SHIFT;
			int32_t *d32 = (int32_t *)d;
			int32_t *s32 = (int32_t *)s;
			for (i = 0; i < cnt; i++) {
				d32--;
				s32--;
				_mm_stream_si32(d32, *s32);
				VALGRIND_DO_FLUSH(d32, sizeof(*d32));
			}

			cnt = len & DWORD_MASK;
			uint8_t *d8 = (uint8_t *)d32;
			const uint8_t *s8 = (uint8_t *)s32;

			for (i = 0; i < cnt; i++) {
				d8--;
				s8--;
				*d8 = *s8;
			}
			pmem_flush(d8, cnt);
		}
	}

	/* serialize non-temporal store instructions */
	predrain_fence_sfence();

	return pmemdest;
}

/*
 * pmem_memmove_nodrain() calls through Func_memmove_nodrain to do the work.
 * Although initialized to memmove_nodrain_normal(), once the existence of the
 * sse2 feature is confirmed by pmem_init() at library initialization time,
 * Func_memmove_nodrain is set to memmove_nodrain_movnt().  That's the most
 * common case on modern hardware that supports persistent memory.
 */
static void *(*Func_memmove_nodrain)
	(void *pmemdest, const void *src, size_t len) = memmove_nodrain_normal;

/*
 * pmem_memmove_nodrain -- memmove to pmem without hw drain
 */
void *
pmem_memmove_nodrain(void *pmemdest, const void *src, size_t len)
{
	LOG(15, "pmemdest %p src %p len %zu", pmemdest, src, len);

	return Func_memmove_nodrain(pmemdest, src, len);
}

/*
 * pmem_memcpy_nodrain -- memcpy to pmem without hw drain
 */
void *
pmem_memcpy_nodrain(void *pmemdest, const void *src, size_t len)
{
	LOG(15, "pmemdest %p src %p len %zu", pmemdest, src, len);

	return pmem_memmove_nodrain(pmemdest, src, len);
}

/*
 * pmem_memmove_persist -- memmove to pmem
 */
void *
pmem_memmove_persist(void *pmemdest, const void *src, size_t len)
{
	LOG(15, "pmemdest %p src %p len %zu", pmemdest, src, len);

	pmem_memmove_nodrain(pmemdest, src, len);
	pmem_drain();
	return pmemdest;
}

/*
 * pmem_memcpy_persist -- memcpy to pmem
 */
void *
pmem_memcpy_persist(void *pmemdest, const void *src, size_t len)
{
	LOG(15, "pmemdest %p src %p len %zu", pmemdest, src, len);

	pmem_memcpy_nodrain(pmemdest, src, len);
	pmem_drain();
	return pmemdest;
}

/*
 * memset_nodrain_normal -- (internal) memset to pmem without hw drain, normal
 */
static void *
memset_nodrain_normal(void *pmemdest, int c, size_t len)
{
	LOG(15, "pmemdest %p c 0x%x len %zu", pmemdest, c, len);

	memset(pmemdest, c, len);
	pmem_flush(pmemdest, len);
	return pmemdest;
}

/*
 * memset_nodrain_movnt -- (internal) memset to pmem without hw drain, movnt
 */
static void *
memset_nodrain_movnt(void *pmemdest, int c, size_t len)
{
	LOG(15, "pmemdest %p c 0x%x len %zu", pmemdest, c, len);

	size_t i;
	void *dest1 = pmemdest;
	size_t cnt;
	__m128i xmm0;
	__m128i *d;

	if (len < Movnt_threshold) {
		memset(pmemdest, c, len);
		pmem_flush(pmemdest, len);
		return pmemdest;
	}

	/* memset up to the next FLUSH_ALIGN boundary */
	cnt = (uint64_t)dest1 & ALIGN_MASK;
	if (cnt != 0) {
		cnt = FLUSH_ALIGN - cnt;

		if (cnt > len)
			cnt = len;

		memset(dest1, c, cnt);
		pmem_flush(dest1, cnt);
		len -= cnt;
		dest1 = (char *)dest1 + cnt;
	}

	xmm0 = _mm_set1_epi8((char)c);

	d = (__m128i *)dest1;
	cnt = len / CHUNK_SIZE;
	if (cnt != 0) {
		for (i = 0; i < cnt; i++) {
			_mm_stream_si128(d, xmm0);
			_mm_stream_si128(d + 1, xmm0);
			_mm_stream_si128(d + 2, xmm0);
			_mm_stream_si128(d + 3, xmm0);
			_mm_stream_si128(d + 4, xmm0);
			_mm_stream_si128(d + 5, xmm0);
			_mm_stream_si128(d + 6, xmm0);
			_mm_stream_si128(d + 7, xmm0);
			VALGRIND_DO_FLUSH(d, 8 * sizeof(*d));
			d += 8;
		}
	}
	/* memset the tail (<128 bytes) in 16 bytes chunks */
	len &= CHUNK_MASK;
	if (len != 0) {
		cnt = len >> MOVNT_SHIFT;
		for (i = 0; i < cnt; i++) {
			_mm_stream_si128(d, xmm0);
			VALGRIND_DO_FLUSH(d, sizeof(*d));
			d++;
		}
	}

	/* memset the last bytes (<16), first dwords then bytes */
	len &= MOVNT_MASK;
	if (len != 0) {
		int32_t *d32 = (int32_t *)d;
		cnt = len >> DWORD_SHIFT;
		if (cnt != 0) {
			for (i = 0; i < cnt; i++) {
				_mm_stream_si32(d32,
					_mm_cvtsi128_si32(xmm0));
				VALGRIND_DO_FLUSH(d32, sizeof(*d32));
				d32++;
			}
		}

		/* at this point the cnt < 16 so use memset */
		cnt = len & DWORD_MASK;
		if (cnt != 0) {
			memset((void *)d32, c, cnt);
			pmem_flush(d32, cnt);
		}
	}

	/* serialize non-temporal store instructions */
	predrain_fence_sfence();

	return pmemdest;
}

/*
 * pmem_memset_nodrain() calls through Func_memset_nodrain to do the work.
 * Although initialized to memset_nodrain_normal(), once the existence of the
 * sse2 feature is confirmed by pmem_init() at library initialization time,
 * Func_memset_nodrain is set to memset_nodrain_movnt().  That's the most
 * common case on modern hardware that supports persistent memory.
 */
static void *(*Func_memset_nodrain)
	(void *pmemdest, int c, size_t len) = memset_nodrain_normal;

/*
 * pmem_memset_nodrain -- memset to pmem without hw drain
 */
void *
pmem_memset_nodrain(void *pmemdest, int c, size_t len)
{
	LOG(15, "pmemdest %p c 0x%x len %zu", pmemdest, c, len);

	return Func_memset_nodrain(pmemdest, c, len);
}

/*
 * pmem_memset_persist -- memset to pmem
 */
void *
pmem_memset_persist(void *pmemdest, int c, size_t len)
{
	LOG(15, "pmemdest %p c 0x%x len %zu", pmemdest, c, len);

	pmem_memset_nodrain(pmemdest, c, len);
	pmem_drain();
	return pmemdest;
}

/*
 * pmem_log_cpuinfo -- log the results of cpu dispatching decisions,
 * and verify them
 */
static void
pmem_log_cpuinfo(void)
{
	LOG(3, NULL);

	if (Func_flush == flush_clwb)
		LOG(3, "using clwb");
	else if (Func_flush == flush_clflushopt)
		LOG(3, "using clflushopt");
	else if (Func_flush == flush_clflush)
		LOG(3, "using clflush");
	else if (Func_flush == flush_empty)
		LOG(3, "not flushing CPU cache");
	else
		FATAL("invalid flush function address");

	if (Func_memmove_nodrain == memmove_nodrain_movnt)
		LOG(3, "using movnt");
	else if (Func_memmove_nodrain == memmove_nodrain_normal)
		LOG(3, "not using movnt");
	else
		FATAL("invalid memove_nodrain function address");
}

/*
 * pmem_get_cpuinfo -- configure libpmem based on CPUID
 */
static void
pmem_get_cpuinfo(void)
{
	LOG(3, NULL);

	if (is_cpu_clflush_present()) {
		Func_is_pmem = is_pmem_detect;
		LOG(3, "clflush supported");
	}

	if (is_cpu_clflushopt_present()) {
		LOG(3, "clflushopt supported");

		char *e = os_getenv("PMEM_NO_CLFLUSHOPT");
		if (e && strcmp(e, "1") == 0)
			LOG(3, "PMEM_NO_CLFLUSHOPT forced no clflushopt");
		else {
			Func_flush = flush_clflushopt;
			Func_predrain_fence = predrain_fence_sfence;
		}
	}

	if (is_cpu_clwb_present()) {
		LOG(3, "clwb supported");

		char *e = os_getenv("PMEM_NO_CLWB");
		if (e && strcmp(e, "1") == 0)
			LOG(3, "PMEM_NO_CLWB forced no clwb");
		else {
			Func_flush = flush_clwb;
			Func_predrain_fence = predrain_fence_sfence;
		}
	}
}

/*
 * pmem_init -- load-time initialization for pmem.c
 */
void
pmem_init(void)
{
	LOG(3, NULL);

	pmem_get_cpuinfo();

	char *e = os_getenv("PMEM_NO_FLUSH");
	if (e && strcmp(e, "1") == 0) {
		LOG(3, "forced not flushing CPU cache");
		Func_flush = flush_empty;
		Func_predrain_fence = predrain_fence_sfence;
	}

	/*
	 * For testing, allow overriding the default threshold
	 * for using non-temporal stores in pmem_memcpy_*(), pmem_memmove_*()
	 * and pmem_memset_*().
	 * It has no effect if movnt is not supported or disabled.
	 */
	char *ptr = os_getenv("PMEM_MOVNT_THRESHOLD");
	if (ptr) {
		long long val = atoll(ptr);

		if (val < 0)
			LOG(3, "Invalid PMEM_MOVNT_THRESHOLD");
		else {
			LOG(3, "PMEM_MOVNT_THRESHOLD set to %zu", (size_t)val);
			Movnt_threshold = (size_t)val;
		}
	}

	ptr = os_getenv("PMEM_NO_MOVNT");
	if (ptr && strcmp(ptr, "1") == 0)
		LOG(3, "PMEM_NO_MOVNT forced no movnt");
	else {
		Func_memmove_nodrain = memmove_nodrain_movnt;
		Func_memset_nodrain = memset_nodrain_movnt;
	}

	pmem_log_cpuinfo();

#if defined(_WIN32) && (NTDDI_VERSION >= NTDDI_WIN10_RS1)
	Func_qvmi = (PQVM)GetProcAddress(
			GetModuleHandle(TEXT("KernelBase.dll")),
			"QueryVirtualMemoryInformation");
#endif
}


/*
 * range_deep_flush -- (internal) perform deep flush of given address range
 */
static int
range_deep_flush(uintptr_t addr, size_t len)
{
#if !defined(_WIN32) && !defined(_FreeBSD)
	while (len != 0) {
		const struct map_tracker *mt = map_range_find(addr, len);

		if (mt == NULL) /* no more overlapping track regions */
			return pmem_msync((void *)addr, len);

		if (mt->base_addr > addr) {
			size_t curr_len = mt->base_addr - addr;
			if (curr_len > len)
				curr_len = len;
			if (pmem_msync((void *)addr, curr_len) != 0)
				return -1;
			if ((len -= curr_len) == 0)
				return 0;
			addr = mt->base_addr;
		}

		if (ddax_deep_flush_final(mt->region_id) < 0) {
			ERR("!cannot perform final write to deep_flush");
			return -1;
		}

		if (mt->end_addr >= addr + len)
			return 0;

		len -= mt->end_addr - addr;
		addr = mt->end_addr;
	}
	return 0;
#else
	return msync((void *) addr, len, MS_SYNC);
#endif
}


/*
 * pmem_deep_flush -- perform deep flush on a memory range
 *
 * It merely acts as wrapper around an msync call in most cases, the only
 * exception is the case of an mmap'ed DAX device on Linux.
 */
int
pmem_deep_flush(const void *addr, size_t len)
{
	LOG(3, "addr %p len %zu", addr, len);

	if (len == 0)
		return 0;

	if (os_rwlock_rdlock(&Mmap_list_lock)) {
		errno = EBUSY;
		ERR("!cannot lock map tracking list");
		return -1;
	}

	int retval = range_deep_flush((uintptr_t)addr, len);

	util_rwlock_unlock(&Mmap_list_lock);

	return retval;
}
