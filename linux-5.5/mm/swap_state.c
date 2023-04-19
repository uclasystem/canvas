// SPDX-License-Identifier: GPL-2.0
/*
 *  linux/mm/swap_state.c
 *
 *  Copyright (C) 1991, 1992, 1993, 1994  Linus Torvalds
 *  Swap reorganised 29.12.95, Stephen Tweedie
 *
 *  Rewritten to use page cache, (C) 1998 Stephen Tweedie
 */
#include <linux/mm.h>
#include <linux/gfp.h>
#include <linux/kernel_stat.h>
#include <linux/swap.h>
#include <linux/swapops.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include <linux/backing-dev.h>
#include <linux/blkdev.h>
#include <linux/pagevec.h>
#include <linux/migrate.h>
#include <linux/vmalloc.h>
#include <linux/swap_slots.h>
#include <linux/huge_mm.h>

#include <asm/pgtable.h>

// [Canvas] for async prefetch.
#include <linux/frontswap.h>

// [Canvas] profile swap stats.
#include <linux/swap_stats.h>
#include <linux/page_idle.h>

// for uffd
#include <linux/swap_global_struct_mem_layer.h>

/*
 * swapper_space is a fiction, retained to simplify the path through
 * vmscan's shrink_page_list.
 */
static const struct address_space_operations swap_aops = {
	.writepage	= swap_writepage,
	.set_page_dirty	= swap_set_page_dirty,
#ifdef CONFIG_MIGRATION
	.migratepage	= migrate_page,
#endif
};

struct address_space *swapper_spaces[MAX_SWAPFILES] __read_mostly;
static unsigned int nr_swapper_spaces[MAX_SWAPFILES] __read_mostly;
static bool enable_vma_readahead __read_mostly = true;

#define SWAP_RA_WIN_SHIFT	(PAGE_SHIFT / 2)
#define SWAP_RA_HITS_MASK	((1UL << SWAP_RA_WIN_SHIFT) - 1)
#define SWAP_RA_HITS_MAX	SWAP_RA_HITS_MASK
#define SWAP_RA_WIN_MASK	(~PAGE_MASK & ~SWAP_RA_HITS_MASK)

#define SWAP_RA_HITS(v)		((v) & SWAP_RA_HITS_MASK)
#define SWAP_RA_WIN(v)		(((v) & SWAP_RA_WIN_MASK) >> SWAP_RA_WIN_SHIFT)
#define SWAP_RA_ADDR(v)		((v) & PAGE_MASK)

#define SWAP_RA_VAL(addr, win, hits)				\
	(((addr) & PAGE_MASK) |					\
	 (((win) << SWAP_RA_WIN_SHIFT) & SWAP_RA_WIN_MASK) |	\
	 ((hits) & SWAP_RA_HITS_MASK))

/* Initial readahead hits is 4 to start up with a small window */
#define GET_SWAP_RA_VAL(vma)					\
	(atomic_long_read(&(vma)->swap_readahead_info) ? : 4)

#define INC_CACHE_INFO(x)	do { swap_cache_info.x++; } while (0)
#define ADD_CACHE_INFO(x, nr)	do { swap_cache_info.x += (nr); } while (0)

static struct {
	unsigned long add_total;
	unsigned long del_total;
	unsigned long find_success;
	unsigned long find_total;
} swap_cache_info;

unsigned long total_swapcache_pages(void)
{
	unsigned int i, j, nr;
	unsigned long ret = 0;
	struct address_space *spaces;
	struct swap_info_struct *si;

	for (i = 0; i < MAX_SWAPFILES; i++) {
		swp_entry_t entry = swp_entry(i, 1);

		/* Avoid get_swap_device() to warn for bad swap entry */
		if (!swp_swap_info(entry))
			continue;
		/* Prevent swapoff to free swapper_spaces */
		si = get_swap_device(entry);
		if (!si)
			continue;
		nr = nr_swapper_spaces[i];
		spaces = swapper_spaces[i];
		for (j = 0; j < nr; j++)
			ret += spaces[j].nrpages;
		put_swap_device(si);
	}
	return ret;
}

static atomic_t swapin_readahead_hits = ATOMIC_INIT(4);

void show_swap_cache_info(void)
{
	printk("%lu pages in swap cache\n", total_swapcache_pages());
	printk("Swap cache stats: add %lu, delete %lu, find %lu/%lu\n",
		swap_cache_info.add_total, swap_cache_info.del_total,
		swap_cache_info.find_success, swap_cache_info.find_total);
	printk("Free swap  = %ldkB\n",
		get_nr_swap_pages() << (PAGE_SHIFT - 10));
	printk("Total swap = %lukB\n", total_swap_pages << (PAGE_SHIFT - 10));
}

/*
 * add_to_swap_cache resembles add_to_page_cache_locked on swapper_space,
 * but sets SwapCache flag and private instead of mapping and index.
 */
int add_to_swap_cache(struct page *page, swp_entry_t entry, gfp_t gfp)
{
	struct address_space *address_space = swap_address_space(entry);
	pgoff_t idx = swp_offset(entry);
	XA_STATE_ORDER(xas, &address_space->i_pages, idx, compound_order(page));
	unsigned long i, nr = compound_nr(page);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(PageSwapCache(page), page);
	VM_BUG_ON_PAGE(!PageSwapBacked(page), page);

	page_ref_add(page, nr);
	SetPageSwapCache(page);

	do {
		xas_lock_irq(&xas);
		xas_create_range(&xas);
		if (xas_error(&xas))
			goto unlock;
		for (i = 0; i < nr; i++) {
			VM_BUG_ON_PAGE(xas.xa_index != idx + i, page);
			set_page_private(page + i, entry.val + i);
			xas_store(&xas, page);
			xas_next(&xas);
		}
		address_space->nrpages += nr;
		__mod_node_page_state(page_pgdat(page), NR_FILE_PAGES, nr);
		ADD_CACHE_INFO(add_total, nr);
unlock:
		xas_unlock_irq(&xas);
	} while (xas_nomem(&xas, gfp));

	if (!xas_error(&xas))
		return 0;

	ClearPageSwapCache(page);
	page_ref_sub(page, nr);
	return xas_error(&xas);
}

/*
 * This must be called only on pages that have
 * been verified to be in the swap cache.
 */
