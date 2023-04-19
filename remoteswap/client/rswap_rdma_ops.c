#include <linux/swap_stats.h>

#include "rswap_rdma.h"
#include "rswap_scheduler.h"

void drain_rdma_queue(struct rswap_rdma_queue *rdma_queue)
{
	unsigned long flags;

	while (atomic_read(&rdma_queue->rdma_post_counter) > 0) {
		spin_lock_irqsave(&rdma_queue->cq_lock, flags);
		ib_process_cq_direct(rdma_queue->cq, 16);
		spin_unlock_irqrestore(&rdma_queue->cq_lock, flags);
	}

	return;
}

void drain_rdma_queue_unblock(struct rswap_rdma_queue *rdma_queue)
{
	unsigned long flags;

	while (atomic_read(&rdma_queue->rdma_post_counter) > 0) {
		spin_lock_irqsave(&rdma_queue->cq_lock, flags);
		ib_process_cq_direct(rdma_queue->cq, 16);
		spin_unlock_irqrestore(&rdma_queue->cq_lock, flags);
		cond_resched();
	}

	return;
}

void drain_all_rdma_queues(int target_mem_server)
{
	int i;
	struct rdma_session_context *rdma_session = &rdma_session_global;

	for (i = 0; i < num_queues; i++) {
		drain_rdma_queue(&(rdma_session->rdma_queues[i]));
	}
}

void fs_rdma_write_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct fs_rdma_req *rdma_req = container_of(wc->wr_cqe, struct fs_rdma_req, cqe);
	struct rswap_rdma_queue *rdma_queue = cq->cq_context;
	struct ib_device *ibdev = rdma_queue->rdma_session->rdma_dev->dev;
#ifdef ENABLE_VQUEUE
	int cpu;
	enum rdma_queue_type type;
	struct rswap_proc *proc;
#ifdef LATENCY_THRESHOLD
	int time_cnt = 0;
	int total_time = 0;
	int once_time = 0;
#endif
#endif

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_err("%s status is not success, it is=%d\n", __func__, wc->status);
	}
	ib_dma_unmap_page(ibdev, rdma_req->dma_addr, PAGE_SIZE, DMA_TO_DEVICE);

	atomic_dec(&rdma_queue->rdma_post_counter);
	complete(&rdma_req->done);

#ifdef ENABLE_VQUEUE
#ifdef LATENCY_THRESHOLD
	total_time = atomic_read(&global_rswap_scheduler->total_pkts);
	time_cnt = total_time % 10000;
	total_time = total_time / 10000;
	if (time_cnt < 9999) {
		once_time = (get_cycles_end() - rdma_req->sent_time_start) / 1000;
		total_time = (once_time + total_time) * 10000 + time_cnt + 1;
		atomic_set(&global_rswap_scheduler->total_pkts, total_time);
	}
#endif
	if (!(rdma_req->no_wait_pkts)) {
		get_rdma_queue_cpu_type(&rdma_session_global, rdma_queue, &cpu, &type);
		proc = rswap_vqlist_get_triple(cpu)->proc;
		rswap_proc_send_pkts_dec(proc, type);
	}
#endif
	kmem_cache_free(rdma_queue->fs_rdma_req_cache, rdma_req);
}

void fs_rdma_read_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct fs_rdma_req *rdma_req = container_of(wc->wr_cqe, struct fs_rdma_req, cqe);
	struct rswap_rdma_queue *rdma_queue = cq->cq_context;
	struct ib_device *ibdev = rdma_queue->rdma_session->rdma_dev->dev;
#ifdef ENABLE_VQUEUE
	int cpu;
	enum rdma_queue_type type;
	struct rswap_proc *proc;
#ifdef LATENCY_THRESHOLD
	int time_cnt = 0;
	int total_time = 0;
	int once_time = 0;
#endif
#endif

	if (unlikely(wc->status != IB_WC_SUCCESS)) {
		pr_err("%s status is not success, it is=%d\n", __func__, wc->status);
	}
	ib_dma_unmap_page(ibdev, rdma_req->dma_addr, PAGE_SIZE, DMA_FROM_DEVICE);

	SetPageUptodate(rdma_req->page);
	unlock_page(rdma_req->page);
	atomic_dec(&rdma_queue->rdma_post_counter);
	complete(&rdma_req->done);

#ifdef ENABLE_VQUEUE
#ifdef LATENCY_THRESHOLD
	total_time = atomic_read(&global_rswap_scheduler->total_pkts);
	time_cnt = total_time % 10000;
	total_time = total_time / 10000;
	if (time_cnt < 9999) {
		once_time = (get_cycles_end() - rdma_req->sent_time_start) / 1000;
		total_time = (once_time + total_time) * 10000 + time_cnt + 1;
		atomic_set(&global_rswap_scheduler->total_pkts, total_time);
	}
