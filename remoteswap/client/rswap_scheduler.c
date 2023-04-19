#include <linux/atomic.h>
#include <linux/delay.h>
#include <linux/interrupt.h>
#include <linux/kthread.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/swap_stats.h>

#include "rswap_scheduler.h"

struct rswap_vqlist *global_rswap_vqlist = NULL;
struct rswap_scheduler *global_rswap_scheduler = NULL;
int global_rswap_scheduler_cores[RSWAP_SCHEDULER_NUM] = { 6, 8, 10, 12 };
int global_thd_id[RSWAP_SCHEDULER_NUM] = { 1, 2, 3, 4 };
int global_scheduler_threshold = 25000;
int global_policy_upper_bound = 40;
int global_poll_times = 10;
int global_config = 0;
int global_auto_threshold = 0;
int global_auto_times = 0;
int global_auto_upper_bound = 20000;
int global_auto_lower_bound = 20000;
int global_highest_pkts = 20000;
int global_auto_maintain_time = 1000;
int global_auto_thd_request = 0;
int global_auto_wait_times = 0;
int global_waiting_pkts = 0;
uint64_t global_check_duration = 100000000;

char global_set_names[MAX_PROC_NUM * MAX_PROC_NAME_LENGTH];
int global_set_cores[MAX_PROC_NUM * MAX_CORES_NUM];
int global_set_weights[MAX_PROC_NUM];
int global_set_num_threads[MAX_PROC_NUM];
int global_set_lat_critical[MAX_PROC_NUM];
int global_set_scheduler_policy_boundary[RSWAP_SCHEDULER_NUM + 1];

bool _bw_control_enabled = true;

void rswap_activate_bw_control(int enable)
{
	_bw_control_enabled = !!enable;
	pr_info("Swap BW control: %s", _bw_control_enabled ? "enabled" : "disabled");
}
EXPORT_SYMBOL(rswap_activate_bw_control);

inline bool is_bw_control_enabled(void)
{
	return _bw_control_enabled;
}

int rswap_vqueue_init(struct rswap_vqueue *vqueue)
{
	if (!vqueue) {
		return -EINVAL;
	}

	atomic_set(&vqueue->cnt, 0);
	atomic_set(&vqueue->send_direct, 1);

	vqueue->max_cnt = RSWAP_VQUEUE_MAX_SIZE;
	vqueue->head = 0;
	vqueue->tail = 0;

	vqueue->reqs = (struct rswap_request *)vmalloc(sizeof(struct rswap_request) * vqueue->max_cnt);

	spin_lock_init(&vqueue->lock);

	return 0;
}
EXPORT_SYMBOL(rswap_vqueue_init);

int rswap_vqueue_destroy(struct rswap_vqueue *vqueue)
{
	if (!vqueue) {
		return -EINVAL;
	}
	vfree(vqueue->reqs);
	memset(vqueue, 0, sizeof(struct rswap_vqueue));
	return 0;
}

inline void rswap_vqueue_enlarge(struct rswap_vqueue *vqueue)
{
	unsigned long flags;
	unsigned head_len;
	unsigned tail_len;
	struct rswap_request *old_buf;
	spin_lock_irqsave(&vqueue->lock, flags);
	if (vqueue->head < vqueue->tail) {
		head_len = vqueue->tail - vqueue->head;
		old_buf = vqueue->reqs;
		vqueue->reqs = (struct rswap_request *)vmalloc(sizeof(struct rswap_request) * vqueue->max_cnt * 2);
		memcpy(vqueue->reqs, old_buf + vqueue->head, sizeof(struct rswap_request) * head_len);
	} else {
		head_len = vqueue->max_cnt - vqueue->head;
		tail_len = vqueue->head;
		old_buf = vqueue->reqs;
		vqueue->reqs = (struct rswap_request *)vmalloc(sizeof(struct rswap_request) * vqueue->max_cnt * 2);
		memcpy(vqueue->reqs, old_buf + vqueue->head, sizeof(struct rswap_request) * head_len);
		memcpy(vqueue->reqs + head_len, old_buf, sizeof(struct rswap_request) * tail_len);
	}
	vqueue->head = 0;
	vqueue->tail = vqueue->max_cnt;
	vqueue->max_cnt *= 2;
	vfree(old_buf);
	spin_unlock_irqrestore(&vqueue->lock, flags);
	pr_info("Enlarge vqueue to %u\n", vqueue->max_cnt);
}

int rswap_vqueue_enqueue(struct rswap_vqueue *vqueue, struct rswap_request *request)
{
	int cnt;
	if (!vqueue || !request) {
		return -EINVAL;
	}

	cnt = atomic_read(&vqueue->cnt);
	if (cnt == vqueue->max_cnt) {
		rswap_vqueue_enlarge(vqueue);
	}

	rswap_request_copy(&vqueue->reqs[vqueue->tail], request);
	vqueue->tail = (vqueue->tail + 1) % vqueue->max_cnt;
	atomic_inc(&vqueue->cnt);

	return 0;
}
EXPORT_SYMBOL(rswap_vqueue_enqueue);

