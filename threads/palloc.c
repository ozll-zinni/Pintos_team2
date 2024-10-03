#include "threads/palloc.h"
#include <bitmap.h>
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "threads/init.h"
#include "threads/loader.h"
#include "threads/synch.h"
#include "threads/vaddr.h"

/* Page allocator.  Hands out memory in page-size (or
   page-multiple) chunks.  See malloc.h for an allocator that
   hands out smaller chunks.

   System memory is divided into two "pools" called the kernel
   and user pools.  The user pool is for user (virtual) memory
   pages, the kernel pool for everything else.  The idea here is
   that the kernel needs to have memory for its own operations
   even if user processes are swapping like mad.

   By default, half of system RAM is given to the kernel pool and
   half to the user pool.  That should be huge overkill for the
   kernel pool, but that's just fine for demonstration purposes. */

/* A memory pool. */
struct pool {
	struct lock lock;               /* Mutual exclusion. */
	struct bitmap *used_map;        /* Bitmap of free pages. */
	uint8_t *base;                  /* Base of pool. */
};

/* Two pools: one for kernel data, one for user pages. */
static struct pool kernel_pool, user_pool;

/* Maximum number of pages to put in user pool. */
size_t user_page_limit = SIZE_MAX;
static void
init_pool (struct pool *p, void **bm_base, uint64_t start, uint64_t end);

static bool page_from_pool (const struct pool *, void *page);

/* multiboot info */
struct multiboot_info {
	uint32_t flags;
	uint32_t mem_low;
	uint32_t mem_high;
	uint32_t __unused[8];
	uint32_t mmap_len;
	uint32_t mmap_base;
};

/* e820 entry */
struct e820_entry {
	uint32_t size;
	uint32_t mem_lo;
	uint32_t mem_hi;
	uint32_t len_lo;
	uint32_t len_hi;
	uint32_t type;
};

/* Represent the range information of the ext_mem/base_mem */
struct area {
	uint64_t start;
	uint64_t end;
	uint64_t size;
};

#define BASE_MEM_THRESHOLD 0x100000
#define USABLE 1
#define ACPI_RECLAIMABLE 3
#define APPEND_HILO(hi, lo) (((uint64_t) ((hi)) << 32) + (lo))

/* Iterate on the e820 entry, parse the range of basemem and extmem. */
static void
resolve_area_info (struct area *base_mem, struct area *ext_mem) {
	struct multiboot_info *mb_info = ptov (MULTIBOOT_INFO);
	struct e820_entry *entries = ptov (mb_info->mmap_base);
	uint32_t i;

	for (i = 0; i < mb_info->mmap_len / sizeof (struct e820_entry); i++) {
		struct e820_entry *entry = &entries[i];
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE) {
			uint64_t start = APPEND_HILO (entry->mem_hi, entry->mem_lo);
			uint64_t size = APPEND_HILO (entry->len_hi, entry->len_lo);
			uint64_t end = start + size;
			printf("%llx ~ %llx %d\n", start, end, entry->type);

			struct area *area = start < BASE_MEM_THRESHOLD ? base_mem : ext_mem;

			// First entry that belong to this area.
			if (area->size == 0) {
				*area = (struct area) {
					.start = start,
					.end = end,
					.size = size,
				};
			} else {  // otherwise
				// Extend start
				if (area->start > start)
					area->start = start;
				// Extend end
				if (area->end < end)
					area->end = end;
				// Extend size
				area->size += size;
			}
		}
	}
}

/*
 * Populate the pool.
 * All the pages are manged by this allocator, even include code page.
 * Basically, give half of memory to kernel, half to user.
 * We push base_mem portion to the kernel as much as possible.
 */
