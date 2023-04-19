/**
 *  [Canvas] swap_stats.h - collect swap stats for profiling
 */
#ifndef _LINUX_SWAP_STATS_H
#define _LINUX_SWAP_STATS_H

#include <linux/swap.h>
#include <linux/atomic.h>

#include <linux/swap_global_struct_mem_layer.h>

/* swap isolation */
extern bool __swap_isolated;

static inline bool swap_isolated(void)
{
	return __swap_isolated;
}

static inline void set_swap_isolated(bool value)
{
	__swap_isolated = value;
}

/* swap slot cache control */
extern int slotcache_cpumask[];

static inline void set_slotcache_cpumask(int *mask)
{
	int i;
	for (i = 0; i < num_online_cpus(); i++)
		slotcache_cpumask[i] = mask[i];
}

extern swp_entry_t *__canvas_page_entries;

static inline bool reserve_swp_entry_enabled(void)
{
	return slotcache_cpumask[smp_processor_id()];
}

static inline bool is_valid_swp_entry(swp_entry_t entry)
{
	return entry.val != -1UL;
}

static inline swp_entry_t invalid_swp_entry(void)
{
	swp_entry_t entry;
	entry.val = -1UL;
	return entry;
}

static inline swp_entry_t get_reserved_swp_entry(struct page *page)
{
	if (!__canvas_page_entries)
		return invalid_swp_entry();
	return __canvas_page_entries[page_to_pfn(page)];
}

static inline swp_entry_t alloc_reserved_swp_entry(struct page *page)
{
	swp_entry_t entry;
	if (!__canvas_page_entries)
		return invalid_swp_entry();
	entry = __canvas_page_entries[page_to_pfn(page)];
	__canvas_page_entries[page_to_pfn(page)] = invalid_swp_entry();
	return entry;
}

static inline void set_reserved_swp_entry(struct page *page, swp_entry_t entry)
{
	swp_entry_t old_entry;
	if (!__canvas_page_entries)
		return;
	old_entry = __canvas_page_entries[page_to_pfn(page)];
	__canvas_page_entries[page_to_pfn(page)] = entry;

	if (is_valid_swp_entry(old_entry) && is_valid_swp_entry(entry)) {
		pr_err("%s:%d rewrite reserved entry! page %p, old %lx, new %lx\n",
		       __func__, __LINE__, page, old_entry.val, entry.val);
	}
}

/* helper funcs; implemented in mm/swapfile.c. */
int free_swap_slot_try_lock(swp_entry_t entry);
void swap_reserve(struct page *page, swp_entry_t entry);
void free_reserved_swp_entry(struct page *page);

/* per-process swap prefetch utils */
extern bool customized_prefetch;

static inline void set_customized_prefetch(int _customized_prefetch)
{
	customized_prefetch = !!_customized_prefetch;
}
static inline bool customized_prefetch_enabled(void)
{
	return customized_prefetch;
}

static inline int trend_prev_idx(int idx)
{
	return (idx + SWAP_TREND_SIZE - 1) % SWAP_TREND_SIZE;
}

static inline int trend_next_idx(int idx)
{
	return (idx + 1) % SWAP_TREND_SIZE;
}

static inline void trend_inc_head(struct swap_trend *s_trend)
{
	int head = atomic_read(&s_trend->head);
	int size = atomic_read(&s_trend->size);
	atomic_set(&s_trend->head, trend_next_idx(head));
	if (size < SWAP_TREND_SIZE) {
		atomic_set(&s_trend->size, size + 1);
	}
}

void log_swap_trend(struct swap_trend *s_trend, unsigned long entry);

int find_trend_in_region(struct swap_trend *s_trend, int size,
			 long *major_delta, int *major_count);

int find_trend(struct swap_trend *s_trend, int *depth, long *major_delta,
	       int *major_count);

static inline int valid_trend(int has_trend, long delta)
{
#define TREND_THRESHOLD 16 // max stride = 16 * PAGE_SIZE = 64KB
	return has_trend && delta != 0 && delta < TREND_THRESHOLD &&
	       delta > -TREND_THRESHOLD;
#undef TREND_THRESHOLD
}

/* limit swap cache size. Ported from Leap. */
extern int swapcache_mode;
int set_swapcache_mode(int mode, int capacity);
static inline int get_swapcache_mode(void)
{
	return swapcache_mode;
}

/* [Canvas] shared swap cache */
extern struct swapcache_list global_sc_list;

static inline void inc_sc_list_head(struct swapcache_list *sc_list)
{
	sc_list->head = (sc_list->head + 1) % sc_list->capacity;
}