int rswap_vqueue_dequeue(struct rswap_vqueue *vqueue, struct rswap_request **request)
{
	int cnt;

	cnt = atomic_read(&vqueue->cnt);
	if (cnt == 0) {
		return -1;
	} else if (cnt < vqueue->max_cnt) {
		*request = &vqueue->reqs[vqueue->head];
		vqueue->head = (vqueue->head + 1) % vqueue->max_cnt;
		return 0;
	} else {
		unsigned long flags;
		spin_lock_irqsave(&vqueue->lock, flags);
		*request = &vqueue->reqs[vqueue->head];
		vqueue->head = (vqueue->head + 1) % vqueue->max_cnt;
		spin_unlock_irqrestore(&vqueue->lock, flags);
		return 0;
	}

	return -1;
}
EXPORT_SYMBOL(rswap_vqueue_dequeue);

int rswap_vqueue_drain(int cpu, enum rdma_queue_type type)
{
	unsigned long flags;
	struct rswap_vqueue *vqueue;
	struct rswap_rdma_queue *rdma_queue;

	vqueue = rswap_vqlist_get(cpu, type);
	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, type);
	while (atomic_read(&vqueue->cnt) > 0) {
		if (atomic_read(&rdma_queue->rdma_post_counter) > 0) {
			spin_lock_irqsave(&rdma_queue->cq_lock, flags);
			ib_process_cq_direct(rdma_queue->cq, 16);
			spin_unlock_irqrestore(&rdma_queue->cq_lock, flags);
		}
		cond_resched();
	}
	return 0;
}
EXPORT_SYMBOL(rswap_vqueue_drain);

int rswap_vqtriple_init(struct rswap_vqtriple *vqtri, int id)
{
	int ret;
	int type;
	if (!vqtri) {
		return -EINVAL;
	}

	vqtri->id = id;
	vqtri->proc = NULL;
	for (type = 0; type < NUM_QP_TYPE; type++) {
		ret = rswap_vqueue_init(&vqtri->qs[type]);
		if (ret) {
			print_err(ret);
			goto cleanup;
		}
	}
	return 0;

cleanup:
	for (; type >= 0; type--) {
		rswap_vqueue_destroy(&vqtri->qs[type]);
	}
	return -1;
}

int rswap_vqtriple_destroy(struct rswap_vqtriple *vqtri)
{
	int ret;
	int type;

	if (!vqtri) {
		return -EINVAL;
	}

	for (type = 0; type < NUM_QP_TYPE; type++) {
		ret = rswap_vqueue_destroy(&vqtri->qs[type]);
		if (ret)
			print_err(ret);
	}
	return 0;
}

int rswap_vqlist_init(void)
{
	int ret;
	int cpu;

	global_rswap_vqlist = kzalloc(sizeof(struct rswap_vqlist), GFP_ATOMIC);
	global_rswap_vqlist->cnt = online_cores;
	global_rswap_vqlist->vqtris = kzalloc(sizeof(struct rswap_vqtriple) * online_cores, GFP_ATOMIC);
	spin_lock_init(&global_rswap_vqlist->lock);

	for (cpu = 0; cpu < online_cores; cpu++) {
		ret = rswap_vqtriple_init(&global_rswap_vqlist->vqtris[cpu], cpu);
		if (ret) {
			print_err(ret);
			goto cleanup;
		}
	}

	return 0;

cleanup:
	for (; cpu >= 0; cpu--) {
		ret = rswap_vqtriple_destroy(&global_rswap_vqlist->vqtris[cpu]);
		if (ret)
			print_err(ret);
	}
	kfree(global_rswap_vqlist->vqtris);
	kfree(global_rswap_vqlist);
	return ret;
}

int rswap_vqlist_destroy(void)
{
	int ret = 0;
	int cpu;
	for (cpu = 0; cpu < online_cores; cpu++) {
		ret = rswap_vqtriple_destroy(&global_rswap_vqlist->vqtris[cpu]);
		if (ret)
			print_err(ret);
	}
	kfree(global_rswap_vqlist->vqtris);
	kfree(global_rswap_vqlist);
	return ret;
}

inline struct rswap_vqueue *rswap_vqlist_get(int qid, enum rdma_queue_type type)
{
	return &global_rswap_vqlist->vqtris[qid].qs[type];
}

inline struct rswap_vqtriple *rswap_vqlist_get_triple(int qid)
{
	return &global_rswap_vqlist->vqtris[qid];
}

int rswap_proc_init(struct rswap_proc *proc, char *name, int critical_latency)
{
	unsigned long flag;
	if (!proc) {
		return -EINVAL;
	}

	strcpy(proc->name, name);
	proc->bw_weight = -1;
	proc->num_threads = 0;
	proc->critical_latency = critical_latency;

	memset(proc->cores, 0, sizeof(proc->cores));
	memset(proc->sent_pkts, 0, sizeof(proc->sent_pkts));

	spin_lock_irqsave(&global_rswap_scheduler->lock, flag);
	list_add_tail(&proc->list_node, &global_rswap_scheduler->proc_list);
	spin_unlock_irqrestore(&global_rswap_scheduler->lock, flag);

	pr_info("init proc %s in rswap, latency critical: %d\n", proc->name, critical_latency);
	return 0;
}