static void
populate_pools (struct area *base_mem, struct area *ext_mem) {
	extern char _end;
	void *free_start = pg_round_up (&_end);

	uint64_t total_pages = (base_mem->size + ext_mem->size) / PGSIZE;
	uint64_t user_pages = total_pages / 2 > user_page_limit ?
		user_page_limit : total_pages / 2;
	uint64_t kern_pages = total_pages - user_pages;

	// Parse E820 map to claim the memory region for each pool.
	enum { KERN_START, KERN, USER_START, USER } state = KERN_START;
	uint64_t rem = kern_pages;
	uint64_t region_start = 0, end = 0, start, size, size_in_pg;

	struct multiboot_info *mb_info = ptov (MULTIBOOT_INFO);
	struct e820_entry *entries = ptov (mb_info->mmap_base);

	uint32_t i;
	for (i = 0; i < mb_info->mmap_len / sizeof (struct e820_entry); i++) {
		struct e820_entry *entry = &entries[i];
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE) {
			start = (uint64_t) ptov (APPEND_HILO (entry->mem_hi, entry->mem_lo));
			size = APPEND_HILO (entry->len_hi, entry->len_lo);
			end = start + size;
			size_in_pg = size / PGSIZE;

			if (state == KERN_START) {
				region_start = start;
				state = KERN;
			}

			switch (state) {
				case KERN:
					if (rem > size_in_pg) {
						rem -= size_in_pg;
						break;
					}
					// generate kernel pool
					init_pool (&kernel_pool,
							&free_start, region_start, start + rem * PGSIZE);
					// Transition to the next state
					if (rem == size_in_pg) {
						rem = user_pages;
						state = USER_START;
					} else {
						region_start = start + rem * PGSIZE;
						rem = user_pages - size_in_pg + rem;
						state = USER;
					}
					break;
				case USER_START:
					region_start = start;
					state = USER;
					break;
				case USER:
					if (rem > size_in_pg) {
						rem -= size_in_pg;
						break;
					}
					ASSERT (rem == size);
					break;
				default:
					NOT_REACHED ();
			}
		}
	}

	// generate the user pool
	init_pool(&user_pool, &free_start, region_start, end);

	// Iterate over the e820_entry. Setup the usable.
	uint64_t usable_bound = (uint64_t) free_start;
	struct pool *pool;
	void *pool_end;
	size_t page_idx, page_cnt;

	for (i = 0; i < mb_info->mmap_len / sizeof (struct e820_entry); i++) {
		struct e820_entry *entry = &entries[i];
		if (entry->type == ACPI_RECLAIMABLE || entry->type == USABLE) {
			uint64_t start = (uint64_t)
				ptov (APPEND_HILO (entry->mem_hi, entry->mem_lo));
			uint64_t size = APPEND_HILO (entry->len_hi, entry->len_lo);
			uint64_t end = start + size;

			// TODO: add 0x1000 ~ 0x200000, This is not a matter for now.
			// All the pages are unuable
			if (end < usable_bound)
				continue;

			start = (uint64_t)
				pg_round_up (start >= usable_bound ? start : usable_bound);
split:
			if (page_from_pool (&kernel_pool, (void *) start))
				pool = &kernel_pool;
			else if (page_from_pool (&user_pool, (void *) start))
				pool = &user_pool;
			else
				NOT_REACHED ();

			pool_end = pool->base + bitmap_size (pool->used_map) * PGSIZE;
			page_idx = pg_no (start) - pg_no (pool->base);
			if ((uint64_t) pool_end < end) {
				page_cnt = ((uint64_t) pool_end - start) / PGSIZE;
				bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
				start = (uint64_t) pool_end;
				goto split;
			} else {
				page_cnt = ((uint64_t) end - start) / PGSIZE;
				bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
			}
		}
	}
}

/* Initializes the page allocator and get the memory size */
uint64_t
palloc_init (void) {
  /* End of the kernel as recorded by the linker.
     See kernel.lds.S. */
	extern char _end;
	struct area base_mem = { .size = 0 };
	struct area ext_mem = { .size = 0 };

	resolve_area_info (&base_mem, &ext_mem);
	printf ("Pintos booting with: \n");
	printf ("\tbase_mem: 0x%llx ~ 0x%llx (Usable: %'llu kB)\n",
		  base_mem.start, base_mem.end, base_mem.size / 1024);
	printf ("\text_mem: 0x%llx ~ 0x%llx (Usable: %'llu kB)\n",
		  ext_mem.start, ext_mem.end, ext_mem.size / 1024);
	populate_pools (&base_mem, &ext_mem);
	return ext_mem.end;
}