void __delete_from_swap_cache(struct page *page, swp_entry_t entry)
{
	struct address_space *address_space = swap_address_space(entry);
	int i, nr = hpage_nr_pages(page);
	pgoff_t idx = swp_offset(entry);
	XA_STATE(xas, &address_space->i_pages, idx);

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageSwapCache(page), page);
	VM_BUG_ON_PAGE(PageWriteback(page), page);

	for (i = 0; i < nr; i++) {
		void *entry = xas_store(&xas, NULL);
		VM_BUG_ON_PAGE(entry != page, entry);
		set_page_private(page + i, 0);
		xas_next(&xas);
	}
	ClearPageSwapCache(page);
	address_space->nrpages -= nr;
	__mod_node_page_state(page_pgdat(page), NR_FILE_PAGES, -nr);
	ADD_CACHE_INFO(del_total, nr);
}

/**
 * add_to_swap - allocate swap space for a page
 * @page: page we want to move to swap
 *
 * Allocate swap space for the page and add the page to the
 * swap cache.  Caller needs to hold the page lock.
 */
int add_to_swap(struct page *page)
{
	swp_entry_t entry;
	int err;

	VM_BUG_ON_PAGE(!PageLocked(page), page);
	VM_BUG_ON_PAGE(!PageUptodate(page), page);

	entry = get_swap_page(page);
	if (!entry.val)
		return 0;

	/*
	 * XArray node allocations from PF_MEMALLOC contexts could
	 * completely exhaust the page allocator. __GFP_NOMEMALLOC
	 * stops emergency reserves from being allocated.
	 *
	 * TODO: this could cause a theoretical memory reclaim
	 * deadlock in the swap out path.
	 */
	/*
	 * Add it to the swap cache.
	 */
	err = add_to_swap_cache(page, entry,
			__GFP_HIGH|__GFP_NOMEMALLOC|__GFP_NOWARN);
	if (err)
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		goto fail;
	/*
	 * Normally the page will be dirtied in unmap because its pte should be
	 * dirty. A special case is MADV_FREE page. The page'e pte could have
	 * dirty bit cleared but the page's SwapBacked bit is still set because
	 * clearing the dirty bit and SwapBacked bit has no lock protected. For
	 * such page, unmap will not set dirty bit for it, so page reclaim will
	 * not write the page out. This can cause data corruption when the page
	 * is swap in later. Always setting the dirty bit for the page solves
	 * the problem.
	 */
	set_page_dirty(page);

	return 1;

fail:
	put_swap_page(page, entry);
	return 0;
}

/*
 * This must be called only on pages that have
 * been verified to be in the swap cache and locked.
 * It will never put the page into the free list,
 * the caller has a reference on the page.
 */
void delete_from_swap_cache(struct page *page)
{
	swp_entry_t entry = { .val = page_private(page) };
	struct address_space *address_space = swap_address_space(entry);

	xa_lock_irq(&address_space->i_pages);
	__delete_from_swap_cache(page, entry);
	xa_unlock_irq(&address_space->i_pages);

	put_swap_page(page, entry);
	page_ref_sub(page, hpage_nr_pages(page));
}

/*
 * If we are the only user, then try to free up the swap cache.
 *
 * Its ok to check for PageSwapCache without the page lock
 * here because we are going to recheck again inside
 * try_to_free_swap() _with_ the lock.
 * 					- Marcelo
 */
static inline void free_swap_cache(struct page *page)
{
	if (PageSwapCache(page) && !page_mapped(page) && trylock_page(page)) {
		try_to_free_swap(page);
		unlock_page(page);
	}
}

/*
 * Perform a free_page(), also freeing any swap cache associated with
 * this page if it is the last user of the page.
 */
void free_page_and_swap_cache(struct page *page)
{
	free_swap_cache(page);
	if (!is_huge_zero_page(page))
		put_page(page);
}

/*
 * Passed an array of pages, drop them all from swapcache and then release
 * them.  They are removed from the LRU and freed if this is their last use.
 */
void free_pages_and_swap_cache(struct page **pages, int nr)
{
	struct page **pagep = pages;
	int i;

	lru_add_drain();
	for (i = 0; i < nr; i++)
		free_swap_cache(pagep[i]);
	release_pages(pagep, nr);
}

static inline bool swap_use_vma_readahead(void)
{
	// [Canvas] ignore nr_rotate_swap to force vma readahead control
	return READ_ONCE(enable_vma_readahead);
	// return READ_ONCE(enable_vma_readahead) && !atomic_read(&nr_rotate_swap);
}

/*
 * Lookup a swap entry in the swap cache. A found page will be returned
 * unlocked and with its refcount incremented - we rely on the kernel
 * lock getting page table operations atomic even if we drop the page
 * lock before returning.
 */
struct page *lookup_swap_cache(swp_entry_t entry, struct vm_area_struct *vma,
			       unsigned long addr)
{
	struct page *page;
	struct swap_info_struct *si;

	si = get_swap_device(entry);
	if (!si)
		return NULL;
	page = find_get_page(swap_address_space(entry), swp_offset(entry));
	put_swap_device(si);

	INC_CACHE_INFO(find_total);
	if (page) {
		bool vma_ra = swap_use_vma_readahead();
		bool readahead;

		INC_CACHE_INFO(find_success);
		/*
		 * At the moment, we don't support PG_readahead for anon THP
		 * so let's bail out rather than confusing the readahead stat.
		 */
		if (unlikely(PageTransCompound(page)))
			return page;

		readahead = TestClearPageReadahead(page);
		if (vma && vma_ra) {
			unsigned long ra_val;
			int win, hits;
			struct mm_struct *mm = vma->vm_mm;

			ra_val = GET_SWAP_RA_VAL(vma);
			win = SWAP_RA_WIN(ra_val);
			hits = SWAP_RA_HITS(ra_val);
			if (readahead)
				hits = min_t(int, hits + 1, SWAP_RA_HITS_MAX);
			atomic_long_set(&vma->swap_readahead_info,
					SWAP_RA_VAL(addr, win, hits));
			// [Canvas] per-process prefetch stats
			if (mm)
				log_swap_trend(&mm->swap_fault_trend,
					       addr >> PAGE_SHIFT);
		}

		if (readahead) {
			count_vm_event(SWAP_RA_HIT);
			if (!vma || !vma_ra)
				atomic_inc(&swapin_readahead_hits);
		}
	}

	return page;
}