int rswap_proc_destroy(struct rswap_proc *proc)
{
	unsigned long flag;
	if (!proc) {
		return -EINVAL;
	}

	spin_lock_irqsave(&global_rswap_scheduler->lock, flag);
	list_del(&proc->list_node);
	spin_unlock_irqrestore(&global_rswap_scheduler->lock, flag);

	rswap_proc_clr_weight(proc);
	pr_info("destroy proc %s in rswap", proc->name);
	kfree(proc);
	return 0;
}

int rswap_proc_clr_weight(struct rswap_proc *proc)
{
	int cpu;
	if (!proc) {
		return -EINVAL;
	}

	for (cpu = 0; cpu < online_cores; cpu++) {
		if (proc->cores[cpu]) {
			global_rswap_vqlist->vqtris[cpu].proc = NULL;
		}
	}
	if (proc->bw_weight > 0)
		global_rswap_scheduler->total_bw_weight /= proc->bw_weight;
	proc->bw_weight = -1;
	memset(proc->cores, 0, sizeof(proc->cores));
	memset(proc->sent_pkts, 0, sizeof(proc->sent_pkts));

	pr_info("clear BW weight of proc %s, total BW weight now: %d", proc->name,
		global_rswap_scheduler->total_bw_weight);
	return 0;
}

int rswap_proc_set_weight(struct rswap_proc *proc, int bw_weight, int num_threads, int *cores)
{
	int thd;
	if (!proc || !cores) {
		return -EINVAL;
	}

	if (proc->bw_weight != -1) {
		rswap_proc_clr_weight(proc);
	}

	proc->bw_weight = bw_weight;
	proc->num_threads = num_threads;
	memcpy(proc->cores, cores, sizeof(int) * num_threads);
	for (thd = 0; thd < num_threads; thd++) {
		global_rswap_vqlist->vqtris[cores[thd]].proc = proc;
		pr_info("Bind core %d to proc %s", cores[thd], proc->name);
	}
	if (bw_weight > 0)
		global_rswap_scheduler->total_bw_weight *= bw_weight;

	pr_info("set BW weight of proc %s, total BW weight now: %d", proc->name,
		global_rswap_scheduler->total_bw_weight);
	return 0;
}

void rswap_set_proc(void __user *info, char __user *names, int __user *cores, int __user *num_threads,
		    int __user *weights, int __user *lat_critical)
{
	struct rswap_proc *procs;
	struct proc_info _info_struct;

	int total_threads_num = 0;
	int i = 0;
	char *_names_p;
	int *_cores_p;
	struct proc_info *_info = &_info_struct;

	if (!global_rswap_scheduler) {
		return;
	}

	copy_from_user(&_info_struct, info, sizeof(struct proc_info));
	if (_info->num_apps > MAX_PROC_NUM) {
		pr_info("%s, error: the number of apps should not exceed %d. It is %d\n", __func__, MAX_PROC_NUM,
			_info->num_apps);
		goto out;
	}
	copy_from_user(global_set_num_threads, num_threads, sizeof(int) * _info->num_apps);
	copy_from_user(global_set_weights, weights, sizeof(int) * _info->num_apps);
	copy_from_user(global_set_lat_critical, lat_critical, sizeof(int) * _info->num_apps);
	for (i = 0; i < _info->num_apps; i++) {
		if (global_set_num_threads[i] > MAX_CORES_NUM) {
			pr_info("%s, error: the number of app's threads should not exceed %d. It is %d\n", __func__,
				MAX_CORES_NUM, global_set_num_threads[i]);
			goto out;
		}
		total_threads_num += global_set_num_threads[i];
	}

	copy_from_user(global_set_names, names, sizeof(char) * _info->proc_name_length * _info->num_apps);
	copy_from_user(global_set_cores, cores, sizeof(int) * total_threads_num);
	if (global_config) {
		kthread_stop(global_rswap_scheduler->scher_thds[0]);
		rswap_deregister_procs();
		pr_info("%s, stop threads and deregister procs\n", __func__);
	} else {
		global_config = 1;
	}

	_names_p = global_set_names;
	_cores_p = global_set_cores;
	for (i = 0; i < _info->num_apps; i++) {
		procs = kzalloc(sizeof(struct rswap_proc), GFP_ATOMIC);

		pr_info("%s, register for proc %s\n", __func__, _names_p);
		rswap_proc_init(procs, _names_p, global_set_lat_critical[i]);
		rswap_proc_set_weight(procs, global_set_weights[i], global_set_num_threads[i], _cores_p);
		_names_p += _info->proc_name_length;
		_cores_p += global_set_num_threads[i];
	}
	pr_info("%s, complete setting procs\n", __func__);

	rswap_scheduler_reset();
	global_rswap_scheduler->scher_thds[0] =
		kthread_create(rswap_scheduler_thread, (void *)(&global_thd_id[0]), "MAIN scheduler");
	kthread_bind(global_rswap_scheduler->scher_thds[0], global_rswap_scheduler_cores[0]);
	wake_up_process(global_rswap_scheduler->scher_thds[0]);

	pr_info("%s, restart the scheduler\n", __func__);
out:
	return;
}
EXPORT_SYMBOL(rswap_set_proc);