/* Obtains and returns a group of PAGE_CNT contiguous free pages.
   If PAL_USER is set, the pages are obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the pages are filled with zeros.  If too few pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
void *
palloc_get_multiple (enum palloc_flags flags, size_t page_cnt) {
	/* 연속적인 가용 페이지를 얻고, PAGE_CNT의 그룹을 반환
			* flags = PAL_USER
				: 사용자 공간에서 페이지 가져오기
					* PAL_USER가 설정되지 않으면 커널 공간에서 페이지 가져오기
			* flags = PAL_ZERO
				: 페이지를 0으로 채우기
			* flags = PAL_ASSERT
				: 사용 가능한 페이지가 너무 적으면 kernel panics 발생
					* PAL ASSERT가 설정되지 않은 경우에는 null pointer 반환
	*/

	struct pool *pool = flags & PAL_USER ? &user_pool : &kernel_pool;										/* flags에 따라 사용자 공간을 사용할지, 커널 공간을 사용할지 결정. *flag가 0이면 커널 공간 사용 */

	/* interrupt disable 처리를 해줘야 할까? 에 대한 chatgpt의 답변
			* 커널 스레드가 동작하는 critical section에서는 interrupt disable을 하여 다른 스레드나 interrupt handler가 자원에 접근하지 않도록 막을 필요가 있다.
				* 문제가 발생할 수 있는 상황
					* 커널 스레드 A가 lock_acquire()로 락을 획득하려고 하는 중에, 인터럽트가 발생하고 그 인터럽트가 다른 커널 스레드 B를 실행 시킨다. 
					* 만약 스레드 B가 동일한 락을 요청하려고 하면, 스레드 A와 B 사이에 교착 상태가 발생할 수 있다.
				* chatgpt의 첨언: lock_aqcuire에서 ASSERT문으로 인터럽트 맥락이 아닌지 확인하고 있다면 추가로 interrupt disable 처리를 하지 않아도 될 듯 (동시성 문제는 발생하지 않음)
			* 사용자 스레드에서 lock_acquire를 사용하는 경우, 일반적으로 interrupt를 수동으로 비활성화하지 않아도 된다. 내부적으로 스핀락이나 세마포어 같은 동기화 기법을 사용할 수 있다.
				* 지인의 첨언: lock_acuire에서 sema_init을 사용하고 있으므로 사용자 스레드에서는 lock 전에 interrupt disable 처리할 필요 없을 듯!!
	 */
	lock_acquire (&pool->lock);																													/* 해당 풀 구조체 접근에 대한 lock 사용 */
	size_t page_idx = bitmap_scan_and_flip (pool->used_map, 0, page_cnt, false);				/* 풀에서 사용 가능한 페이지를 조사하고 page_idx를 구함 */
	lock_release (&pool->lock);																													/* 해당 풀 구조체 접근에 대한 lock 반환 */
	void *pages;

	if (page_idx != BITMAP_ERROR)
		pages = pool->base + PGSIZE * page_idx;																						/* pool의 base부터 필요한 page_cnt만큼 더한 결과 나온 주소 */
	else
		pages = NULL;																																			/* 페이지가 부족하면 null pointer 반환 *flag가 0이면 NULL 포인터 반환 */

	if (pages) {
		/* 예외 처리: pages가 NULL일 경우에는 memset을 하지 않음 */
		if (flags & PAL_ZERO)
			memset (pages, 0, PGSIZE * page_cnt);																						/* PAL_ZERO가 설정되어 있을 경우 페이지를 0으로 채움 */
	} 
	else {
		/* PAL_ASSERT가 설정된 경우 NULL 포인터 대신 커널 패닉을 발생시킴 */
		if (flags & PAL_ASSERT)
			PANIC ("palloc_get: out of pages");																							/* PAL_ASSERT가 설정되어 있을 경우: 페이지가 부족하면 커널 패닉 발생시킴 */
	}

	return pages;
}

/* Obtains a single free page and returns its kernel virtual
   address.
   If PAL_USER is set, the page is obtained from the user pool,
   otherwise from the kernel pool.  If PAL_ZERO is set in FLAGS,
   then the page is filled with zeros.  If no pages are
   available, returns a null pointer, unless PAL_ASSERT is set in
   FLAGS, in which case the kernel panics. */
void *
palloc_get_page (enum palloc_flags flags) {
	return palloc_get_multiple (flags, 1);																							/* 하나의 페이지만 할당받음 */
}

/* Frees the PAGE_CNT pages starting at PAGES. */
void
palloc_free_multiple (void *pages, size_t page_cnt) {
	struct pool *pool;
	size_t page_idx;

	ASSERT (pg_ofs (pages) == 0);
	if (pages == NULL || page_cnt == 0)
		return;

	if (page_from_pool (&kernel_pool, pages))
		pool = &kernel_pool;
	else if (page_from_pool (&user_pool, pages))
		pool = &user_pool;
	else
		NOT_REACHED ();

	page_idx = pg_no (pages) - pg_no (pool->base);

#ifndef NDEBUG
	memset (pages, 0xcc, PGSIZE * page_cnt);
#endif
	ASSERT (bitmap_all (pool->used_map, page_idx, page_cnt));
	bitmap_set_multiple (pool->used_map, page_idx, page_cnt, false);
}

/* Frees the page at PAGE. */
void
palloc_free_page (void *page) {
	palloc_free_multiple (page, 1);
}

/* Initializes pool P as starting at START and ending at END */
static void
init_pool (struct pool *p, void **bm_base, uint64_t start, uint64_t end) {
  /* We'll put the pool's used_map at its base.
     Calculate the space needed for the bitmap
     and subtract it from the pool's size. */
	uint64_t pgcnt = (end - start) / PGSIZE;
	size_t bm_pages = DIV_ROUND_UP (bitmap_buf_size (pgcnt), PGSIZE) * PGSIZE;

	lock_init(&p->lock);
	p->used_map = bitmap_create_in_buf (pgcnt, *bm_base, bm_pages);
	p->base = (void *) start;

	// Mark all to unusable.
	bitmap_set_all(p->used_map, true);

	*bm_base += bm_pages;
}

/* Returns true if PAGE was allocated from POOL,
   false otherwise. */
static bool
page_from_pool (const struct pool *pool, void *page) {
	size_t page_no = pg_no (page);
	size_t start_page = pg_no (pool->base);
	size_t end_page = start_page + bitmap_size (pool->used_map);
	return page_no >= start_page && page_no < end_page;
}