struct page *__read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
			struct vm_area_struct *vma, unsigned long addr,
			bool *new_page_allocated)
{
	struct page *found_page = NULL, *new_page = NULL;
	struct swap_info_struct *si;
	int err;
	*new_page_allocated = false;

	do {
		/*
		 * First check the swap cache.  Since this is normally
		 * called after lookup_swap_cache() failed, re-calling
		 * that would confuse statistics.
		 */
		si = get_swap_device(entry);
		if (!si)
			break;
		found_page = find_get_page(swap_address_space(entry),
					   swp_offset(entry));
		put_swap_device(si);
		if (found_page)
			break;

		/*
		 * Just skip read ahead for unused swap slot.
		 * During swap_off when swap_slot_cache is disabled,
		 * we have to handle the race between putting
		 * swap entry in swap cache and marking swap slot
		 * as SWAP_HAS_CACHE.  That's done in later part of code or
		 * else swap_off will be aborted if we return NULL.
		 */
		if (!__swp_swapcount(entry) && swap_slot_cache_enabled)
			break;

		/*
		 * Get a new page to read into from swap.
		 */
		if (!new_page) {
			new_page = alloc_page_vma(gfp_mask, vma, addr);
			if (!new_page)
				break;		/* Out of memory */
		}

		/*
		 * Swap entry may have been freed since our caller observed it.
		 */
		err = swapcache_prepare(entry);
		if (err == -EEXIST) {
			/*
			 * We might race against get_swap_page() and stumble
			 * across a SWAP_HAS_CACHE swap_map entry whose page
			 * has not been brought into the swapcache yet.
			 */
			cond_resched();
			continue;
		} else if (err)		/* swp entry is obsolete ? */
			break;

		/* May fail (-ENOMEM) if XArray node allocation failed. */
		__SetPageLocked(new_page);
		__SetPageSwapBacked(new_page);
		err = add_to_swap_cache(new_page, entry, gfp_mask & GFP_KERNEL);
		if (likely(!err)) {
			/* Initiate read into locked page */
			SetPageWorkingset(new_page);
			lru_cache_add_anon(new_page);
			*new_page_allocated = true;
			return new_page;
		}
		__ClearPageLocked(new_page);
		/*
		 * add_to_swap_cache() doesn't return -EEXIST, so we can safely
		 * clear SWAP_HAS_CACHE flag.
		 */
		put_swap_page(new_page, entry);
	} while (err != -ENOMEM);

	if (new_page)
		put_page(new_page);
	return found_page;
}

/*
 * Locate a page of swap in physical memory, reserving swap cache space
 * and reading the disk if it is not already cached.
 * A failure return means that either the page allocation failed or that
 * the swap entry is no longer in use.
 */
struct page *read_swap_cache_async(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_area_struct *vma, unsigned long addr, bool do_poll)
{
	bool page_was_allocated;
	struct page *retpage = __read_swap_cache_async(entry, gfp_mask,
			vma, addr, &page_was_allocated);

	if (page_was_allocated) {
		int cpu = get_cpu();
		swap_readpage(retpage, do_poll);
		frontswap_poll_load(cpu);
		put_cpu();
	}

	return retpage;
}

static unsigned int __swapin_nr_pages(unsigned long prev_offset,
				      unsigned long offset,
				      int hits,
				      int max_pages,
				      int prev_win)
{
	unsigned int pages, last_ra;

	// [Canvas] disable kernel prefetch
	// disable readahead is essentially to fix readahead win to 1
	if (fixed_readahead())
		return fixed_readahead_win();

	/*
	 * This heuristic has been found to work well on both sequential and
	 * random loads, swapping to hard disk or to SSD: please don't ask
	 * what the "+ 2" means, it just happens to work well, that's all.
	 */
	pages = hits + 2;
	if (pages == 2) {
		/*
		 * We can have no readahead hits to judge by: but must not get
		 * stuck here forever, so check for an adjacent offset instead
		 * (and don't even bother to check whether swap type is same).
		 */
		if (offset != prev_offset + 1 && offset != prev_offset - 1)
			pages = 1;
	} else {
		unsigned int roundup = 4;
		while (roundup < pages)
			roundup <<= 1;
		pages = roundup;
	}

	if (pages > max_pages)
		pages = max_pages;

	/* Don't shrink readahead too fast */
	last_ra = prev_win / 2;
	if (pages < last_ra)
		pages = last_ra;

	return pages;
}

static unsigned long swapin_nr_pages(unsigned long offset)
{
	static unsigned long prev_offset;
	unsigned int hits, pages, max_pages;
	static atomic_t last_readahead_pages;

	max_pages = 1 << READ_ONCE(page_cluster);
	if (max_pages <= 1)
		return 1;

	hits = atomic_xchg(&swapin_readahead_hits, 0);
	pages = __swapin_nr_pages(prev_offset, offset, hits, max_pages,
				  atomic_read(&last_readahead_pages));
	if (!hits)
		prev_offset = offset;
	atomic_set(&last_readahead_pages, pages);

	return pages;
}

/**
 * swap_cluster_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read an aligned block of
 * (1 << page_cluster) entries in the swap area. This method is chosen
 * because it doesn't cost us any seek time.  We also make sure to queue
 * the 'original' request together with the readahead ones...
 *
 * This has been extended to use the NUMA policies from the mm triggering
 * the readahead.
 *
 * Caller must hold read mmap_sem if vmf->vma is not NULL.
 */
struct page *swap_cluster_readahead_profiling(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_fault *vmf, int *swap_major); // [Canvas]
struct page *swap_cluster_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf)
{
	int swap_major; // [Canvas] foo variable
	return swap_cluster_readahead_profiling(entry, gfp_mask, vmf,
						&swap_major);
}
/* [Canvas] when do cluster prefetch, prefetched pages may not belong to current
 * process. We simply count them to current cgroup for now. The best solution
 * for now is not using cluster readahead and doing vma readahead instead.
 */
