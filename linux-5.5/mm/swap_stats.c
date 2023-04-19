#include <linux/swap_stats.h>
#include <linux/swap.h>
#include <linux/swapops.h>

// [Canvas]
#include <linux/swap_global_macro.h>

/* swap isolation */
bool __swap_isolated = true;
EXPORT_SYMBOL(__swap_isolated);

/* swap cache & prefetching */
bool __bypass_swap_cache = true;
int __readahead_win = 0;
bool enable_async_prefetch = true;

/* per-process swap prefetch */
bool customized_prefetch = true;

void log_swap_trend(struct swap_trend *s_trend, unsigned long pfn)
{
	struct swap_trend_entry se;
	if (atomic_read(&s_trend->size)) {
		long offset_delta;
		int prev_index;
		prev_index = trend_prev_idx(atomic_read(&s_trend->head));
		offset_delta = pfn - s_trend->history[prev_index].entry;
		se.delta = offset_delta;
		se.entry = pfn;
	} else {
		se.delta = 0;
		se.entry = pfn;
	}

	s_trend->history[atomic_read(&s_trend->head)] = se;
	trend_inc_head(s_trend);
}

int find_trend_in_region(struct swap_trend *s_trend, int size,
			 long *major_delta, int *major_count)
{
	int count, i, j;
	long candidate;
	int maj_index = trend_prev_idx(atomic_read(&s_trend->head));
	for (i = trend_prev_idx(maj_index), j = 1, count = 1; j < size;
	     i = trend_prev_idx(i), j++) {
		if (s_trend->history[maj_index].delta ==
		    s_trend->history[i].delta)
			count++;
		else
			count--;
		if (count == 0) {
			maj_index = i;
			count = 1;
		}
	}

	candidate = s_trend->history[maj_index].delta;
	for (i = trend_prev_idx(atomic_read(&s_trend->head)), j = 0, count = 0;
	     j < size; i = trend_prev_idx(i), j++) {
		if (s_trend->history[i].delta == candidate)
			count++;
	}

	//printk("majority index: %d, candidate: %ld, count:%d\n", maj_index, candidate, count);
	*major_delta = candidate;
	*major_count = count;
	return count > (size / 2);
}

int find_trend(struct swap_trend *s_trend, int *depth, long *major_delta,
	       int *major_count)
{
	int has_trend = 0;
	int size = SWAP_TREND_SIZE / 4;
	int max_size;

	max_size = size * 4;
	while (has_trend == 0 && size <= max_size) {
		has_trend = find_trend_in_region(s_trend, size, major_delta,
						 major_count);
		size *= 2;
	}
	*depth = size;

	return valid_trend(has_trend, *major_delta);
}

/* limit swap cache size */
int swapcache_mode = 0;

struct swapcache_list global_sc_list;

int set_swapcache_mode(int mode, int capacity)
{
	// mode:
	//	0: no isolation and size limit
	//	1: per-process isolation & size limit
	//	2: global swapcache size limit
	// capacity: #(pages) of global swap cache
	swapcache_mode = mode;
	if (mode < 0 || mode > 2 || capacity < 0) {
		printk("%s: wrong argument! mode %d, capacity %d pages",
		       __func__, mode, capacity);
		return 0;
	}
	if (mode != 0) {
		destroy_swapcache_list(&global_sc_list);
		init_swapcache_list(&global_sc_list, capacity);
	}
	return 1;
}
#undef PREFETCH_BUFFER_SIZE
EXPORT_SYMBOL(set_swapcache_mode);

/**
 * [Canvas] limit swap cache size
 */
void add_to_sc_list(struct swapcache_list *sc_list, swp_entry_t entry,
		    struct page *page)
{
	int head = 0;
	bool find = false;
	int trails = 0;

