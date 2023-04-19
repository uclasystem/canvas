#ifndef __RSWAP_RDMA_H
#define __RSWAP_RDMA_H

#include <linux/kernel.h>
#include <linux/err.h>
#include <linux/version.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/init.h>

#include <linux/blk-mq.h>
#include <linux/blkdev.h>
#include <linux/swapfile.h>
#include <linux/swap.h>
#include <linux/frontswap.h>

#include <rdma/ib_verbs.h>
#include <rdma/rdma_cm.h>
#include <linux/pci.h>
#include <linux/inet.h>
#include <linux/lightnvm.h>
#include <linux/sed-opal.h>

#include <linux/log2.h>
#include <linux/spinlock.h>
#include <linux/ktime.h>
#include <linux/scatterlist.h>
#include <asm/uaccess.h>
#include <linux/slab.h>
#include <linux/debugfs.h>
#include <linux/vmalloc.h>
#include <linux/delay.h>
#include <linux/page-flags.h>
#include <linux/smp.h>

#include "constants.h"
#include "utils.h"

#define RDMA_SEND_QUEUE_DEPTH 128
#define RDMA_RECV_QUEUE_DEPTH 32
#define GB_SHIFT 30
#define CHUNK_SHIFT (u64)(GB_SHIFT + ilog2(REGION_SIZE_GB))
#define CHUNK_MASK (u64)(((u64)1 << CHUNK_SHIFT) - 1)

enum rdma_queue_state {
	IDLE = 1,
	CONNECT_REQUEST,
	ADDR_RESOLVED,
	ROUTE_RESOLVED,
	CONNECTED,

	MEMORY_SERVER_AVAILABLE,

	FREE_MEM_RECV,
	RECEIVED_CHUNKS,
	RDMA_BUF_ADV,
	WAIT_OPS,
	RECV_STOP,

	RECV_EVICT,
	RDMA_WRITE_RUNNING,
	RDMA_READ_RUNNING,
	SEND_DONE,
	RDMA_DONE,

	RDMA_READ_ADV,
	RDMA_WRITE_ADV,
	CM_DISCONNECT,
	ERROR,
	TEST_DONE,
};

enum chunk_mapping_state {
	EMPTY,
	MAPPED,
};

struct remote_chunk {
	uint32_t remote_rkey;
	uint64_t remote_addr;
	uint64_t mapped_size;
	enum chunk_mapping_state chunk_state;
};

struct chunk_list {
	struct remote_chunk *chunks;
	uint32_t remote_mem_size;
	uint32_t chunk_num;
};

struct fs_rdma_req {
	struct ib_cqe cqe;
	struct page *page;
	u64 dma_addr;
	struct ib_sge sge;
	struct ib_rdma_wr rdma_wr;

	struct completion done;
	struct rswap_rdma_queue *rdma_queue;
	int no_wait_pkts;
#ifdef LATENCY_THRESHOLD
	uint64_t sent_time_start;
#endif
};

struct two_sided_rdma_send {
	struct ib_cqe cqe;
	struct ib_send_wr sq_wr;
	struct ib_sge send_sgl;
	struct message *send_buf;
	u64 send_dma_addr;

	struct rswap_rdma_queue *rdma_queue;
};

struct two_sided_rdma_recv {
	struct ib_cqe cqe;
	struct ib_recv_wr rq_wr;
	struct ib_sge recv_sgl;
	struct message *recv_buf;
	u64 recv_dma_addr;

	struct rswap_rdma_queue *rdma_queue;
};

struct rswap_rdma_queue {
	struct rdma_cm_id *cm_id;

	struct ib_cq *cq;
	struct ib_qp *qp;

	enum rdma_queue_state state;
	wait_queue_head_t sem;
	spinlock_t cq_lock;
	uint8_t freed;
	atomic_t rdma_post_counter;

	int q_index;
	enum rdma_queue_type type;
	struct rdma_session_context *rdma_session;

	struct kmem_cache *fs_rdma_req_cache;
	struct kmem_cache *rdma_req_sg_cache;
};

struct rswap_rdma_dev {
	struct ib_device *dev;
	struct ib_pd *pd;
};

struct rdma_session_context {
	struct rswap_rdma_dev *rdma_dev;
	struct rswap_rdma_queue *rdma_queues;

	uint16_t port;
	u8 addr[16];
	uint8_t addr_type;
	int send_queue_depth;
	int recv_queue_depth;

	struct two_sided_rdma_recv rdma_recv_req;
	struct two_sided_rdma_send rdma_send_req;

	struct chunk_list remote_mem_pool;
};

static inline size_t pgoff2addr(pgoff_t offset)
{
	return offset << PAGE_SHIFT;
}

enum rdma_queue_type get_qp_type(int idx);
struct rswap_rdma_queue *
get_rdma_queue(struct rdma_session_context *rdma_session, unsigned int cpu,
	       enum rdma_queue_type type);
void get_rdma_queue_cpu_type(struct rdma_session_context *rdma_session,
			     struct rswap_rdma_queue *rdma_queue,
			     unsigned int *cpu, enum rdma_queue_type *type);

int init_rdma_sessions(struct rdma_session_context *rdma_session);
int rdma_session_connect(struct rdma_session_context *rdma_session);
int rswap_init_rdma_queue(struct rdma_session_context *rdma_session, int cpu);
int rswap_create_rdma_queue(struct rdma_session_context *rdma_session,
			    int rdma_queue_index);
int rswap_connect_remote_memory_server(
	struct rdma_session_context *rdma_session, int rdma_queue_inx);
int rswap_query_available_memory(struct rdma_session_context *rdma_session);
int setup_rdma_session_comm_buffer(struct rdma_session_context *rdma_session);
int rswap_setup_buffers(struct rdma_session_context *rdma_session);

int init_remote_chunk_list(struct rdma_session_context *rdma_session);
void bind_remote_memory_chunks(struct rdma_session_context *rdma_session);

int rswap_disconnect_and_collect_resource(
	struct rdma_session_context *rdma_session);
void rswap_free_buffers(struct rdma_session_context *rdma_session);
void rswap_free_rdma_structure(struct rdma_session_context *rdma_session);

void two_sided_message_done(struct ib_cq *cq, struct ib_wc *wc);
int handle_recv_wr(struct rswap_rdma_queue *rdma_queue, struct ib_wc *wc);
int send_message_to_remote(struct rdma_session_context *rdma_session,
			   int rdma_queue_ind, int messge_type, int chunk_num);

int rswap_rdma_send(int cpu, pgoff_t offset, struct page *page,
		    enum rdma_queue_type type);
int rswap_rdma_send_note(int cpu, pgoff_t offset, struct page *page,
			 enum rdma_queue_type type, int no_wait_pkts);

void drain_rdma_queue(struct rswap_rdma_queue *rdma_queue);
void drain_rdma_queue_unblock(struct rswap_rdma_queue *rdma_queue);
void drain_all_rdma_queues(int target_mem_server);

char *rdma_message_print(int message_id);
char *rdma_session_context_state_print(int id);
char *rdma_cm_message_print(int cm_message_id);
char *rdma_wc_status_name(int wc_status_id);

void print_scatterlist_info(struct scatterlist *sl_ptr, int nents);

void print_critical_macros(void);

extern struct rdma_session_context rdma_session_global;

extern int online_cores;
extern int num_queues;
extern char *server_ip;
extern uint16_t server_port;

extern u64 rmda_ops_count;
extern u64 cq_notify_count;
extern u64 cq_get_count;

#ifdef DEBUG_LATENCY_CLIENT
extern u64 *total_cycles;
extern u64 *last_cycles;
#endif

#endif