struct page *swap_cluster_readahead_profiling(swp_entry_t entry, gfp_t gfp_mask,
		struct vm_fault *vmf, int *swap_major)
{
	struct page *page;
	struct page *fault_page;
	unsigned long entry_offset = swp_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = swp_swap_info(entry);
	// struct blk_plug plug;
	bool do_poll = true, page_allocated;
	bool demand_page_allocated;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address;
	int cpu;
	// [Canvas] profile rdma latency
	uint64_t pf_ts_stt, pf_ts_end;

	mask = swapin_nr_pages(offset) - 1;

	// [Canvas] always do demand swap-in first
	fault_page = __read_swap_cache_async(entry, gfp_mask,
			vma, addr, &demand_page_allocated);

	/* [Canvas] Note that the process might be rescheduled after get_cpu!
	 * This happens even after disabling preemptions.
	 * Inside __read_swap_cache_async() and alloc_page_vma() there is
	 * cond_resched() which might do re-scheduling regardless the state
	 * of preemption. So be careful here as swap_readpage and
	 * frontswap_poll_load need to work on the same CPU!
	 */
	cpu = get_cpu();
	pf_ts_stt = get_cycles_start();
	if (demand_page_allocated) {
		swap_readpage(fault_page, do_poll);
		*swap_major |= ADC_PROFILE_MAJOR_BIT;
		adc_profile_counter_inc(ADC_ONDEMAND_SWAPIN);
		if (vma->vm_mm)
			count_memcg_event_mm(vma->vm_mm, ONDEMAND_SWAPIN);
	}
	// need uffd prefetch to bring in more pages.
	// pattern_prefetch_only = !(mask < TRIGGER_UFFD_PREFETCH_THRESHOLD);
	if (!mask)
		goto skip;

	/* Test swap type to make sure the dereference is safe */
	if (likely(si->flags & (SWP_BLKDEV | SWP_FS))) {
		struct inode *inode = si->swap_file->f_mapping->host;
		if (inode_read_congested(inode))
			goto skip;
	}

	do_poll = false;
	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset)	/* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	// blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset; offset++) {
		/* Ok, do the async read-ahead now */
		swp_entry_t offset_entry = swp_entry(swp_type(entry), offset);
		if (offset == entry_offset) // skip demand swap-ins
			continue;
		if (!swp_swap_info(offset_entry))
			printk("[Canvas] %s entry %08lx bad file\n", __func__,
			       offset_entry.val);
		page = __read_swap_cache_async(
			offset_entry, gfp_mask, vma, addr,
			&page_allocated);
		if (!page)
			continue;
		if (page_allocated) { // must be prefetched swap-ins now
			if (get_swapcache_mode() != 0 && vma && vma->vm_mm) {
				/* offset_entry must be valid here,
				 * since page has been allocated.
				 * swapcache_mode == 0: no size limit
				 * 		  == 1: per-process limit
				 * 		  == 2: global limit
				 */
				struct swapcache_list *sc_list;
				if (get_swapcache_mode() == 1 &&
				    __swap_count(offset_entry) == 1)
					sc_list = &vma->vm_mm->sc_list;
				else
					sc_list = &global_sc_list;
				add_to_sc_list(sc_list, offset_entry, page);
			}
			if (async_prefetch_enabled()) {
				swap_readpage_async(page);
			} else {
				swap_readpage(page, false);
			}
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);

			set_page_prefetch(page);
			adc_profile_counter_inc(ADC_PREFETCH_SWAPIN);
			if (vma->vm_mm)
				count_memcg_event_mm(vma->vm_mm,
						     PREFETCH_SWAPIN);
		}
		put_page(page);
	}
	// blk_finish_plug(&plug);

	lru_add_drain();	/* Push any new pages onto the LRU now */
skip:
	pf_ts_end = get_cycles_end();
	if (demand_page_allocated)
		frontswap_poll_load(cpu);
	put_cpu();
	if (*swap_major & ADC_PROFILE_MAJOR_BIT) {
		accum_adc_time_stat(ADC_RDMA_LATENCY, pf_ts_end - pf_ts_stt);
	}
	return fault_page;
}

int init_swap_address_space(unsigned int type, unsigned long nr_pages)
{
	struct address_space *spaces, *space;
	unsigned int i, nr;

	nr = DIV_ROUND_UP(nr_pages, SWAP_ADDRESS_SPACE_PAGES);
	spaces = kvcalloc(nr, sizeof(struct address_space), GFP_KERNEL);
	if (!spaces)
		return -ENOMEM;
	for (i = 0; i < nr; i++) {
		space = spaces + i;
		xa_init_flags(&space->i_pages, XA_FLAGS_LOCK_IRQ);
		atomic_set(&space->i_mmap_writable, 0);
		space->a_ops = &swap_aops;
		/* swap cache doesn't use writeback related tags */
		mapping_set_no_writeback_tags(space);
	}
	nr_swapper_spaces[type] = nr;
	swapper_spaces[type] = spaces;

	return 0;
}

void exit_swap_address_space(unsigned int type)
{
	kvfree(swapper_spaces[type]);
	nr_swapper_spaces[type] = 0;
	swapper_spaces[type] = NULL;
}

static inline void swap_ra_clamp_pfn(struct vm_area_struct *vma,
				     unsigned long faddr,
				     unsigned long lpfn,
				     unsigned long rpfn,
				     unsigned long *start,
				     unsigned long *end)
{
	*start = max3(lpfn, PFN_DOWN(vma->vm_start),
		      PFN_DOWN(faddr & PMD_MASK));
	*end = min3(rpfn, PFN_DOWN(vma->vm_end),
		    PFN_DOWN((faddr & PMD_MASK) + PMD_SIZE));
}

static void swap_ra_info(struct vm_fault *vmf,
			struct vma_swap_readahead *ra_info)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	swp_entry_t entry;
	unsigned long faddr, pfn, fpfn;
	unsigned long start, end;
	pte_t *pte, *orig_pte;
	unsigned int max_win, hits, prev_win, win, left;
#ifndef CONFIG_64BIT
	pte_t *tpte;