static inline int is_sc_list_full(struct swapcache_list *sc_list)
{
	return (sc_list->capacity <= sc_list->size);
}

void add_to_sc_list(struct swapcache_list *sc_list, swp_entry_t entry,
		    struct page *page);

/* async prefetch switch */
extern bool __bypass_swap_cache;
extern int __readahead_win;
extern bool enable_async_prefetch;

static inline bool bypass_swap_cache(void)
{
	return __bypass_swap_cache;
}

static inline void __set_bypass_swap_cache(int bypass_bit)
{
	__bypass_swap_cache = !!bypass_bit;
}

static inline bool disable_readahead(void)
{
	return __readahead_win == 1;
}

static inline bool fixed_readahead(void)
{
	return __readahead_win != 0;
}

static inline int fixed_readahead_win(void)
{
	return __readahead_win;
}

static inline void __set_readahead_win(int win)
{
	// win == 0: default kernel prefetch strategy
	// win == 1: disable prefetch (prefetch win == 1)
	// win >  1: fixed-len prefetch (1 demand page, (win-1) prefetch page)
	__readahead_win = win;
}

static inline bool async_prefetch_enabled(void)
{
	return enable_async_prefetch;
}

static inline void __set_async_prefetch(int async_bit)
{
	enable_async_prefetch = !!async_bit;
}

/* profile swap stats */
enum adc_counter_type {
	ADC_ONDEMAND_SWAPIN,
	ADC_PREFETCH_SWAPIN,
	ADC_HIT_ON_PREFETCH,
	NUM_ADC_COUNTER_TYPE
};

extern atomic_t adc_profile_counters[NUM_ADC_COUNTER_TYPE];

static inline void reset_adc_profile_counter(enum adc_counter_type type)
{
	atomic_set(&adc_profile_counters[type], 0);
}

static inline void adc_profile_counter_inc(enum adc_counter_type type)
{
	atomic_inc(&adc_profile_counters[type]);
}

static inline int get_adc_profile_counter(enum adc_counter_type type)
{
	return (int)atomic_read(&adc_profile_counters[type]);
}

// profile page fault latency
enum adc_profile_flag { ADC_PROFILE_SWAP_BIT = 1, ADC_PROFILE_MAJOR_BIT = 2 };
// profile accumulated time stats
struct adc_time_stat {
	atomic64_t accum_val;
	atomic_t cnt;
};

enum adc_time_stat_type {
	ADC_SWAP_MAJOR_LATENCY,
	ADC_SWAP_MINOR_LATENCY,
	ADC_NON_SWAP_LATENCY,
	ADC_RDMA_LATENCY,
	ADC_SWAPOUT_LATENCY,
	NUM_ADC_TIME_STAT_TYPE
};

extern struct adc_time_stat adc_time_stats[NUM_ADC_TIME_STAT_TYPE];

static inline void reset_adc_time_stat(enum adc_time_stat_type type)
{
	struct adc_time_stat *ts = &adc_time_stats[type];
	atomic64_set(&(ts->accum_val), 0);
	atomic_set(&(ts->cnt), 0);
}

static inline void accum_adc_time_stat(enum adc_time_stat_type type,
				       uint64_t val)
{
	struct adc_time_stat *ts = &adc_time_stats[type];
	atomic64_add(val, &(ts->accum_val));
	atomic_inc(&(ts->cnt));
}

static inline unsigned report_adc_time_stat(enum adc_time_stat_type type)
{
	struct adc_time_stat *ts = &adc_time_stats[type];
	if ((unsigned)atomic_read(&(ts->cnt)) == 0) {
		return 0;
	} else {
		return (int64_t)atomic64_read(&(ts->accum_val)) /
		       (int64_t)atomic_read(&(ts->cnt));
	}
}

void report_adc_time_stats(void);

static inline void reset_adc_swap_stats(void)
{
	int i;
	for (i = 0; i < NUM_ADC_COUNTER_TYPE; i++) {
		reset_adc_profile_counter(i);
	}

	for (i = 0; i < NUM_ADC_TIME_STAT_TYPE; i++) {
		reset_adc_time_stat(i);
	}
}

/* profile per-process RDMA bandwidth */
extern void (*set_swap_bw_control)(int);
extern void (*get_all_procs_swap_pkts)(int *, char *);

extern void (*syscall_scheduler_set_policy)(int *, int *, int *, int, int);
extern void (*syscall_rswap_set_proc)(void *, char *, int *, int *, int *, int *);

#endif /* _LINUX_SWAP_STATS_H */