#endif
	if (!(rdma_req->no_wait_pkts)) {
		get_rdma_queue_cpu_type(&rdma_session_global, rdma_queue, &cpu, &type);
		proc = rswap_vqlist_get_triple(cpu)->proc;
		rswap_proc_send_pkts_dec(proc, type);
	}
#endif
	kmem_cache_free(rdma_queue->fs_rdma_req_cache, rdma_req);
}

int fs_enqueue_send_wr(struct rdma_session_context *rdma_session, struct rswap_rdma_queue *rdma_queue,
		       struct fs_rdma_req *rdma_req)
{
	int ret = 0;
	const struct ib_send_wr *bad_wr;
	int test;

	rdma_req->rdma_queue = rdma_queue;

	while (1) {
		test = atomic_inc_return(&rdma_queue->rdma_post_counter);
		if (test < RDMA_SEND_QUEUE_DEPTH - 16) {
			ret = ib_post_send(rdma_queue->qp, (struct ib_send_wr *)&rdma_req->rdma_wr, &bad_wr);
			if (unlikely(ret)) {
				pr_err("%s, post 1-sided RDMA send wr failed, "
				       "return value :%d. counter %d \n",
				       __func__, ret, test);
				ret = -1;
				goto err;
			}

			return ret;
		} else {
			test = atomic_dec_return(&rdma_queue->rdma_post_counter);
			drain_rdma_queue(rdma_queue);
		}
	}
err:
	pr_err(" Error in %s \n", __func__);
	return -1;
}

int fs_build_rdma_wr(struct rdma_session_context *rdma_session, struct rswap_rdma_queue *rdma_queue,
		     struct fs_rdma_req *rdma_req, struct remote_chunk *remote_chunk_ptr, size_t offset_within_chunk,
		     struct page *page, enum rdma_queue_type type)
{
	int ret = 0;
	enum dma_data_direction dir;
	struct ib_device *dev = rdma_session->rdma_dev->dev;

	rdma_req->page = page;
	init_completion(&(rdma_req->done));

	dir = type == QP_STORE ? DMA_TO_DEVICE : DMA_FROM_DEVICE;
	rdma_req->dma_addr = ib_dma_map_page(dev, page, 0, PAGE_SIZE, dir);
	if (unlikely(ib_dma_mapping_error(dev, rdma_req->dma_addr))) {
		pr_err("%s, ib_dma_mapping_error\n", __func__);
		ret = -ENOMEM;
		kmem_cache_free(rdma_queue->fs_rdma_req_cache, rdma_req);
		goto out;
	}

	ib_dma_sync_single_for_device(dev, rdma_req->dma_addr, PAGE_SIZE, dir);

	rdma_req->cqe.done = type == QP_STORE ? fs_rdma_write_done : fs_rdma_read_done;

	rdma_req->sge.addr = rdma_req->dma_addr;
	rdma_req->sge.length = PAGE_SIZE;
	rdma_req->sge.lkey = rdma_session->rdma_dev->pd->local_dma_lkey;

	rdma_req->rdma_wr.wr.next = NULL;
	rdma_req->rdma_wr.wr.wr_cqe = &rdma_req->cqe;
	rdma_req->rdma_wr.wr.sg_list = &(rdma_req->sge);
	rdma_req->rdma_wr.wr.num_sge = 1;
	rdma_req->rdma_wr.wr.opcode = (dir == DMA_TO_DEVICE ? IB_WR_RDMA_WRITE : IB_WR_RDMA_READ);
	rdma_req->rdma_wr.wr.send_flags = IB_SEND_SIGNALED;
	rdma_req->rdma_wr.remote_addr = remote_chunk_ptr->remote_addr + offset_within_chunk;
	rdma_req->rdma_wr.rkey = remote_chunk_ptr->remote_rkey;
	rdma_req->no_wait_pkts = 1;
#ifdef LATENCY_THRESHOLD
	rdma_req->sent_time_start = 0;
#endif
out:
	return ret;
}

int rswap_rdma_send_note(int cpu, pgoff_t offset, struct page *page, enum rdma_queue_type type, int no_wait_pkts)
{
	int ret = 0;
	size_t page_addr;
	size_t chunk_idx;
	size_t offset_within_chunk;
	struct rswap_rdma_queue *rdma_queue;
	struct fs_rdma_req *rdma_req;
	struct remote_chunk *remote_chunk_ptr;

	page_addr = pgoff2addr(offset);
	chunk_idx = page_addr >> CHUNK_SHIFT;
	offset_within_chunk = page_addr & CHUNK_MASK;

	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, type);
	rdma_req = (struct fs_rdma_req *)kmem_cache_alloc(rdma_queue->fs_rdma_req_cache, GFP_ATOMIC);
	if (!rdma_req) {
		pr_err("%s, get reserved fs_rdma_req failed. \n", __func__);
		goto out;
	}

	remote_chunk_ptr = &(rdma_session_global.remote_mem_pool.chunks[chunk_idx]);

	ret = fs_build_rdma_wr(&rdma_session_global, rdma_queue, rdma_req, remote_chunk_ptr, offset_within_chunk, page,
			       type);
	if (unlikely(ret)) {
		pr_err("%s, build rdma_wr failed.\n", __func__);
		goto out;
	}
	rdma_req->no_wait_pkts = no_wait_pkts;