#endif

	max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster),
			     SWAP_RA_ORDER_CEILING);
	if (max_win == 1) {
		ra_info->win = 1;
		return;
	}

	faddr = vmf->address;
	orig_pte = pte = pte_offset_map(vmf->pmd, faddr);
	entry = pte_to_swp_entry(*pte);
	if ((unlikely(non_swap_entry(entry)))) {
		pte_unmap(orig_pte);
		return;
	}

	fpfn = PFN_DOWN(faddr);
	ra_val = GET_SWAP_RA_VAL(vma);
	pfn = PFN_DOWN(SWAP_RA_ADDR(ra_val));
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	ra_info->win = win = __swapin_nr_pages(pfn, fpfn, hits,
					       max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info,
			SWAP_RA_VAL(faddr, win, 0));

	if (win == 1) {
		pte_unmap(orig_pte);
		return;
	}

	/* Copy the PTEs because the page table may be unmapped */
	if (fpfn == pfn + 1)
		swap_ra_clamp_pfn(vma, faddr, fpfn, fpfn + win, &start, &end);
	else if (pfn == fpfn + 1)
		swap_ra_clamp_pfn(vma, faddr, fpfn - win + 1, fpfn + 1,
				  &start, &end);
	else {
		left = (win - 1) / 2;
		swap_ra_clamp_pfn(vma, faddr, fpfn - left, fpfn + win - left,
				  &start, &end);
	}
	ra_info->nr_pte = end - start;
	ra_info->offset = fpfn - start;
	pte -= ra_info->offset;
#ifdef CONFIG_64BIT
	ra_info->ptes = pte;
#else
	tpte = ra_info->ptes;
	for (pfn = start; pfn != end; pfn++)
		*tpte++ = *pte++;
#endif
	pte_unmap(orig_pte);
}

/* [Canvas] trend detection */
static inline void swap_ra_clamp_pfn_delta(struct vm_area_struct *vma,
					   unsigned long faddr,
					   unsigned long lpfn,
					   unsigned long rpfn,
					   unsigned long *start,
					   unsigned long *end,
					   long delta)
{
	unsigned long max_stt_pfn =
		max(PFN_DOWN(vma->vm_start), PFN_DOWN(faddr & PMD_MASK));
	unsigned long min_end_pfn = min(
		PFN_DOWN(vma->vm_end), PFN_DOWN((faddr & PMD_MASK) + PMD_SIZE));
	unsigned long abs_delta = (unsigned long)(delta > 0 ? delta : -delta);
	if (lpfn < max_stt_pfn)
		lpfn += ((max_stt_pfn - lpfn + abs_delta - 1) / abs_delta) *
			abs_delta;
	if (rpfn > min_end_pfn)
		rpfn -= ((rpfn - min_end_pfn + abs_delta - 1) / abs_delta) *
			abs_delta;

	*start = lpfn;
	*end = rpfn;
}

static void swap_ra_info_delta(struct vm_fault *vmf,
			       struct vma_swap_readahead *ra_info, long delta)
{
	struct vm_area_struct *vma = vmf->vma;
	unsigned long ra_val;
	swp_entry_t entry;
	unsigned long faddr, pfn, fpfn;
	unsigned long start, end;
	pte_t *pte, *orig_pte;
	unsigned int max_win, hits, prev_win, win;
#ifndef CONFIG_64BIT
	pte_t *tpte;
#endif

	max_win = 1 << min_t(unsigned int, READ_ONCE(page_cluster),
			     SWAP_RA_ORDER_CEILING);
	if (delta == 0 || max_win == 1) {
		ra_info->win = 1;
		return;
	}

	faddr = vmf->address;
	orig_pte = pte = pte_offset_map(vmf->pmd, faddr);
	entry = pte_to_swp_entry(*pte);
	if ((unlikely(non_swap_entry(entry)))) {
		pte_unmap(orig_pte);
		return;
	}

	fpfn = PFN_DOWN(faddr);
	ra_val = GET_SWAP_RA_VAL(vma);
	pfn = PFN_DOWN(SWAP_RA_ADDR(ra_val));
	prev_win = SWAP_RA_WIN(ra_val);
	hits = SWAP_RA_HITS(ra_val);
	ra_info->win = win = __swapin_nr_pages(pfn, fpfn, hits,
					       max_win, prev_win);
	atomic_long_set(&vma->swap_readahead_info,
			SWAP_RA_VAL(faddr, win, 0));
	if (win == 1) {
		pte_unmap(orig_pte);
		return;
	}

	/* Copy the PTEs because the page table may be unmapped */
	if (delta > 0) // sequential fault #1, increasing order
		swap_ra_clamp_pfn_delta(vma, faddr, fpfn, fpfn + win * delta,
					&start, &end, delta);
	else if (delta < 0) { // sequential fault #1, decreasing order
		delta = -delta;
		swap_ra_clamp_pfn_delta(vma, faddr, fpfn - (win - 1) * delta,
					fpfn + delta, &start, &end, delta);
	} else { // impossible to reach here
		start = fpfn;
		end = fpfn + delta;
	}
	// delta is always positive now
	ra_info->nr_pte = (end - start) / delta;
	ra_info->offset = (fpfn - start) / delta;
	pte -= ra_info->offset;
#ifdef CONFIG_64BIT
	ra_info->ptes = pte;
#else
	tpte = ra_info->ptes;
	for (pfn = start; pfn != end; pfn++)
		*tpte++ = *pte;
		pte += delta;
#endif
	pte_unmap(orig_pte);
}

/**
 * swap_vma_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * Primitive swap readahead code. We simply read in a few pages whoes
 * virtual addresses are around the fault address in the same vma.
 *
 * Caller must hold read mmap_sem if vmf->vma is not NULL.
 *
 */
static struct page *swap_vma_readahead_profiling(swp_entry_t fentry,
	gfp_t gfp_mask, struct vm_fault *vmf, int *swap_major); // [Canvas]
static struct page *swap_vma_readahead(swp_entry_t fentry, gfp_t gfp_mask,
				       struct vm_fault *vmf)
{
	int swap_major; // [Canvas] foo variable for profiling
	return swap_vma_readahead_profiling(fentry, gfp_mask, vmf,
					    &swap_major);
}
/* [Canvas] vma readahead prefetches pages only for current process and can be
 * easily and precisely charged.
 */