void scheduler_set_policy(int __user *scheduler_cores, int __user *info, int __user *scheduler_policy_boundary,
			  int check_duration, int poll_times)
{
	int i, j;
	struct threshold_info _info_struct;
	struct threshold_info *_info = &_info_struct;

	copy_from_user(global_set_scheduler_policy_boundary, scheduler_policy_boundary,
		       sizeof(int) * RSWAP_SCHEDULER_NUM);
	global_policy_upper_bound = global_set_scheduler_policy_boundary[RSWAP_SCHEDULER_NUM - 1] + 10;
	global_set_scheduler_policy_boundary[RSWAP_SCHEDULER_NUM] = global_policy_upper_bound;
	if (global_policy_upper_bound > MAX_POLICY_BOUND) {
		pr_info("%s, error: scheduler policy boundary exceed %d, it is %d\n", __func__, MAX_POLICY_BOUND - 10,
			global_set_scheduler_policy_boundary[RSWAP_SCHEDULER_NUM - 1]);
		return;
	}

	if (global_config) {
		kthread_stop(global_rswap_scheduler->scher_thds[0]);
		rswap_deregister_procs();
		pr_info("%s, stop threads and deregister procs\n", __func__);
	} else {
		global_config = 1;
	}

	copy_from_user(global_rswap_scheduler_cores, scheduler_cores, sizeof(int) * RSWAP_SCHEDULER_NUM);
	pr_info("%s, set scheduler cores.\n", __func__);

	copy_from_user(_info, info, sizeof(struct threshold_info));
	if (_info->scheduler_threshold >= 0) {
#ifdef LATENCY_THRESHOLD
		global_scheduler_threshold = _info->scheduler_threshold;
#else
		global_scheduler_threshold = _info->scheduler_threshold * 1024 / 40;
#endif
		global_auto_threshold = 0;
		pr_info("%s, set scheduler threshold %d\n", __func__, global_scheduler_threshold);
	} else {
#ifdef LATENCY_THRESHOLD
		global_auto_upper_bound = -_info->scheduler_threshold;
		global_auto_lower_bound = global_auto_upper_bound / 2;
		if(global_auto_lower_bound > 1000) { //1ms
			global_auto_lower_bound = 1000;
		}
#else
		global_auto_upper_bound = (-_info->scheduler_threshold) * 1024 / 40;
		global_auto_lower_bound = global_auto_upper_bound / 2;
		if(global_auto_lower_bound > 20000) { //800M
			global_auto_lower_bound = 20000;
		}
#endif
		global_scheduler_threshold = global_auto_upper_bound;
		global_auto_threshold = 1;
		global_auto_times = 0;
		global_auto_maintain_time = _info->auto_maintain_time * 10;
		global_highest_pkts = 1;
		pr_info("%s, auto scheduler threshold, with upper bound: %d\n", __func__, global_auto_upper_bound);
	}

	global_poll_times = poll_times;
	pr_info("%s, set scheduler poll times %d\n", __func__, poll_times);

	global_check_duration = (uint64_t)check_duration * 1000000;
	pr_info("%s, set scheduler check duration %lld\n", __func__, global_check_duration);

	if (global_set_scheduler_policy_boundary[0] < 0) {
		pr_info("%s, auto scheduler boudnary, with lower bound %d\n", __func__,
			global_set_scheduler_policy_boundary[1]);
		global_auto_thd_request = 1;
		global_auto_wait_times = 0;
		global_waiting_pkts = 0;
		global_set_scheduler_policy_boundary[0] = 0;
		goto skip;
	}
	global_auto_thd_request = 0;
skip:
	for (i = 1; i <= RSWAP_SCHEDULER_NUM; i++) {
		for (j = global_set_scheduler_policy_boundary[i - 1]; j < global_set_scheduler_policy_boundary[i];
		     j++) {
			global_rswap_scheduler->check_thd_num[j] = i;
		}
		pr_info("%s, set scheduler boundary %d, from %d to %d\n", __func__, i,
			global_set_scheduler_policy_boundary[i - 1], global_set_scheduler_policy_boundary[i] - 1);
	}

	rswap_scheduler_reset();
	global_rswap_scheduler->scher_thds[0] =
		kthread_create(rswap_scheduler_thread, (void *)(&global_thd_id[0]), "MAIN scheduler");
	kthread_bind(global_rswap_scheduler->scher_thds[0], global_rswap_scheduler_cores[0]);
	wake_up_process(global_rswap_scheduler->scher_thds[0]);

	pr_info("%s, restart the scheduler\n", __func__);

	return;
}
EXPORT_SYMBOL(scheduler_set_policy);

inline void rswap_proc_send_pkts_inc(struct rswap_proc *proc, enum rdma_queue_type type)
{
	if (!proc)
		return;
	atomic_inc(&proc->sent_pkts[type]);
}

inline void rswap_proc_send_pkts_dec(struct rswap_proc *proc, enum rdma_queue_type type)
{
	if (!proc)
		return;
	atomic_dec(&proc->sent_pkts[type]);
}

void rswap_deregister_procs(void)
{
	struct rswap_proc *proc, *tmp;

	list_for_each_entry_safe (proc, tmp, &global_rswap_scheduler->proc_list, list_node) {
		rswap_proc_destroy(proc);
	}
}

