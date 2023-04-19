#ifndef __RSWAP_SCHEDULER_H
#define __RSWAP_SCHEDULER_H

#include <linux/list.h>
#include <linux/mm.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "rswap_rdma.h"
#include "utils.h"

#define RSWAP_SCHEDULER_NUM 4
#define RSWAP_VQUEUE_MAX_SIZE 2048
#define MAX_PROC_NUM 50
#define MAX_CORES_NUM 200
#define MAX_PROC_NAME_LENGTH 100
#define MAX_POLICY_BOUND 120

#define ADC_NUM_APPS 4

struct rswap_request {
	pgoff_t offset;
	struct page *page;
};

static inline int rswap_request_copy(struct rswap_request *dst,
				     struct rswap_request *src)
{
	if (!src || !dst) {
		return -EINVAL;
	}
	memcpy(dst, src, sizeof(struct rswap_request));

	return 0;
}

struct rswap_vqueue {
	atomic_t cnt;
	atomic_t send_direct;
	int max_cnt;
	unsigned head;
	unsigned tail;
	struct rswap_request *reqs;
	spinlock_t lock;
};

int rswap_vqueue_init(struct rswap_vqueue *queue);
int rswap_vqueue_destroy(struct rswap_vqueue *queue);
int rswap_vqueue_enqueue(struct rswap_vqueue *queue,
			 struct rswap_request *request);
int rswap_vqueue_dequeue(struct rswap_vqueue *queue,
			 struct rswap_request **request);
int rswap_vqueue_drain(int cpu, enum rdma_queue_type type);

#define RSWAP_PROC_NAME_LEN 32
#define RSWAP_ONLINE_CORES 80
struct rswap_proc {
	char name[RSWAP_PROC_NAME_LEN];
	int bw_weight;
	int num_threads;
	int critical_latency;
	int cores[RSWAP_ONLINE_CORES];

	atomic_t sent_pkts[NUM_QP_TYPE];

	spinlock_t lock;
	struct list_head list_node;
};
int rswap_proc_init(struct rswap_proc *proc, char *name, int critical_latency);
int rswap_proc_destroy(struct rswap_proc *proc);
int rswap_proc_clr_weight(struct rswap_proc *proc);
int rswap_proc_set_weight(struct rswap_proc *proc, int bw_weight,
			  int num_threads, int *cores);

void rswap_proc_send_pkts_inc(struct rswap_proc *proc,
			      enum rdma_queue_type type);
void rswap_proc_send_pkts_dec(struct rswap_proc *proc,
			      enum rdma_queue_type type);
bool is_proc_throttled(struct rswap_proc *proc, enum rdma_queue_type type);

void rswap_activate_bw_control(int enable);

struct rswap_vqtriple {
	int id;
	struct rswap_proc *proc;
	struct rswap_vqueue qs[NUM_QP_TYPE];
};

int rswap_vqtriple_init(struct rswap_vqtriple *vqtri, int id);
int rswap_vqtriple_destroy(struct rswap_vqtriple *vqtri);

struct rswap_vqlist {
	int cnt;
	struct rswap_vqtriple *vqtris;
	spinlock_t lock;
};

int rswap_vqlist_init(void);
int rswap_vqlist_destroy(void);
void rswap_deregister_procs(void);
struct rswap_vqueue *rswap_vqlist_get(int qid, enum rdma_queue_type type);
struct rswap_vqtriple *rswap_vqlist_get_triple(int qid);

struct rswap_scheduler {
	struct rswap_vqlist *vqlist;
	struct rdma_session_context *rdma_session;

	struct task_struct *scher_thds[RSWAP_SCHEDULER_NUM];

	int total_bw_weight;
	atomic_t total_pkts;
	atomic_t wait_for_others;
	atomic_t wait_thd_num;
	atomic64_t time_for_sched;
	int scheduler_num;
	int check_thd_num[MAX_POLICY_BOUND];
	spinlock_t lock;
	struct list_head proc_list;
};

struct proc_info {
	int num_apps;
	int proc_name_length;
};

struct threshold_info {
	int scheduler_threshold;
	int auto_maintain_time;
};

int rswap_scheduler_init(void);
int rswap_scheduler_stop(void);
int rswap_scheduler_thread(void *args);
void rswap_scheduler_reset(void);

extern struct rswap_vqlist *global_rswap_vqueue_list;
extern struct rswap_scheduler *global_rswap_scheduler;
extern int global_rswap_scheduler_cores[RSWAP_SCHEDULER_NUM];
extern int global_scheduler_threshold;
extern int global_policy_upper_bound;

#define print_err(errno)                                                       \
	pr_err(KERN_ERR "%s, line %d : %d\n", __func__, __LINE__, errno)

#endif