static struct page *swap_vma_readahead_profiling(swp_entry_t fentry,
		gfp_t gfp_mask, struct vm_fault *vmf, int *swap_major)
{
	// struct blk_plug plug;
	struct vm_area_struct *vma = vmf->vma;
	struct page *page;
	struct page *fault_page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i;
	bool page_allocated;
	bool demand_page_allocated;
	struct vma_swap_readahead ra_info = {
		0,
	};
	int cpu;
	// [Canvas] profile rdma latency
	uint64_t pf_ts_stt, pf_ts_end;

	long delta = 1;
	if (customized_prefetch_enabled() && vma && vma->vm_mm) {
		int depth, major_count;
		int has_trend = find_trend(&vma->vm_mm->swap_fault_trend, &depth,
					   &delta, &major_count);
		if (has_trend) { // valid pattern detected
			// printk("has trend, delta %ld, count %d, depth %d\n",
			//        delta, major_count, depth);
			swap_ra_info_delta(vmf, &ra_info, delta);
			delta = delta > 0 ? delta : -delta; // always positive
		} else {
			swap_ra_info(vmf, &ra_info);
			delta = 1;
		}
	} else {
		swap_ra_info(vmf, &ra_info);
		delta = 1;
	}
	BUG_ON(!(delta > 0));

	// [Canvas] always do demand swap-ins first
	fault_page = __read_swap_cache_async(
		fentry, gfp_mask, vma, vmf->address, &demand_page_allocated);

	/* [Canvas] Note that the process might be rescheduled after get_cpu!
	 * This happens even after disabling preemptions.
	 * Inside __read_swap_cache_async() and alloc_page_vma() there is
	 * cond_resched() which might do re-scheduling regardless the state
	 * of preemption. So be careful here as swap_readpage and
	 * frontswap_poll_load need to work on the same CPU!
	 */
	cpu = get_cpu(); // get_cpu disables preemption by itself
	pf_ts_stt = get_cycles_start();
	if (demand_page_allocated) {
		swap_readpage(fault_page, ra_info.win == 1);

		*swap_major |= ADC_PROFILE_MAJOR_BIT;
		adc_profile_counter_inc(ADC_ONDEMAND_SWAPIN);
		if (vma->vm_mm)
			count_memcg_event_mm(vma->vm_mm, ONDEMAND_SWAPIN);
	}

	// needs uffd prefetch to bring in more pages.
	// pattern_prefetch_only = !(ra_info.win < TRIGGER_UFFD_PREFETCH_THRESHOLD + 1);
	if (ra_info.win == 1)
		goto skip;

	// blk_start_plug(&plug);
#ifdef CONFIG_64BIT
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte; i++, pte += delta) {
#else
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte; i++, pte++) {
#endif
		if (i == ra_info.offset) // skip demand swap-ins
			continue;
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		if (!swp_swap_info(entry))
			printk("[Canvas] %s entry %08lx bad file\n", __func__,
			       entry.val);
		page = __read_swap_cache_async(entry, gfp_mask, vma,
					       vmf->address, &page_allocated);
		if (!page)
			continue;
		if (page_allocated) { // must be prefetched swap-ins now
			if (get_swapcache_mode() != 0 && vma && vma->vm_mm) {
				/* entry must be valid here,
					 * since page has been allocated.
					 * swapcache_mode == 0: no size limit
					 * 	== 1: per-process limit
					 * 	== 2: global limit
					 */
				struct swapcache_list *sc_list;
				if (get_swapcache_mode() == 1 &&
				    __swap_count(entry) == 1)
					sc_list = &vma->vm_mm->sc_list;
				else
					sc_list = &global_sc_list;
				add_to_sc_list(sc_list, entry, page);
			}
			if (async_prefetch_enabled()) {
				swap_readpage_async(page);
			} else {
				swap_readpage(page, false);
			}
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);

			set_page_prefetch(page);
			adc_profile_counter_inc(ADC_PREFETCH_SWAPIN);
			if (vma->vm_mm)
				count_memcg_event_mm(vma->vm_mm,
						     PREFETCH_SWAPIN);
		}
		put_page(page);
	}
	// blk_finish_plug(&plug);
	lru_add_drain();
skip:
	if (demand_page_allocated)
		frontswap_poll_load(cpu);
	put_cpu();
	pf_ts_end = get_cycles_end();
	if (*swap_major & ADC_PROFILE_MAJOR_BIT) {
		accum_adc_time_stat(ADC_RDMA_LATENCY, pf_ts_end - pf_ts_stt);
	}
	return fault_page;
}

/**
 * prefetch without reading on-demand page
 *
 * return value:
 * 	false : skip the kernel pattern based pretch.
 * 		Deliver the infor to user space via ufd.
 * 	true : do kernel pattern based prefetch.
 *
 */
static inline bool swap_vma_prefetch(swp_entry_t fentry, gfp_t gfp_mask,
				     struct vm_fault *vmf)
{
	// struct blk_plug plug;
	struct vm_area_struct *vma = vmf->vma;
	struct page *page;
	pte_t *pte, pentry;
	swp_entry_t entry;
	unsigned int i;
	bool page_allocated;
	bool pattern_prefetch_only = true;
	struct vma_swap_readahead ra_info = {
		0,
	};

	long delta = 1;
	if (customized_prefetch_enabled() && vma && vma->vm_mm) {
		int depth, major_count;
		int has_trend = find_trend(&vma->vm_mm->swap_fault_trend, &depth,
					   &delta, &major_count);
		if (has_trend) { // valid pattern detected
			// printk("has trend, delta %ld, count %d, depth %d\n",
			//        delta, major_count, depth);
			swap_ra_info_delta(vmf, &ra_info, delta);
			delta = delta > 0 ? delta : -delta;
		} else { // no pattern detected, default vma-prefetch
			swap_ra_info(vmf, &ra_info);
			delta = 1;
		}
	} else { // no vma or mm info, default kernel vma-prefetch
		swap_ra_info(vmf, &ra_info);
		delta = 1;
	}
	BUG_ON(!(delta > 0));

	// needs uffd prefetch to bring in more pages.
	pattern_prefetch_only = !(ra_info.win < TRIGGER_UFFD_PREFETCH_THRESHOLD + 1);

	if(ra_info.win == 1)
		goto out;