void rswap_scheduler_reset(void)
{
	global_rswap_scheduler->total_bw_weight = 1;
	global_rswap_scheduler->scheduler_num = 0;
	atomic_set(&global_rswap_scheduler->total_pkts, 0);
	atomic_set(&global_rswap_scheduler->wait_for_others, 0);
	atomic_set(&global_rswap_scheduler->wait_thd_num, 0);
	atomic64_set(&global_rswap_scheduler->time_for_sched, get_cycles());
}

int rswap_scheduler_init(void)
{
	struct rdma_session_context *rdma_session = &rdma_session_global;
	int ret = 0;

	pr_info("%s starts.\n", __func__);

	ret = rswap_vqlist_init();
	if (ret) {
		print_err(ret);
		return ret;
	}
	pr_info("%s inits vqueues.\n", __func__);

	global_rswap_scheduler = (struct rswap_scheduler *)vmalloc(sizeof(struct rswap_scheduler));

	rswap_scheduler_reset();

	spin_lock_init(&global_rswap_scheduler->lock);
	INIT_LIST_HEAD(&global_rswap_scheduler->proc_list);

	global_rswap_scheduler->vqlist = global_rswap_vqlist;
	global_rswap_scheduler->rdma_session = rdma_session;
	pr_info("%s, wait for configuration to launch scheduler thd.\n", __func__);

#ifdef RSWAP_KERNEL_SUPPORT
	syscall_scheduler_set_policy = scheduler_set_policy;
	syscall_rswap_set_proc = rswap_set_proc;
	set_swap_bw_control = rswap_activate_bw_control;
	pr_info("Swap RDMA bandwidth control functions registered.");
#else
	pr_info("Kernel doesn't support swap RDMA bandwidth control.");
#endif

	return 0;
}

int rswap_scheduler_stop(void)
{
	int ret = 0;
	if (!global_rswap_scheduler) {
		return -EINVAL;
	}

	if (global_config) {
		ret = kthread_stop(global_rswap_scheduler->scher_thds[0]);
	}

	rswap_deregister_procs();
#ifdef RSWAP_KERNEL_SUPPORT
	set_swap_bw_control = NULL;
	syscall_scheduler_set_policy = NULL;
	syscall_rswap_set_proc = NULL;
	pr_info("Swap RDMA bandwidth control functions deregistered.");
#else
	pr_info("Kernel doesn't support swap RDMA bandwidth control. Do nothing.");
#endif
	if (global_rswap_vqlist) {
		rswap_vqlist_destroy();
		global_rswap_vqlist = NULL;
	}
	vfree(global_rswap_scheduler);
	pr_info("%s, line %d, after kthread stop\n", __func__, __LINE__);
	return ret;
}

static inline bool poll_all_vqueues(enum rdma_queue_type type, int i, int n)
{
	int ret;
	bool find;
	int cpu;

	struct rswap_vqtriple *vqtri;
	struct rswap_proc *proc, *tmp;
	struct rswap_vqueue *vqueue;
	struct rswap_request *vrequest;

	int min_active_pkts = 1 << 30;
	int min_weight = 1 << 30;
	int num_active_procs = 0;
	int wait_pkts = 0;
	struct rswap_proc *baseline_proc = NULL;

	find = false;

	if (n == 0)
		n = 1;

	list_for_each_entry_safe (proc, tmp, &global_rswap_scheduler->proc_list, list_node) {
		int thd;
		int active_pkts = 0;
		int vqueue_cnt = 0;
		active_pkts = atomic_read(&proc->sent_pkts[type]) / n;
		for (thd = proc->num_threads * (i - 1) / n; thd < proc->num_threads * i / n; thd++) {
			vqueue = rswap_vqlist_get(proc->cores[thd], type);
			vqueue_cnt = atomic_read(&vqueue->cnt);
			active_pkts += vqueue_cnt;
			wait_pkts += vqueue_cnt;
		}
		if (wait_pkts > 0) {
			int curr_weight;
			num_active_procs++;
			curr_weight = active_pkts * global_rswap_scheduler->total_bw_weight / proc->bw_weight;
			if (curr_weight < min_weight) {
				min_weight = curr_weight;
				min_active_pkts = active_pkts;
				baseline_proc = proc;
			}
		}
	}

	if (num_active_procs == 0) {
		;
	} else if (num_active_procs == 1) {
		int thd;
		int base_num_threads = baseline_proc->num_threads;
		int poll_start = base_num_threads * (i - 1) / n;
		int poll_end = base_num_threads * i / n;
		for (thd = poll_start; thd < poll_end; thd++) {
			cpu = baseline_proc->cores[thd];
			vqueue = rswap_vqlist_get(cpu, type);
			ret = rswap_vqueue_dequeue(vqueue, &vrequest);
			if (ret == 0) {
				rswap_proc_send_pkts_inc(baseline_proc, type);
				rswap_rdma_send_note(cpu, vrequest->offset, vrequest->page, type, 0);
				atomic_dec(&vqueue->cnt);
				find = true;
			} else if (ret != -1) {
				print_err(ret);
			}
		}
	} else if (num_active_procs > 1) {
		list_for_each_entry_safe (proc, tmp, &global_rswap_scheduler->proc_list, list_node) {
			int budget;
			int prev_budget;
			int thd;
			if (is_bw_control_enabled()) {
				budget = ((min_active_pkts * proc->bw_weight / baseline_proc->bw_weight) -
					  (atomic_read(&proc->sent_pkts[type])) / n);
			} else {
				budget = proc->num_threads;
			}
			if (!proc->critical_latency && type == QP_LOAD_ASYNC && budget > proc->num_threads / n)
				budget = proc->num_threads / n;
				
			if (budget == 0) {
				budget = 1;
			}

			prev_budget = budget;
			thd = proc->num_threads * (i - 1) / n;
			while (budget > 0) {
				cpu = proc->cores[thd];
				vqtri = rswap_vqlist_get_triple(cpu);
				vqueue = &vqtri->qs[type];
				ret = rswap_vqueue_dequeue(vqueue, &vrequest);
				if (ret == 0) {
					rswap_proc_send_pkts_inc(proc, type);
					rswap_rdma_send_note(cpu, vrequest->offset, vrequest->page, type, 0);
					atomic_dec(&vqueue->cnt);
					budget--;
					find = true;
				} else if (ret != -1) {
					print_err(ret);
				}
				thd++;
				if (thd == proc->num_threads * i / n) {
					if (budget == prev_budget)
						break;
					thd = proc->num_threads * (i - 1) / n;
					prev_budget = budget;
				}
			}
		}
	}
	return find;
}

