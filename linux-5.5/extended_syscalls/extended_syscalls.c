#include <linux/swap_stats.h>
#include <linux/syscalls.h>
#include <linux/printk.h>

SYSCALL_DEFINE0(reset_swap_stats)
{
	reset_adc_swap_stats();
	return 0;
}

SYSCALL_DEFINE3(get_swap_stats, int __user *, ondemand_swapin_num, int __user *, prefetch_swapin_num, int __user *,
		hit_on_prefetch_num)
{
	put_user(get_adc_profile_counter(ADC_ONDEMAND_SWAPIN), ondemand_swapin_num);
	put_user(get_adc_profile_counter(ADC_PREFETCH_SWAPIN), prefetch_swapin_num);
	put_user(get_adc_profile_counter(ADC_HIT_ON_PREFETCH), hit_on_prefetch_num);

	report_adc_time_stats();
	return 0;
}

SYSCALL_DEFINE1(set_async_prefetch, int, enable)
{
	__set_async_prefetch(enable);

	printk("Current prefetch swap-in mode: %s\n", enable ? "async" : "sync");
	return 0;
}

SYSCALL_DEFINE1(set_bypass_swap_cache, int, bypass)
{
	__set_bypass_swap_cache(bypass);

	printk("Current swap cache mode: %s\n", bypass ? "bypass" : "non-bypass");
	return 0;
}

SYSCALL_DEFINE1(set_readahead_win, int, win)
{
	__set_readahead_win(win);

	if (win == 0) {
		printk("Using kernel default prefetch window strategy.\n");
	} else if (win == 1) {
		printk("Disable kernel prefetch.\n");
	} else {
		printk("Fixed kernel readahead window: %d pages\n", win);
	}
	return 0;
}

SYSCALL_DEFINE1(set_customized_prefetch, int, customized_prefetch)
{
	set_customized_prefetch(customized_prefetch);
	printk("Current prefetch strategy: %s based.\n", customized_prefetch ? "Leap" : "VMA");
	return 0;
}

SYSCALL_DEFINE2(set_swapcache_mode, int, mode, int, capacity)
{
	if (set_swapcache_mode(mode, capacity)) {
		printk("swapcache mode: %s, capacity: %d pages\n",
		       mode == 0 ? "no size limit" :
		       mode == 1 ? "per-process isolation" :
				   "global size limit",
		       capacity);
	}

	return 0;
}

SYSCALL_DEFINE1(set_swap_bw_control, int, enable)
{
	if (set_swap_bw_control) {
		set_swap_bw_control(enable);
		printk("Current swap BW status: %s\n", enable ? "under control" : "uncontrolled");
		return 0;
	} else {
		printk("rswap is not registered yet!");
		return 1;
	}
}

SYSCALL_DEFINE2(get_all_procs_swap_pkts, int __user *, num_procs, char __user *, buf)
{
	if (get_all_procs_swap_pkts) {
		get_all_procs_swap_pkts(num_procs, buf);
		return 0;
	} else {
		printk("rswap is not registered yet!");
		return 1;
	}
}

SYSCALL_DEFINE5(syscall_scheduler_set_policy, int __user *, scheduler_cores, int __user *, info,
				 int __user *, scheduler_policy_boundary, int, check_duration, int, poll_times)
{
	printk("Please make sure all applications have stoped\n");
	if(syscall_scheduler_set_policy == NULL) {
		printk("Error: scheduler system unloaded\n");
		return 1;
	}
	syscall_scheduler_set_policy(scheduler_cores, info, scheduler_policy_boundary, check_duration, poll_times);
	return 0;
}

SYSCALL_DEFINE6(syscall_rswap_set_proc, void __user *, info, char __user *, names, int __user *, cores,
			   int __user *, num_threads, int __user *, weights, int __user *, lat_critical)
{
	printk("Please make sure all applications have stoped\n");
	if(syscall_rswap_set_proc == NULL) {
		printk("Error: scheduler system unloaded\n");
		return 1;
	}
	syscall_rswap_set_proc(info, names, cores, num_threads, weights, lat_critical);
	return 0;
}

SYSCALL_DEFINE1(set_slotcache_cpumask, int __user *, mask_usr)
{
	int i;
	int mask[ADC_MAX_NUM_CORES];
	copy_from_user(mask, mask_usr, num_online_cpus() * sizeof(int));
	set_slotcache_cpumask(mask);
	printk("swap slot cache cpumask set to: ");
	for (i = 0; i < num_online_cpus(); i++)
		if (slotcache_cpumask[i])
			printk("%d ", i);
	printk("\n");
	return 0;
}

SYSCALL_DEFINE1(set_swap_isolated, int, enable)
{
	set_swap_isolated(enable);
	printk("swap partition isolation: %s\n", enable ? "enabled" : "disabled");
	return 0;
}