#ifdef LATENCY_THRESHOLD
	rdma_req->sent_time_start = get_cycles_start();
#endif

	ret = fs_enqueue_send_wr(&rdma_session_global, rdma_queue, rdma_req);
	if (unlikely(ret)) {
		pr_err("%s, enqueue rdma_wr failed.\n", __func__);
		goto out;
	}

out:
	return ret;
}

static inline pgoff_t local_to_remote_page_mapping(unsigned type, pgoff_t swap_entry_offset)
{
#ifndef RSWAP_KERNEL_SUPPORT
	return swap_entry_offset;
#else
	if (!swap_isolated())
		return swap_entry_offset;
	else
		return swap_partition_global_entry_offset(type) + swap_entry_offset;
#endif
}

int rswap_frontswap_store(unsigned type, pgoff_t swap_entry_offset, struct page *page)
{
	pgoff_t remote_page_offset = local_to_remote_page_mapping(type, swap_entry_offset);

#ifdef ENABLE_VQUEUE
	int ret = 0;
	int cpu = -1;
	int sent_vqueue = 0;
	struct rswap_rdma_queue *rdma_queue;
	struct rswap_vqueue *vqueue;
	struct rswap_request vrequest = { remote_page_offset, page };

	cpu = get_cpu();
	vqueue = rswap_vqlist_get(cpu, QP_STORE);

	if (atomic_read(&vqueue->send_direct)) {
		ret = rswap_rdma_send_note(cpu, remote_page_offset, page, QP_STORE, 1);
	} else {
		ret = rswap_vqueue_enqueue(vqueue, &vrequest);
		sent_vqueue = 1;
	}
	atomic_inc(&global_rswap_scheduler->total_pkts);

	put_cpu();
	if (unlikely(ret) != 0) {
		print_err(ret);
		goto out;
	}

	if (sent_vqueue) {
		rswap_vqueue_drain(cpu, QP_STORE);
	}
	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_STORE);
	drain_rdma_queue(rdma_queue);
#else
	int ret = 0;
	int cpu;
	struct rswap_rdma_queue *rdma_queue;

	cpu = get_cpu();
	ret = rswap_rdma_send_note(cpu, remote_page_offset, page, QP_STORE, 1);
	put_cpu();
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n", __func__);
		goto out;
	}

	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_STORE);
	drain_rdma_queue(rdma_queue);
	ret = 0;
#endif

out:
	return ret;
}

int rswap_frontswap_load(unsigned type, pgoff_t swap_entry_offset, struct page *page)
{
	pgoff_t remote_page_offset = local_to_remote_page_mapping(type, swap_entry_offset);

#ifdef ENABLE_VQUEUE
	int ret = 0;
	int cpu = -1;
	struct rswap_vqueue *vqueue;
	struct rswap_request vrequest = { remote_page_offset, page };

	cpu = smp_processor_id();

	vqueue = rswap_vqlist_get(cpu, QP_LOAD_SYNC);
	if (atomic_read(&vqueue->send_direct)) {
		ret = rswap_rdma_send_note(cpu, remote_page_offset, page, QP_LOAD_SYNC, 1);
	} else {
		ret = rswap_vqueue_enqueue(vqueue, &vrequest);
	}
	atomic_inc(&global_rswap_scheduler->total_pkts);

	if (ret != 0) {
		print_err(ret);
		goto out;
	}
#else
	int ret = 0;
	int cpu;

	cpu = smp_processor_id();

	ret = rswap_rdma_send_note(cpu, remote_page_offset, page, QP_LOAD_SYNC, 1);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n", __func__);
		goto out;
	}
	ret = 0;
#endif

out:
	return ret;
}

