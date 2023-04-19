// SPDX-License-Identifier: GPL-2.0
#include <linux/mm_types.h>
#include <linux/rbtree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/mman.h>

#include <linux/atomic.h>
#include <linux/user_namespace.h>
#include <asm/pgtable.h>
#include <asm/mmu.h>

/* [Canvas] */
#include <linux/slab.h>

#ifndef INIT_MM_CONTEXT
#define INIT_MM_CONTEXT(name)
#endif

/*
 * For dynamically allocated mm_structs, there is a dynamically sized cpumask
 * at the end of the structure, the size of which depends on the maximum CPU
 * number the system can see. That way we allocate only as much memory for
 * mm_cpumask() as needed for the hundreds, or thousands of processes that
 * a system typically runs.
 *
 * Since there is only one init_mm in the entire system, keep it simple
 * and size this cpu_bitmask to NR_CPUS.
 */
struct mm_struct init_mm = {
	.mm_rb		= RB_ROOT,
	.pgd		= swapper_pg_dir,
	.mm_users	= ATOMIC_INIT(2),
	.mm_count	= ATOMIC_INIT(1),
	.mmap_sem	= __RWSEM_INITIALIZER(init_mm.mmap_sem),
	.page_table_lock =  __SPIN_LOCK_UNLOCKED(init_mm.page_table_lock),
	.arg_lock	=  __SPIN_LOCK_UNLOCKED(init_mm.arg_lock),
	.mmlist		= LIST_HEAD_INIT(init_mm.mmlist),
	.user_ns	= &init_user_ns,
	.cpu_bitmap	= CPU_BITS_NONE,
	INIT_MM_CONTEXT(init_mm)
};

/* [Canvas] */
#ifdef CONFIG_SWAP
void init_swapcache_list(struct swapcache_list *sc_list, int capacity)
{
	sc_list->head = 0;
	sc_list->size = 0;
	sc_list->capacity = capacity;
	sc_list->pages = (struct page **)kzalloc(
		sizeof(*sc_list->pages) * capacity, GFP_KERNEL);
	sc_list->entries = (unsigned long *)kzalloc(
		sizeof(unsigned long) * capacity, GFP_KERNEL);
	spin_lock_init(&sc_list->lock);
}

void destroy_swapcache_list(struct swapcache_list *sc_list)
{
	spin_lock_irq(&sc_list->lock);
	if (sc_list->pages)
		kfree(sc_list->pages);
	if (sc_list->entries)
		kfree(sc_list->entries);
	sc_list->head = 0;
	sc_list->size = 0;
	sc_list->capacity = 0;
	sc_list->pages = NULL;
	sc_list->entries = NULL;
	spin_unlock_irq(&sc_list->lock);
}
#endif
/* [Canvas] end */