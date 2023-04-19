#ifndef _LINUX_EXTENDED_SYSCALLS_H
#define _LINUX_EXTENDED_SYSCALLS_H

/* [Canvas] extended syscalls.
 * Syscalls for profiling.
 */
asmlinkage long sys_reset_swap_stats(void);
asmlinkage long sys_get_swap_stats(int __user *on_demand_swapin_num, int __user *prefetch_swapin_num,
				   int __user *hiton_swap_cache_num);
asmlinkage long sys_set_async_prefetch(int enable);

asmlinkage long sys_set_bypass_swap_cache(int bypass);
asmlinkage long sys_set_readahead_win(int win);

asmlinkage long sys_set_costomized_prefetch(int _customize_prefetch);

asmlinkage long sys_set_swapcache_mode(int mode, int capacity);
asmlinkage long sys_get_all_procs_swap_pkts(int __user *num_procs, char __user *buf);

asmlinkage long sys_syscall_scheduler_set_policy(int __user *scheduler_cores, int __user *info,
				 int __user *scheduler_policy_boundary, int check_duration, int poll_times);
asmlinkage long sys_syscall_rswap_set_proc(void __user *info, char __user *names, int __user *cores,
			   int __user *num_threads, int __user *weights, int __user *lat_critical);

asmlinkage long sys_set_slotcache_cpumask(int __user *mask);
asmlinkage long sys_set_swap_isolated(int enable);
#endif // _LINUX_EXTENDED_SYSCALLS_H