int rswap_frontswap_load_async(unsigned type, pgoff_t swap_entry_offset, struct page *page)
{
	pgoff_t remote_page_offset = local_to_remote_page_mapping(type, swap_entry_offset);

#ifdef ENABLE_VQUEUE
	int ret = 0;
	int cpu = -1;
	struct rswap_vqueue *vqueue;
	struct rswap_request vrequest = { remote_page_offset, page };

	cpu = smp_processor_id();

	vqueue = rswap_vqlist_get(cpu, QP_LOAD_ASYNC);
	if (atomic_read(&vqueue->send_direct)) {
		ret = rswap_rdma_send_note(cpu, remote_page_offset, page, QP_LOAD_ASYNC, 1);
	} else {
		ret = rswap_vqueue_enqueue(vqueue, &vrequest);
	}
	atomic_inc(&global_rswap_scheduler->total_pkts);

	if (ret != 0) {
		print_err(ret);
		goto out;
	}

#else
	int ret = 0;
	int cpu = smp_processor_id();

	ret = rswap_rdma_send_note(cpu, remote_page_offset, page, QP_LOAD_ASYNC, 1);
	if (unlikely(ret)) {
		pr_err("%s, enqueuing rdma frontswap write failed.\n", __func__);
		goto out;
	}
	ret = 0;

#endif

out:
	return ret;
}

int rswap_frontswap_poll_load(int cpu)
{
#ifdef ENABLE_VQUEUE
	struct rswap_rdma_queue *rdma_queue;
	struct rswap_vqueue *vqueue;

	vqueue = rswap_vqlist_get(cpu, QP_LOAD_SYNC);
	if (atomic_read(&vqueue->cnt) > 0)
		rswap_vqueue_drain(cpu, QP_LOAD_SYNC);
	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_LOAD_SYNC);
	drain_rdma_queue(rdma_queue);
#else
	struct rswap_rdma_queue *rdma_queue;

	rdma_queue = get_rdma_queue(&rdma_session_global, cpu, QP_LOAD_SYNC);
	drain_rdma_queue(rdma_queue);
#endif
	return 0;
}

static void rswap_invalidate_page(unsigned type, pgoff_t offset)
{
	return;
}

static void rswap_invalidate_area(unsigned type)
{
	return;
}

static void rswap_frontswap_init(unsigned type)
{
}

static struct frontswap_ops rswap_frontswap_ops = {
	.init = rswap_frontswap_init,
	.store = rswap_frontswap_store,
	.load = rswap_frontswap_load,
	.load_async = rswap_frontswap_load_async,
	.poll_load = rswap_frontswap_poll_load,
	.invalidate_page = rswap_invalidate_page,
	.invalidate_area = rswap_invalidate_area,
};

int rswap_register_frontswap(void)
{
	int ret = 0;
	frontswap_register_ops(&rswap_frontswap_ops);

	pr_info("frontswap module loaded\n");
	return ret;
}

int rswap_replace_frontswap(void)
{
#ifdef RSWAP_KERNEL_SUPPORT
	frontswap_ops->init = rswap_frontswap_ops.init;
	frontswap_ops->store = rswap_frontswap_ops.store;
	frontswap_ops->load = rswap_frontswap_ops.load;
	frontswap_ops->load_async = rswap_frontswap_ops.load_async;
	frontswap_ops->poll_load = rswap_frontswap_ops.poll_load;
#else
	frontswap_ops->init = rswap_frontswap_ops.init;
	frontswap_ops->store = rswap_frontswap_ops.store;
	frontswap_ops->load = rswap_frontswap_ops.load;
	frontswap_ops->poll_load = rswap_frontswap_ops.poll_load;
#endif
	pr_info("frontswap ops replaced\n");
	return 0;
}

int rswap_client_init(char *_server_ip, int _server_port, int _mem_size)
{
	int ret = 0;
	pr_info("%s, start \n", __func__);

	online_cores = num_online_cpus();
	num_queues = online_cores * NUM_QP_TYPE;
	server_ip = _server_ip;
	server_port = _server_port;
	rdma_session_global.remote_mem_pool.remote_mem_size = _mem_size;
	rdma_session_global.remote_mem_pool.chunk_num = _mem_size / REGION_SIZE_GB;

	pr_info("%s, num_queues : %d (Can't exceed the slots on Memory server) \n", __func__, num_queues);

	ret = init_rdma_sessions(&rdma_session_global);

	ret = rdma_session_connect(&rdma_session_global);
	if (unlikely(ret)) {
		pr_err("%s, rdma_session_connect failed. \n", __func__);
		goto out;
	}

#ifdef ENABLE_VQUEUE
	ret = rswap_scheduler_init();
	if (ret) {
		pr_err("%s, rswap_scheduler_init failed. \n", __func__);
		goto out;
	}
#endif
out:
	return ret;
}

void rswap_client_exit(void)
{
	int ret;
	ret = rswap_disconnect_and_collect_resource(&rdma_session_global);
	if (unlikely(ret)) {
		pr_err("%s,  failed.\n", __func__);
	}
	pr_info("%s done.\n", __func__);
#ifdef ENABLE_VQUEUE
	rswap_scheduler_stop();
#endif // ENABLE_VQUEUE
}