static inline void poll_idle_cores(int i, int n)
{
	int cpu;
	if (n == 0)
		n = 1;
	for (cpu = online_cores * (i - 1) / n; cpu < online_cores * i / n; cpu++) {
		int ret;
		int type;
		int j;
		struct rswap_vqtriple *vqtri;
		struct rswap_vqueue *vqueue;
		struct rswap_request *vrequest;

		vqtri = rswap_vqlist_get_triple(cpu);
		for (j = 0; j < n; j++) {
			if (cpu == global_rswap_scheduler_cores[j]) {
				continue;
			}
		}
		if (vqtri->proc)
			continue;
		for (type = 0; type < NUM_QP_TYPE; type++) {
			vqueue = rswap_vqlist_get(cpu, type);
			ret = rswap_vqueue_dequeue(vqueue, &vrequest);
			if (ret == 0) {
				rswap_rdma_send_note(cpu, vrequest->offset, vrequest->page, type, 0);
				atomic_dec(&vqueue->cnt);
			} else if (ret != -1) {
				print_err(ret);
			}
		}
	}
}

void inc_scheduler_thread(int request_num)
{
	int sched_num = 0;
	int check_ret = 0;
	char name[20];
	int from = 0;
	atomic_inc(&global_rswap_scheduler->wait_for_others);
	atomic_inc(&global_rswap_scheduler->wait_thd_num);
	while (atomic_read(&global_rswap_scheduler->wait_thd_num) != global_rswap_scheduler->scheduler_num &&
	       !kthread_should_stop()) {
			cond_resched();
	}

	sched_num = global_rswap_scheduler->scheduler_num;
	global_rswap_scheduler->scheduler_num = request_num;

	from = sched_num;
	while (sched_num < request_num) {
		sprintf(name, "RSWAP sched%d", sched_num);
		global_rswap_scheduler->scher_thds[sched_num] =
			kthread_create(rswap_scheduler_thread, (void *)(&global_thd_id[sched_num]), name);
		kthread_bind(global_rswap_scheduler->scher_thds[sched_num], global_rswap_scheduler_cores[sched_num]);
		wake_up_process(global_rswap_scheduler->scher_thds[sched_num]);
		sched_num++;
	}

	check_ret = atomic_dec_return(&global_rswap_scheduler->wait_for_others);
	if (check_ret != 0)
		pr_info("%s: error, the control variable for dynamic scheduler is not zero, but %d.\n", __func__,
			check_ret);
	atomic_set(&global_rswap_scheduler->wait_thd_num, 0);

	//pr_info("%s, inc from %d to %d\n", __func__, from, request_num);
}

void dec_scheduler_thread(int request_num)
{
	int check_ret = 0;
	int sched_num = global_rswap_scheduler->scheduler_num;
	//int from = global_rswap_scheduler->scheduler_num;
	atomic_inc(&global_rswap_scheduler->wait_for_others);
	atomic_inc(&global_rswap_scheduler->wait_thd_num);
	while (atomic_read(&global_rswap_scheduler->wait_thd_num) != global_rswap_scheduler->scheduler_num &&
	       !kthread_should_stop())
		cond_resched();

	while(sched_num > request_num) {
		global_rswap_scheduler->scheduler_num--;
		sched_num = global_rswap_scheduler->scheduler_num;

		kthread_stop(global_rswap_scheduler->scher_thds[sched_num]);
	}
	check_ret = atomic_dec_return(&global_rswap_scheduler->wait_for_others);
	if (check_ret != 0)
		pr_info("%s: error, the control variable for dynamic scheduler is not "
			"zero, but %d.\n",
			__func__, check_ret);
	atomic_set(&global_rswap_scheduler->wait_thd_num, 0);

	//pr_info("%s, dec from %d to %d\n", __func__, from, request_num);
}