	if (!is_sc_list_full(sc_list)) {
		spin_lock_irq(&sc_list->lock);
		if (!is_sc_list_full(sc_list)) {
			head = sc_list->head;
			sc_list->pages[head] = page;
			sc_list->entries[head] = entry.val;
			inc_sc_list_head(sc_list);
			sc_list->size++;
		}
		spin_unlock_irq(&sc_list->lock);
		return;
	}
	while (!find && trails < sc_list->capacity) {
		struct page *victim_page = NULL;
		swp_entry_t victim_entry;

		spin_lock_irq(&sc_list->lock);
		head = sc_list->head;
		inc_sc_list_head(sc_list);
		victim_entry.val = sc_list->entries[head];
		victim_page = sc_list->pages[head];
		spin_unlock_irq(&sc_list->lock);

		// filter out invalid page or entry
		if (!victim_page || !PageSwapCache(victim_page) ||
		    non_swap_entry(victim_entry) ||
		    victim_entry.val != page_private(victim_page)) {
			find = true;
			break;
		}

		if (!page_mapped(victim_page)) {
			if (trylock_page(victim_page)) {
				struct page *comp_page;
				get_page(victim_page);
				test_clear_page_writeback(victim_page);
				comp_page = compound_head(victim_page);
				delete_from_swap_cache(comp_page);
				SetPageDirty(comp_page);
				find = true;
				// find = try_to_free_swap(victim_page);
				// if (!find)
				// 	pr_err("[Canvas] try_to_free_swap failed! page %p\n",
				// 	       victim_page);
				unlock_page(victim_page);
				put_page(victim_page);
			}
		} else if (page_mapcount(victim_page) == 1) {
			if (trylock_page(victim_page)) {
				get_page(victim_page);
				find = try_to_free_swap(victim_page);
				unlock_page(victim_page);
				put_page(victim_page);
			}
		} else { // never get into this branch in practice
			printk("trails: %d, page %p: mapcount %d, PageSwapCache %d\n",
			       trails, victim_page, page_mapcount(victim_page),
			       PageSwapCache(victim_page));
		}
		trails++;
	}
	sc_list->pages[head] = page;
	sc_list->entries[head] = entry.val;

	// if (!find) {
	// 	printk("%s: failed to free a swap cache\n", __func__);
	// }
}

/* profile swap stats */
atomic_t adc_profile_counters[NUM_ADC_COUNTER_TYPE];
struct adc_time_stat adc_time_stats[NUM_ADC_TIME_STAT_TYPE];

static const char *adc_time_stat_names[NUM_ADC_TIME_STAT_TYPE] = {
	"major swap duration", "minor swap duration", "non-swap   duration",
	"swap-out   duration", "RDMA write latency ",
};

void report_adc_time_stats(void)
{
	int type;
	for (type = 0; type < NUM_ADC_TIME_STAT_TYPE; type++) {
		struct adc_time_stat *ts = &adc_time_stats[type];
		if ((unsigned)atomic_read(&ts->cnt) == 0) {
			printk("%s: %dns, #: %d", adc_time_stat_names[type], 0,
			       0);
		} else {
			int64_t dur = (int64_t)atomic64_read(&ts->accum_val) /
				      (int64_t)atomic_read(&ts->cnt) * 1000 /
				      ADC_CPU_FREQ;
			printk("%s: %lldns, #: %lld", adc_time_stat_names[type],
			       dur, (int64_t)atomic_read(&ts->cnt));
		}
	}
}

/* for swap RDMA bandwidth control */
void (*set_swap_bw_control)(int) = NULL;
EXPORT_SYMBOL(set_swap_bw_control);
void (*get_all_procs_swap_pkts)(int *, char *) = NULL;
EXPORT_SYMBOL(get_all_procs_swap_pkts);

/* for swap RDMA scheduler control */
void (*syscall_scheduler_set_policy)(int *, int*, int *, int, int) = NULL;
EXPORT_SYMBOL(syscall_scheduler_set_policy);
void (*syscall_rswap_set_proc)(void *, char *, int *, int *, int *, int *) = NULL;
EXPORT_SYMBOL(syscall_rswap_set_proc);

/* swap slot reservation control */
int slotcache_cpumask[ADC_MAX_NUM_CORES] = { 0 };

/*
 * [Canvas] swap entry field per page struct
 */
swp_entry_t *__canvas_page_entries = NULL;
static int __init canvas_init_page_entries(void)
{
	int i;
	__canvas_page_entries =
		kvmalloc(sizeof(swp_entry_t) * (CANVAS_MAX_MEM / PAGE_SIZE),
			 GFP_KERNEL);
	for (i = 0; i < CANVAS_MAX_MEM / PAGE_SIZE; i++)
		__canvas_page_entries[i] = invalid_swp_entry();
	pr_err("canvas_page_entries init at 0x%lx\n",
	       (unsigned long)__canvas_page_entries);
	return 0;
}
early_initcall(canvas_init_page_entries);