	// blk_start_plug(&plug);
#ifdef CONFIG_64BIT // 64 BIT, increment PTE by delta each time
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte; i++, pte += delta) {
#else // 32BIT, ptes have been prepared in the buffer
	for (i = 0, pte = ra_info.ptes; i < ra_info.nr_pte; i++, pte++) {
#endif
		if (i == ra_info.offset) { // skip on-demand swap-in page
			continue;
		}
		pentry = *pte;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;
		if (!swp_swap_info(entry))
			printk("[Canvas]: %s entry %08lx bad file, delta %ld, nr_pte %d, win %d\n", __func__,
			       entry.val, delta, ra_info.nr_pte, ra_info.win);
		page = __read_swap_cache_async(entry, gfp_mask, vma,
					       vmf->address, &page_allocated);
		if (!page)
			continue;
		if (page_allocated) {
			if (get_swapcache_mode() != 0 && vma && vma->vm_mm) {
				/* entry must be valid here,
				 * since page has been allocated.
				 * swapcache_mode == 0: no size limit
				 * 	== 1: per-process limit
				 * 	== 2: global limit
				 */
				struct swapcache_list *sc_list;
				if (get_swapcache_mode() == 1 &&
				    __swap_count(entry) == 1)
					sc_list = &vma->vm_mm->sc_list;
				else
					sc_list = &global_sc_list;
				add_to_sc_list(sc_list, entry, page);
			}
			if (async_prefetch_enabled()) {
				swap_readpage_async(page);
			} else {
				swap_readpage(page, false);
			}
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);

			set_page_prefetch(page);
			adc_profile_counter_inc(ADC_PREFETCH_SWAPIN);
			if (vma->vm_mm)
				count_memcg_event_mm(vma->vm_mm,
						     PREFETCH_SWAPIN);
		}
		put_page(page);
	}
	// blk_finish_plug(&plug);
	lru_add_drain();

out:
	return pattern_prefetch_only;
}

static inline bool swap_cluster_prefetch(swp_entry_t entry, gfp_t gfp_mask,
					 struct vm_fault *vmf)
{
	struct page *page;
	unsigned long entry_offset = swp_offset(entry);
	unsigned long offset = entry_offset;
	unsigned long start_offset, end_offset;
	unsigned long mask;
	struct swap_info_struct *si = swp_swap_info(entry);
	struct blk_plug plug;
	bool do_poll = true, page_allocated;
	struct vm_area_struct *vma = vmf->vma;
	unsigned long addr = vmf->address;
	bool pattern_prefetch_only = true;

	mask = swapin_nr_pages(offset) - 1; // number of pages to be prefetched

	// need uffd prefetch to bring in more pages.
	pattern_prefetch_only = !(mask < TRIGGER_UFFD_PREFETCH_THRESHOLD);
	if(!mask)
		goto out;

	/* Test swap type to make sure the dereference is safe */
	if (likely(si->flags & (SWP_BLKDEV | SWP_FS))) {
		struct inode *inode = si->swap_file->f_mapping->host;
		if (inode_read_congested(inode))
			return false;
	}

	do_poll = false;
	/* Read a page_cluster sized and aligned cluster around offset. */
	start_offset = offset & ~mask;
	end_offset = offset | mask;
	if (!start_offset) /* First page is swap header. */
		start_offset++;
	if (end_offset >= si->max)
		end_offset = si->max - 1;

	blk_start_plug(&plug);
	for (offset = start_offset; offset <= end_offset; offset++) {
		swp_entry_t offset_entry = swp_entry(swp_type(entry), offset);
		if (offset == entry_offset) // skip on-demand swap-ins
			continue;
		/* Ok, do the async read-ahead now */
		if (!swp_swap_info(offset_entry))
			printk("%s:%d entry %08lx bad file\n", __func__,
			       __LINE__, offset_entry.val);
		page = __read_swap_cache_async(
			offset_entry, gfp_mask, vma, addr,
			&page_allocated);
		if (!page)
			continue;
		if (page_allocated) {
			if (get_swapcache_mode() != 0 && vma && vma->vm_mm) {
				/* entry must be valid here,
				 * since page has been allocated.
				 * swapcache_mode == 0: no size limit
				 * 	== 1: per-process limit
				 * 	== 2: global limit
				 */
				struct swapcache_list *sc_list;
				if (get_swapcache_mode() == 1 &&
				    __swap_count(offset_entry) == 1)
					sc_list = &vma->vm_mm->sc_list;
				else
					sc_list = &global_sc_list;
				add_to_sc_list(sc_list, offset_entry, page);
			}
			if (async_prefetch_enabled()) {
				swap_readpage_async(page);
			} else {
				swap_readpage(page, false);
			}
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);

			set_page_prefetch(page);
			adc_profile_counter_inc(ADC_PREFETCH_SWAPIN);
			if (vma->vm_mm)
				count_memcg_event_mm(vma->vm_mm,
						     PREFETCH_SWAPIN);
		}
		put_page(page);
	}
	blk_finish_plug(&plug);
	lru_add_drain(); /* Push any new pages onto the LRU now */

out:
	return pattern_prefetch_only;
}

/**
 * [Canvas] Do prefetch based on the fault addr.
 *  Choose vma based prefetch or swap entry based prefetch.
 *
 * return value
 * 	true : do kernel pattern based prefetch only.
 * 	false : Trigger uffd user space based prefetch (With kernel pattern based prefetch).
 */
bool swap_prefetch_only(swp_entry_t entry, gfp_t gfp_mask,
			struct vm_fault *vmf)
{
	if (swap_use_vma_readahead()) { // check /sys/kernel/mm/swap/vma_ra_enabled
		return swap_vma_prefetch(entry, gfp_mask, vmf);
	} else {
		return swap_cluster_prefetch(entry, gfp_mask, vmf);
	}
}
/* [Canvas] profile functions end */

/**
 * swapin_readahead - swap in pages in hope we need them soon
 * @entry: swap entry of this memory
 * @gfp_mask: memory allocation flags
 * @vmf: fault information
 *
 * Returns the struct page for entry and addr, after queueing swapin.
 *
 * It's a main entry function for swap readahead. By the configuration,
 * it will read ahead blocks by cluster-based(ie, physical disk based)
 * or vma-based(ie, virtual address based on faulty address) readahead.
 */
struct page *swapin_readahead(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf)
{
	return swap_use_vma_readahead() ?
			swap_vma_readahead(entry, gfp_mask, vmf) :
			swap_cluster_readahead(entry, gfp_mask, vmf);
}