static inline void wait_vqueue_clear(void)
{
	int cpu;

	for (cpu = 0; cpu < online_cores; cpu++) {
		int ret;
		int type;
		struct rswap_vqueue *vqueue;
		struct rswap_request *vrequest;

		if (cpu == global_rswap_scheduler_cores[0])
			continue;
		for (type = 0; type < NUM_QP_TYPE; type++) {
			vqueue = rswap_vqlist_get(cpu, type);
		again:
			ret = rswap_vqueue_dequeue(vqueue, &vrequest);
			if (ret == 0) {
				rswap_proc_send_pkts_inc(rswap_vqlist_get_triple(cpu)->proc, type);
				rswap_rdma_send_note(cpu, vrequest->offset, vrequest->page, type, 0);
				atomic_dec(&vqueue->cnt);
				cond_resched();
				goto again;
			} else if (ret != -1) {
				print_err(ret);
			}
		}
	}
	return;
}

inline void set_scheduler_boundary(void)
{
	int i = 0;
	int j = 0;
	int lbound = 0;
	int rbound = 0;
	for (i = 1; i <= RSWAP_SCHEDULER_NUM; i++) {
		lbound = global_waiting_pkts * (i * 2 + 1) / 10;
		rbound = global_waiting_pkts * (i * 2 + 3) / 10;
		if (i == 0) {
			lbound = 0;
		}
		if (lbound > MAX_POLICY_BOUND - 10) {
			lbound = MAX_POLICY_BOUND - 10;
		}
		if (rbound > MAX_POLICY_BOUND - 10) {
			rbound = MAX_POLICY_BOUND - 10;
		}
		if (lbound > global_set_scheduler_policy_boundary[i - 1]) {
			lbound = global_set_scheduler_policy_boundary[i - 1];
		}
		if (rbound > global_set_scheduler_policy_boundary[i]) {
			rbound = global_set_scheduler_policy_boundary[i];
		}
		for (j = lbound; j < rbound; j++) {
			global_rswap_scheduler->check_thd_num[j] = i;
		}
		if (i == RSWAP_SCHEDULER_NUM) {
			global_policy_upper_bound = rbound + 6;
		}
	}
}

static inline void auto_adjust_threshold(int total_pkts)
{
	if (total_pkts > global_highest_pkts && total_pkts > global_auto_lower_bound) {
		if (total_pkts > global_auto_upper_bound)
			global_highest_pkts = global_auto_upper_bound;
		else
			global_highest_pkts = total_pkts;
		global_auto_times = 0;
		global_scheduler_threshold = global_highest_pkts * 95 / 100;
	} else {
		global_auto_times++;
		if (global_auto_times >= global_auto_maintain_time) {
			if (global_highest_pkts * 4 / 5 > global_auto_lower_bound) {
				global_highest_pkts = global_highest_pkts * 4 / 5;
				global_scheduler_threshold = global_highest_pkts * 95 / 100;
			}
			global_auto_times = 0;
		}
	}
}

static inline void auto_adjust_boundary(int waiting_pkts)
{
	if (waiting_pkts > global_waiting_pkts && waiting_pkts >= global_set_scheduler_policy_boundary[1] / 2) {
		global_waiting_pkts = waiting_pkts;
		global_auto_wait_times = 0;
		set_scheduler_boundary();
	} else {
		global_auto_wait_times++;
		if (global_auto_wait_times >= global_auto_maintain_time) {
			if (global_waiting_pkts * 4 / 5 >= global_set_scheduler_policy_boundary[1] / 2) {
				global_waiting_pkts = global_waiting_pkts * 4 / 5;
				set_scheduler_boundary();
			}
			global_auto_wait_times = 0;
		}
	}
}

static inline void check_all_vqueues(int *sched_num)
{
	struct rswap_proc *proc, *tmp;
	struct rswap_vqueue *vqueue;
	int check = 0;
	int thd_num_request = 1;
	uint64_t time_for_sched = 0;

	time_for_sched = atomic64_read(&global_rswap_scheduler->time_for_sched);
	if (get_cycles() - time_for_sched >= global_check_duration) {
		int cpu = 0;
		int type = 0;
		int total_pkts = 0;
		int waiting_pkts = 0;

		atomic64_set(&global_rswap_scheduler->time_for_sched, get_cycles());
		total_pkts = atomic_read(&global_rswap_scheduler->total_pkts);
		atomic_set(&global_rswap_scheduler->total_pkts, 0);
#ifdef LATENCY_THRESHOLD
		if (total_pkts % 10000 != 0)
			total_pkts = (total_pkts / 10000) / (total_pkts % 10000);
#endif

		for (type = 0; type < NUM_QP_TYPE; type++) {
			list_for_each_entry_safe (proc, tmp, &global_rswap_scheduler->proc_list, list_node) {
				int thd;
				for (thd = 0; thd < proc->num_threads; thd++) {
					vqueue = rswap_vqlist_get(proc->cores[thd], type);
					waiting_pkts += atomic_read(&vqueue->cnt);
				}
			}
		}

		if (total_pkts >= global_scheduler_threshold && total_pkts < 1000000 && *sched_num == 0) {
			for (cpu = 0; cpu < online_cores; cpu++) {
				for (type = 0; type < NUM_QP_TYPE; type++) {
					vqueue = rswap_vqlist_get(cpu, type);
					atomic_set(&vqueue->send_direct, 0);
				}
			}
			global_rswap_scheduler->scheduler_num = 1;
			*sched_num = 1;

			// pr_info("%s: inc from 0, to 1\n", __func__);
		} else if (total_pkts > 0 && total_pkts < global_scheduler_threshold && *sched_num == 1) {
			for (cpu = 0; cpu < online_cores; cpu++) {
				for (type = 0; type < NUM_QP_TYPE; type++) {
					vqueue = rswap_vqlist_get(cpu, type);
					atomic_set(&vqueue->send_direct, 1);
				}
			}
			wait_vqueue_clear();
			global_rswap_scheduler->scheduler_num = 0;
			*sched_num = 0;

			// pr_info("%s: dec from 1, to 0\n", __func__);
		} else if (*sched_num >= 1) {
			if (waiting_pkts >= 0) {
				int check_id = waiting_pkts;
				if (waiting_pkts >= global_policy_upper_bound - 5)
					check_id = global_policy_upper_bound - 5;
				thd_num_request = global_rswap_scheduler->check_thd_num[check_id];
			}
			check = thd_num_request - global_rswap_scheduler->scheduler_num;
			if (check > 0) {
#ifdef POLICY_SLOW_UP_SLOW_DOWN
				inc_scheduler_thread(global_rswap_scheduler->scheduler_num + 1);
#else
				inc_scheduler_thread(thd_num_request);
#endif
				*sched_num = global_rswap_scheduler->scheduler_num;
			} else if (check < 0 && *sched_num >= 2) {
#ifdef POLICY_SLOW_UP_SLOW_DOWN
				dec_scheduler_thread(global_rswap_scheduler->scheduler_num - 1);
#else
				dec_scheduler_thread(thd_num_request);
#endif
				*sched_num = global_rswap_scheduler->scheduler_num;
			}
		}
		if (global_auto_threshold) {
			auto_adjust_threshold(total_pkts);
		}
		if (global_auto_thd_request) {
			auto_adjust_boundary(waiting_pkts);
		}
	}
}

bool check_sync(void)
{
	bool ret = false;
	if (atomic_read(&global_rswap_scheduler->wait_for_others) > 0 && !kthread_should_stop())
		ret = true;
	return ret;
}

void set_send_direct(void)
{
	int cpu = 0, type = 0;
	struct rswap_vqueue *vqueue;

	for (cpu = 0; cpu < online_cores; cpu++) {
		for (type = 0; type < NUM_QP_TYPE; type++) {
			vqueue = rswap_vqlist_get(cpu, type);
			atomic_set(&vqueue->send_direct, 1);
		}
	}
	wait_vqueue_clear();
}

int rswap_scheduler_thread(void *args)
{
	int id = *((int *)args);

	if (id == 1) {
		struct rswap_vqlist *vqlist;
		struct rdma_session_context *rdma_session;

		vqlist = global_rswap_scheduler->vqlist;
		rdma_session = global_rswap_scheduler->rdma_session;
		while (!kthread_should_stop() && (!vqlist || !rdma_session || !vqlist->cnt)) {
			usleep_range(5000, 5001);
			cond_resched();
		}
		pr_info("RSWAP scheduler starts working...\n");
		while (!kthread_should_stop() && vqlist->cnt < online_cores) {
			usleep_range(5000, 5001);
			cond_resched();
		}
		pr_info("RSWAP scheduler gets all vqueues.\n");
	} else {
		while (check_sync())
			cond_resched();
	}

	while (!kthread_should_stop()) {
		int repeat;
		bool global_find;
		int sched_num = id;

		global_find = false;
		for (repeat = 0; repeat < global_poll_times; repeat++) {
			bool store_find, load_find;

			store_find = true;
			load_find = true;
			sched_num = global_rswap_scheduler->scheduler_num;

			if (id == 1)
				check_all_vqueues(&sched_num);
			else {
				if (check_sync()) {
					atomic_inc(&global_rswap_scheduler->wait_thd_num);
					while (check_sync())
						cond_resched();
					if (kthread_should_stop())
						goto out;
					sched_num = global_rswap_scheduler->scheduler_num;
				}
			}
			while (store_find || load_find) {
				store_find = poll_all_vqueues(QP_STORE, id, sched_num);
				global_find |= store_find;

				load_find = poll_all_vqueues(QP_LOAD_SYNC, id, sched_num);
				global_find |= load_find;
			}

			load_find = poll_all_vqueues(QP_LOAD_ASYNC, id, sched_num);
			global_find |= load_find;
		}
		if (!global_find) {
			poll_idle_cores(id, sched_num);
		}
		if (sched_num == 0) {
			schedule();
		} else {
			cond_resched();
		}
	}
out:
	if (id == 1) {
		dec_scheduler_thread(1);
		set_send_direct();
	}
	return 0;
}
EXPORT_SYMBOL(rswap_scheduler_thread);