struct page *swapin_readahead_profiling(swp_entry_t entry, gfp_t gfp_mask,
				struct vm_fault *vmf, int *swap_major)
{
	return swap_use_vma_readahead() ?
			swap_vma_readahead_profiling(entry, gfp_mask, vmf, swap_major) :
			swap_cluster_readahead_profiling(entry, gfp_mask, vmf, swap_major);
}

#ifdef CONFIG_SYSFS
static ssize_t vma_ra_enabled_show(struct kobject *kobj,
				     struct kobj_attribute *attr, char *buf)
{
	return sprintf(buf, "%s\n", enable_vma_readahead ? "true" : "false");
}
static ssize_t vma_ra_enabled_store(struct kobject *kobj,
				      struct kobj_attribute *attr,
				      const char *buf, size_t count)
{
	if (!strncmp(buf, "true", 4) || !strncmp(buf, "1", 1))
		enable_vma_readahead = true;
	else if (!strncmp(buf, "false", 5) || !strncmp(buf, "0", 1))
		enable_vma_readahead = false;
	else
		return -EINVAL;

	return count;
}
static struct kobj_attribute vma_ra_enabled_attr =
	__ATTR(vma_ra_enabled, 0644, vma_ra_enabled_show,
	       vma_ra_enabled_store);

static struct attribute *swap_attrs[] = {
	&vma_ra_enabled_attr.attr,
	NULL,
};

static struct attribute_group swap_attr_group = {
	.attrs = swap_attrs,
};

static int __init swap_init_sysfs(void)
{
	int err;
	struct kobject *swap_kobj;

	swap_kobj = kobject_create_and_add("swap", mm_kobj);
	if (!swap_kobj) {
		pr_err("failed to create swap kobject\n");
		return -ENOMEM;
	}
	err = sysfs_create_group(swap_kobj, &swap_attr_group);
	if (err) {
		pr_err("failed to register swap group\n");
		goto delete_obj;
	}
	return 0;

delete_obj:
	kobject_put(swap_kobj);
	return err;
}
subsys_initcall(swap_init_sysfs);
#endif

/* [Canvas] */
/**
 * Do prefetch according the information passed down by user process.
 */
static void swap_vma_readahead_prefetch(gfp_t gfp_mask,
					struct vm_fault_prefetch *vmf_pref)
{
	struct vm_area_struct *vma = vmf_pref->vma;
	pte_t *ptep, pentry;
	struct page *page;
	swp_entry_t entry;
	unsigned int i;
	bool page_allocated;
	int cpu;

	cpu = get_cpu();

#ifdef UFFD_PREFETCH_LATENCY_PROFILING
	if (vmf_pref->fault_addr != 0) {
		uffd_prefetch_received(vmf_pref->fault_addr,
				       get_reference_cycles());
	}
#endif

	for (i = 0, ptep = vmf_pref->ptes; i < vmf_pref->prefetch_num;
	     i++, ptep++) {
		pentry = *ptep;
		if (pte_none(pentry))
			continue;
		if (pte_present(pentry))
			continue;
		entry = pte_to_swp_entry(pentry);
		if (unlikely(non_swap_entry(entry)))
			continue;

		page = __read_swap_cache_async(entry, gfp_mask, vma,
					       vmf_pref->start_address +
						       i * PAGE_SIZE,
					       &page_allocated);
		if (!page) // Should be Out-of-Memory error ? 1) There is no page in swap_cache; 2) can't allocate a page.
			continue;

		if (page_allocated) {
			if (async_prefetch_enabled()) {
				swap_readpage_async(page);
			} else {
				swap_readpage(page, false);
			}
			SetPageReadahead(page);
			count_vm_event(SWAP_RA);

			set_page_prefetch(page);
			adc_profile_counter_inc(ADC_PREFETCH_SWAPIN);
			if (vma->vm_mm)
				count_memcg_event_mm(vma->vm_mm,
						     PREFETCH_SWAPIN);
		}
		put_page(page);
	}
	put_cpu();
	lru_add_drain();
}

/**
 * Check the correctness of the recorded vma field.
 *
 */
static bool check_uffd_message_vma(struct vm_fault_prefetch *vmf_pref)
{
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned long prefetch_virt_addr = vmf_pref->start_address;
	bool ret = true;

	// Prepare the necessary fields of the vm_fault_prefetch
	mm = current->mm; // mm_struct of this thread
	vma = find_vma(mm, prefetch_virt_addr);
	if (unlikely(!vma)) {
		pr_err("%s, passed down wrong prfetch info. #1 NULL vma, skip \n",
		       __func__);
		ret = false;
		goto out;
	}
	if (unlikely((unsigned long)vmf_pref->vma != (unsigned long)vma)) {
		pr_err("%s, Calculated vma 0x%lx doesn't match the recorded vma 0x%lx \n",
		       __func__, (unsigned long)vma,
		       (unsigned long)vmf_pref->vma);
		ret = false;
		goto out;
	}
	if (likely(vma->vm_start <= prefetch_virt_addr)) {
		ret = true;
		goto out;
	}

out:
	return ret;
}

/**
 * Do prefetch by using the prefetch hints passed dwon from user space
 *
 * user space virtual address -> pte -> swan_entry_t
 * Physical page can be allocated and added into the swap cache during this procedure.
 *
 * Warning : we use virtual address based prefetch now.
 *
 * Parameters
 * 	struct vm_fault_prefetch.
 * 	This variable is built by the user space, address[], and this function.
 * 	It contains all the pages to be prefetched.
 *
 * 	[Warning] : all the pages within the vm_fault_prefetch->address[] should belone to one VMA.
 * 	Or we have to retrive the vma for each virtual address, which cause significant overhead in the critical path
 *
 * Return value
 * 	no need to return.
 * 	All the prefetched pages are added into swap_cache.
 *
 */
void swapin_readahead_prefetch(struct vm_fault_prefetch *vmf_prefetch)
{
	/*
	 * Ok, we have a good vm_area for this memory access, so
	 * we can handle it..
	 */
	if (false) // check VMA
		BUG_ON(!check_uffd_message_vma(vmf_prefetch));
	if (filling_prefetch_pte(vmf_prefetch) == -1)
		return;
	swap_vma_readahead_prefetch(GFP_HIGHUSER_MOVABLE, vmf_prefetch);
}
