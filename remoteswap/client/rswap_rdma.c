#include "rswap_rdma.h"

struct rdma_session_context rdma_session_global;
int online_cores; // Control the parallelism
int num_queues; // Total #(queues)

char *server_ip; // the memory server ip
uint16_t server_port; // the memory server port

u64 rmda_ops_count = 0;
u64 cq_notify_count = 0;
u64 cq_get_count = 0;

inline enum rdma_queue_type get_qp_type(int idx)
{
	unsigned type = idx / online_cores;

	if (type < NUM_QP_TYPE) {
		return type;
	} else {
		pr_err("wrong rdma queue type %d\n", idx / online_cores);
		return QP_STORE;
	}

	return QP_STORE;
}

inline struct rswap_rdma_queue *
get_rdma_queue(struct rdma_session_context *rdma_session, unsigned int cpu,
	       enum rdma_queue_type type)
{
	if (type < NUM_QP_TYPE) {
		return &(rdma_session->rdma_queues[cpu + online_cores * type]);
	} else {
		BUG();
	}
	return &(rdma_session->rdma_queues[cpu]);
}

inline void get_rdma_queue_cpu_type(struct rdma_session_context *rdma_session,
				    struct rswap_rdma_queue *rdma_queue,
				    unsigned int *cpu,
				    enum rdma_queue_type *type)
{
	struct rswap_rdma_queue *start = rdma_session->rdma_queues;
	int qid = rdma_queue - start;
	*cpu = qid % online_cores;
	*type = qid / online_cores;
}

int handle_recv_wr(struct rswap_rdma_queue *rdma_queue, struct ib_wc *wc)
{
	int ret = 0;
	struct rdma_session_context *rdma_session = rdma_queue->rdma_session;

	if (wc->byte_len != sizeof(struct message)) {
		pr_err("%s, Received bogus data, size %d\n", __func__,
		       wc->byte_len);
		ret = -1;
		goto out;
	}

	if (unlikely(rdma_queue->state < CONNECTED)) {
		pr_debug("%s, RDMA is not connected\n", __func__);
		return -1;
	}

	switch (rdma_session->rdma_recv_req.recv_buf->type) {
	case AVAILABLE_TO_QUERY:
		pr_debug(
			"%s, Received AVAILABLE_TO_QUERY, memory server is prepared well. We can query\n ",
			__func__);
		rdma_queue->state = MEMORY_SERVER_AVAILABLE;

		wake_up_interruptible(&rdma_queue->sem);
		break;
	case FREE_SIZE:
		pr_info("%s, Received FREE_SIZE, avaible chunk number : %d \n ",
			__func__,
			rdma_session->rdma_recv_req.recv_buf->mapped_chunk);

		rdma_session->remote_mem_pool.chunk_num =
			rdma_session->rdma_recv_req.recv_buf->mapped_chunk;
		rdma_queue->state = FREE_MEM_RECV;

		ret = init_remote_chunk_list(rdma_session);
		if (unlikely(ret)) {
			pr_err("Initialize the remote chunk failed. \n");
		}

		wake_up_interruptible(&rdma_queue->sem);
		break;
	case GOT_CHUNKS:
		pr_info("%s, got %u chunks from remote memory, bind "
			"them to CPU server.\n",
			__func__, rdma_session->remote_mem_pool.chunk_num);
		bind_remote_memory_chunks(rdma_session);

		rdma_queue->state = RECEIVED_CHUNKS;
		wake_up_interruptible(&rdma_queue->sem);

		break;
	default:
		pr_err("%s, Recieved WRONG RDMA message %d \n", __func__,
		       rdma_session->rdma_recv_req.recv_buf->type);
		ret = -1;
		goto out;
	}

out:
	return ret;
}

void two_sided_message_done(struct ib_cq *cq, struct ib_wc *wc)
{
	struct rswap_rdma_queue *rdma_queue = cq->cq_context;
	int ret = 0;

	if (wc->status != IB_WC_SUCCESS) {
		pr_err("%s, cq completion failed with wr_id 0x%llx "
		       "status %d,  status name %s, opcode %d,\n",
		       __func__, wc->wr_id, wc->status,
		       rdma_wc_status_name(wc->status), wc->opcode);
		goto out;
	}

	switch (wc->opcode) {
	case IB_WC_RECV:
		pr_debug("%s, Got a WC from CQ, IB_WC_RECV. \n", __func__);
		ret = handle_recv_wr(rdma_queue, wc);
		if (unlikely(ret)) {
			pr_err("%s, recv wc error: %d\n", __func__, ret);
			goto out;
		}
		break;
	case IB_WC_SEND:
		pr_debug("%s, Got a WC from CQ, IB_WC_SEND. 2-sided "
			 "RDMA post done. \n",
			 __func__);
		break;
	default:
		pr_err("%s:%d Unexpected opcode %d, Shutting down\n", __func__,
		       __LINE__, wc->opcode);
		goto out;
	}

	atomic_dec(&rdma_queue->rdma_post_counter);
out:
	return;
}

int send_message_to_remote(struct rdma_session_context *rdma_session,
			   int rdma_queue_ind, int message_type, int chunk_num)
{
	int ret = 0;
	const struct ib_recv_wr *recv_bad_wr;
	const struct ib_send_wr *send_bad_wr;
	struct rswap_rdma_queue *rdma_queue;

	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_ind]);
	rdma_session->rdma_send_req.send_buf->type = message_type;
	rdma_session->rdma_send_req.send_buf->mapped_chunk = chunk_num;

	// post a 2-sided RDMA recv wr first.
	ret = ib_post_recv(rdma_queue->qp, &rdma_session->rdma_recv_req.rq_wr,
			   &recv_bad_wr);
	if (ret) {
		pr_err("%s, Post 2-sided message to receive data failed.\n",
		       __func__);
		goto err;
	}
	atomic_inc(&rdma_queue->rdma_post_counter);

	pr_debug("Send a Message to memory server. send_buf->type : %d, %s \n",
		 message_type, rdma_message_print(message_type));
	ret = ib_post_send(rdma_queue->qp, &rdma_session->rdma_send_req.sq_wr,
			   &send_bad_wr);
	if (ret) {
		pr_err("%s: BIND_SINGLE MSG send error %d\n", __func__, ret);
		goto err;
	}
	atomic_inc(&rdma_queue->rdma_post_counter);

err:
	return ret;
}

int rswap_query_available_memory(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	struct rswap_rdma_queue *rdma_queue;

	rdma_queue = &(rdma_session->rdma_queues[num_queues - 1]);
	wait_event_interruptible(rdma_queue->sem,
				 rdma_queue->state == MEMORY_SERVER_AVAILABLE);
	pr_info("%s, All %d rdma queues are prepared well. Query its available memory. \n",
		__func__, num_queues);
	rdma_queue = &(rdma_session->rdma_queues[0]);
	ret = send_message_to_remote(rdma_session, 0, QUERY, 0);
	if (ret) {
		pr_err("%s, Post 2-sided message to remote server failed.\n",
		       __func__);
		goto err;
	}

	drain_rdma_queue(rdma_queue);
	wait_event_interruptible(rdma_queue->sem,
				 rdma_queue->state == FREE_MEM_RECV);

	pr_info("%s: Got %d free memory chunks from remote memory server. Request for Chunks \n",
		__func__, rdma_session->remote_mem_pool.chunk_num);

err:
	return ret;
}

int rswap_request_for_chunk(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	struct rswap_rdma_queue *rdma_queue;
	int chunk_num = rdma_session->remote_mem_pool.chunk_num;

	rdma_queue = &(rdma_session->rdma_queues[0]);
	if (chunk_num == 0 || rdma_session == NULL) {
		pr_err("%s, current memory server has no available memory at all. Exit. \n",
		       __func__);
		goto err;
	}

	ret = send_message_to_remote(rdma_session, 0, REQUEST_CHUNKS,
				     chunk_num);
	if (ret) {
		pr_err("%s, Post 2-sided message to remote server failed.\n",
		       __func__);
		goto err;
	}

	drain_rdma_queue(rdma_queue);
	wait_event_interruptible(rdma_queue->sem,
				 rdma_queue->state == RECEIVED_CHUNKS);

	pr_info("%s, Got %d chunks from memory server.\n", __func__, chunk_num);
err:
	return ret;
}

int rswap_rdma_cm_event_handler(struct rdma_cm_id *cma_id,
				struct rdma_cm_event *event)
{
	int ret = 0;
	struct rswap_rdma_queue *rdma_queue = cma_id->context;

	pr_debug("cma_event type %d, type_name: %s \n", event->event,
		 rdma_cm_message_print(event->event));
	switch (event->event) {
	case RDMA_CM_EVENT_ADDR_RESOLVED:
		rdma_queue->state = ADDR_RESOLVED;
		pr_debug(
			"%s,  get RDMA_CM_EVENT_ADDR_RESOLVED. Send RDMA_ROUTE_RESOLVE to Memory server \n",
			__func__);

		ret = rdma_resolve_route(cma_id, 2000);
		if (ret) {
			pr_err("%s,rdma_resolve_route error %d\n", __func__,
			       ret);
		}
		break;
	case RDMA_CM_EVENT_ROUTE_RESOLVED:
		pr_debug(
			"%s : RDMA_CM_EVENT_ROUTE_RESOLVED, wake up rdma_queue->sem\n ",
			__func__);

		rdma_queue->state = ROUTE_RESOLVED;
		wake_up_interruptible(&rdma_queue->sem);
		break;
	case RDMA_CM_EVENT_CONNECT_REQUEST:
		pr_info("Receive but Not Handle : RDMA_CM_EVENT_CONNECT_REQUEST \n");
		break;
	case RDMA_CM_EVENT_ESTABLISHED:
		pr_debug("%s, ESTABLISHED, wake up rdma_queue->sem\n",
			 __func__);
		rdma_queue->state = CONNECTED;
		wake_up_interruptible(&rdma_queue->sem);
		break;
	case RDMA_CM_EVENT_ADDR_ERROR:
	case RDMA_CM_EVENT_ROUTE_ERROR:
	case RDMA_CM_EVENT_CONNECT_ERROR:
	case RDMA_CM_EVENT_UNREACHABLE:
	case RDMA_CM_EVENT_REJECTED:
		pr_err("%s, cma event %d, event name %s, error code %d \n",
		       __func__, event->event,
		       rdma_cm_message_print(event->event), event->status);
		rdma_queue->state = ERROR;
		wake_up_interruptible(&rdma_queue->sem);
		break;
	case RDMA_CM_EVENT_DISCONNECTED:
		pr_info("%s, Receive DISCONNECTED  signal \n", __func__);

		if (rdma_queue->freed) {
			pr_debug(
				"%s, RDMA disconnect evetn, requested by client. \n",
				__func__);
		} else {
			pr_debug(
				"%s, RDMA disconnect evetn, requested by client. \n",
				__func__);
			rdma_disconnect(rdma_queue->cm_id);
		}
		break;
	case RDMA_CM_EVENT_TIMEWAIT_EXIT:
		pr_info("%s, Wait for in-the-fly RDMA message finished. \n",
			__func__);
		rdma_queue->state = CM_DISCONNECT;

		wake_up_interruptible(&rdma_queue->sem);
		break;
	case RDMA_CM_EVENT_DEVICE_REMOVAL:
		pr_err("%s, cma detected device removal!!!!\n", __func__);
		return -1;
		break;
	default:
		pr_err("%s,oof bad type!\n", __func__);
		wake_up_interruptible(&rdma_queue->sem);
		break;
	}
	return ret;
}

static int
rdma_resolve_ip_to_ib_device(struct rdma_session_context *rdma_session,
			     struct rswap_rdma_queue *rdma_queue)
{
	int ret;
	struct sockaddr_storage sin;
	struct sockaddr_in *sin4 = (struct sockaddr_in *)&sin;

	sin4->sin_family = AF_INET;
	memcpy((void *)&(sin4->sin_addr.s_addr), rdma_session->addr, 4);
	sin4->sin_port = rdma_session->port;

	ret = rdma_resolve_addr(rdma_queue->cm_id, NULL,
				(struct sockaddr *)&sin, 2000);
	if (ret) {
		pr_err("%s, rdma_resolve_ip_to_ib_device error %d\n", __func__,
		       ret);
		return ret;
	} else {
		pr_info("rdma_resolve_ip_to_ib_device - rdma_resolve_addr success.\n");
	}

	wait_event_interruptible(rdma_queue->sem,
				 rdma_queue->state >= ROUTE_RESOLVED);
	if (rdma_queue->state != ROUTE_RESOLVED) {
		pr_err("%s, addr/route resolution did not resolve: state %d\n",
		       __func__, rdma_queue->state);
		return -EINTR;
	}

	pr_info("%s, resolve address and route successfully\n", __func__);
	return ret;
}

int rswap_create_qp(struct rdma_session_context *rdma_session,
		    struct rswap_rdma_queue *rdma_queue)
{
	struct ib_qp_init_attr init_attr;
	int ret;

	memset(&init_attr, 0, sizeof(init_attr));
	init_attr.cap.max_send_wr = rdma_session->send_queue_depth;
	init_attr.cap.max_recv_wr = rdma_session->recv_queue_depth;
	init_attr.cap.max_recv_sge = MAX_REQUEST_SGL;
	init_attr.cap.max_send_sge = MAX_REQUEST_SGL;
	init_attr.sq_sig_type = IB_SIGNAL_REQ_WR;
	init_attr.qp_type = IB_QPT_RC;
	init_attr.send_cq = rdma_queue->cq;
	init_attr.recv_cq = rdma_queue->cq;

	ret = rdma_create_qp(rdma_queue->cm_id, rdma_session->rdma_dev->pd,
			     &init_attr);
	if (!ret) {
		rdma_queue->qp = rdma_queue->cm_id->qp;
	} else {
		pr_err("%s:  Create QP falied. errno : %d \n", __func__, ret);
	}

	return ret;
}

int rswap_create_rdma_queue(struct rdma_session_context *rdma_session,
			    int rdma_queue_index)
{
	int ret = 0;
	struct rdma_cm_id *cm_id;
	struct rswap_rdma_queue *rdma_queue;
	int cq_num_cqes;
	int comp_vector = 0;

	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_index]);
	cm_id = rdma_queue->cm_id;
	if (rdma_session->rdma_dev == NULL) {
		rdma_session->rdma_dev =
			kzalloc(sizeof(struct rswap_rdma_dev), GFP_KERNEL);

		rdma_session->rdma_dev->pd = ib_alloc_pd(
			cm_id->device, IB_ACCESS_LOCAL_WRITE |
					       IB_ACCESS_REMOTE_READ |
					       IB_ACCESS_REMOTE_WRITE);
		rdma_session->rdma_dev->dev =
			rdma_session->rdma_dev->pd->device;

		if (IS_ERR(rdma_session->rdma_dev->pd)) {
			pr_err("%s, ib_alloc_pd failed\n", __func__);
			goto err;
		}
		pr_info("%s, created pd %p\n", __func__,
			rdma_session->rdma_dev->pd);
		setup_rdma_session_comm_buffer(rdma_session);
	}

	cq_num_cqes =
		rdma_session->send_queue_depth + rdma_session->recv_queue_depth;
	if (rdma_queue->type == QP_LOAD_ASYNC) {
		rdma_queue->cq =
			ib_alloc_cq(cm_id->device, rdma_queue, cq_num_cqes,
				    comp_vector, IB_POLL_SOFTIRQ);
	} else {
		rdma_queue->cq =
			ib_alloc_cq(cm_id->device, rdma_queue, cq_num_cqes,
				    comp_vector, IB_POLL_DIRECT);
	}

	if (IS_ERR(rdma_queue->cq)) {
		pr_err("%s, ib_create_cq failed\n", __func__);
		ret = PTR_ERR(rdma_queue->cq);
		goto err;
	}
	pr_debug("%s, created cq %p\n", __func__, rdma_queue->cq);

	ret = rswap_create_qp(rdma_session, rdma_queue);
	if (ret) {
		pr_err("%s, failed: %d\n", __func__, ret);
		goto err;
	}
	pr_debug("%s, created qp %p\n", __func__, rdma_queue->qp);

err:
	return ret;
}

void rswap_setup_message_wr(struct rdma_session_context *rdma_session)
{
	rdma_session->rdma_recv_req.recv_sgl.addr =
		rdma_session->rdma_recv_req.recv_dma_addr;
	rdma_session->rdma_recv_req.recv_sgl.length = sizeof(struct message);
	rdma_session->rdma_recv_req.recv_sgl.lkey =
		rdma_session->rdma_dev->dev->local_dma_lkey;

	rdma_session->rdma_recv_req.rq_wr.sg_list =
		&(rdma_session->rdma_recv_req.recv_sgl);
	rdma_session->rdma_recv_req.rq_wr.num_sge = 1;
	rdma_session->rdma_recv_req.cqe.done = two_sided_message_done;
	rdma_session->rdma_recv_req.rq_wr.wr_cqe =
		&(rdma_session->rdma_recv_req.cqe);

	rdma_session->rdma_send_req.send_sgl.addr =
		rdma_session->rdma_send_req.send_dma_addr;
	rdma_session->rdma_send_req.send_sgl.length = sizeof(struct message);
	rdma_session->rdma_send_req.send_sgl.lkey =
		rdma_session->rdma_dev->dev->local_dma_lkey;

	rdma_session->rdma_send_req.sq_wr.opcode = IB_WR_SEND;
	rdma_session->rdma_send_req.sq_wr.send_flags = IB_SEND_SIGNALED;
	rdma_session->rdma_send_req.sq_wr.sg_list =
		&rdma_session->rdma_send_req.send_sgl;
	rdma_session->rdma_send_req.sq_wr.num_sge = 1;
	rdma_session->rdma_send_req.cqe.done = two_sided_message_done;
	rdma_session->rdma_send_req.sq_wr.wr_cqe =
		&(rdma_session->rdma_send_req.cqe);

	return;
}

int rswap_setup_buffers(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	rdma_session->rdma_recv_req.recv_buf =
		kzalloc(sizeof(struct message), GFP_KERNEL);
	rdma_session->rdma_send_req.send_buf =
		kzalloc(sizeof(struct message), GFP_KERNEL);

	rdma_session->rdma_recv_req.recv_dma_addr =
		ib_dma_map_single(rdma_session->rdma_dev->dev,
				  rdma_session->rdma_recv_req.recv_buf,
				  sizeof(struct message), DMA_BIDIRECTIONAL);
	rdma_session->rdma_send_req.send_dma_addr =
		ib_dma_map_single(rdma_session->rdma_dev->dev,
				  rdma_session->rdma_send_req.send_buf,
				  sizeof(struct message), DMA_BIDIRECTIONAL);

	pr_debug("%s, Got dma/bus address 0x%llx, for the recv_buf 0x%llx \n",
		 __func__,
		 (unsigned long long)rdma_session->rdma_recv_req.recv_dma_addr,
		 (unsigned long long)rdma_session->rdma_recv_req.recv_buf);
	pr_debug("%s, Got dma/bus address 0x%llx, for the send_buf 0x%llx \n",
		 __func__,
		 (unsigned long long)rdma_session->rdma_send_req.send_dma_addr,
		 (unsigned long long)rdma_session->rdma_send_req.send_buf);

	rswap_setup_message_wr(rdma_session);
	pr_debug("%s, allocated & registered buffers...\n", __func__);
	pr_debug("%s is done. \n", __func__);

	return ret;
}

int rswap_connect_remote_memory_server(
	struct rdma_session_context *rdma_session, int rdma_queue_inx)
{
	struct rdma_conn_param conn_param;
	int ret;
	struct rswap_rdma_queue *rdma_queue;
	const struct ib_recv_wr *bad_wr;

	rdma_queue = &(rdma_session->rdma_queues[rdma_queue_inx]);
	memset(&conn_param, 0, sizeof conn_param);
	conn_param.responder_resources = 1;
	conn_param.initiator_depth = 1;
	conn_param.retry_count = 10;

	atomic_inc(&rdma_queue->rdma_post_counter);
	ret = ib_post_recv(rdma_queue->qp, &rdma_session->rdma_recv_req.rq_wr,
			   &bad_wr);
	if (ret) {
		pr_err("%s: post a 2-sided RDMA message error \n", __func__);
		goto err;
	}

	ret = rdma_connect(rdma_queue->cm_id, &conn_param);
	if (ret) {
		pr_err("%s, rdma_connect error %d\n", __func__, ret);
		return ret;
	}
	pr_info("%s, Send RDMA connect request to remote server, "
		"queue index[%d] \n",
		__func__, rdma_queue_inx);

	wait_event_interruptible(rdma_queue->sem,
				 rdma_queue->state >= CONNECTED);
	if (rdma_queue->state == ERROR) {
		pr_err("%s, Received ERROR response, state %d\n", __func__,
		       rdma_queue->state);
		return -1;
	}

	drain_rdma_queue_unblock(rdma_queue);

	pr_info("%s, RDMA queue [%d] connect successful\n", __func__,
		rdma_queue_inx);

err:
	return ret;
}

int init_remote_chunk_list(struct rdma_session_context *rdma_session)
{
	int ret = 0;
	uint32_t i;

	rdma_session->remote_mem_pool.chunks = (struct remote_chunk *)kzalloc(
		sizeof(struct remote_chunk) *
			rdma_session->remote_mem_pool.chunk_num,
		GFP_KERNEL);

	for (i = 0; i < rdma_session->remote_mem_pool.chunk_num; i++) {
		rdma_session->remote_mem_pool.chunks[i].chunk_state = EMPTY;
		rdma_session->remote_mem_pool.chunks[i].remote_addr = 0x0;
		rdma_session->remote_mem_pool.chunks[i].mapped_size = 0x0;
		rdma_session->remote_mem_pool.chunks[i].remote_rkey = 0x0;
	}

	return ret;
}

void bind_remote_memory_chunks(struct rdma_session_context *rdma_session)
{
	int i;
	int chunk_num = rdma_session->remote_mem_pool.chunk_num;

	for (i = 0; i < chunk_num; i++) {
		if (rdma_session->rdma_recv_req.recv_buf->rkey[i]) {
			rdma_session->remote_mem_pool.chunks[i].remote_rkey =
				rdma_session->rdma_recv_req.recv_buf->rkey[i];
			rdma_session->remote_mem_pool.chunks[i].remote_addr =
				rdma_session->rdma_recv_req.recv_buf->buf[i];
			rdma_session->remote_mem_pool.chunks[i].mapped_size =
				rdma_session->rdma_recv_req.recv_buf
					->mapped_size[i];
			rdma_session->remote_mem_pool.chunks[i].chunk_state =
				MAPPED;

			pr_info("Got chunk[%d] : remote_addr : 0x%llx, "
				"remote_rkey: 0x%x, mapped_size: 0x%llx \n",
				i,
				rdma_session->remote_mem_pool.chunks[i]
					.remote_addr,
				rdma_session->remote_mem_pool.chunks[i]
					.remote_rkey,
				rdma_session->remote_mem_pool.chunks[i]
					.mapped_size);
		}
	}
}

int init_rdma_sessions(struct rdma_session_context *rdma_session)
{
	int ret = 0;

	rdma_session->rdma_queues = kzalloc(
		sizeof(struct rswap_rdma_queue) * num_queues, GFP_KERNEL);
	rdma_session->send_queue_depth = RDMA_SEND_QUEUE_DEPTH + 1;
	rdma_session->recv_queue_depth = RDMA_RECV_QUEUE_DEPTH + 1;

	rdma_session->port = htons(server_port);
	ret = in4_pton(server_ip, strlen(server_ip), rdma_session->addr, -1,
		       NULL);
	if (ret == 0) {
		pr_err("Assign ip %s to  rdma_session->addr : %s failed.\n",
		       server_ip, rdma_session->addr);
		goto err;
	}
	rdma_session->addr_type = AF_INET;

err:
	return ret;
}

int setup_rdma_session_comm_buffer(struct rdma_session_context *rdma_session)
{
	int ret = 0;

	if (rdma_session->rdma_dev == NULL) {
		pr_err("%s, rdma_session->rdma_dev is NULL. too early to regiseter RDMA buffer.\n",
		       __func__);
		goto err;
	}

	ret = rswap_setup_buffers(rdma_session);
	if (unlikely(ret)) {
		pr_err("%s, Bind DMA buffer error\n", __func__);
		goto err;
	}

err:
	return ret;
}

int rswap_init_rdma_queue(struct rdma_session_context *rdma_session, int idx)
{
	int ret = 0;
	struct rswap_rdma_queue *rdma_queue = &(rdma_session->rdma_queues[idx]);

	rdma_queue->rdma_session = rdma_session;
	rdma_queue->q_index = idx;
	rdma_queue->type = get_qp_type(idx);
	rdma_queue->cm_id =
		rdma_create_id(&init_net, rswap_rdma_cm_event_handler,
			       rdma_queue, RDMA_PS_TCP, IB_QPT_RC);
	if (IS_ERR(rdma_queue->cm_id)) {
		pr_err("failed to create cm id: %ld\n",
		       PTR_ERR(rdma_queue->cm_id));
		ret = -ENODEV;
		goto err;
	}

	rdma_queue->state = IDLE;
	init_waitqueue_head(&rdma_queue->sem);
	spin_lock_init(&(rdma_queue->cq_lock));
	atomic_set(&(rdma_queue->rdma_post_counter), 0);
	rdma_queue->fs_rdma_req_cache =
		kmem_cache_create("fs_rdma_req_cache",
				  sizeof(struct fs_rdma_req), 0,
				  SLAB_TEMPORARY | SLAB_HWCACHE_ALIGN, NULL);
	if (unlikely(rdma_queue->fs_rdma_req_cache == NULL)) {
		pr_err("%s, allocate rdma_queue->fs_rdma_req_cache failed.\n",
		       __func__);
		ret = -1;
		goto err;
	}

	ret = rdma_resolve_ip_to_ib_device(rdma_session, rdma_queue);
	if (unlikely(ret)) {
		pr_err("%s, bind socket error (addr or route resolve error)\n",
		       __func__);
		return ret;
	}

	return ret;

err:
	rdma_destroy_id(rdma_queue->cm_id);
	return ret;
}

int rdma_session_connect(struct rdma_session_context *rdma_session)
{
	int ret;
	int i;

	for (i = 0; i < num_queues; i++) {
		ret = rswap_init_rdma_queue(rdma_session, i);
		if (unlikely(ret)) {
			pr_err("%s,init rdma queue [%d] failed.\n", __func__,
			       i);
		}
		ret = rswap_create_rdma_queue(rdma_session, i);
		if (unlikely(ret)) {
			pr_err("%s, Create rdma queues failed. \n", __func__);
		}
		ret = rswap_connect_remote_memory_server(rdma_session, i);
		if (unlikely(ret)) {
			pr_err("%s: Connect to remote server error \n",
			       __func__);
			goto err;
		}

		pr_info("%s, RDMA queue[%d] Connect to remote server successfully \n",
			__func__, i);
	}
	ret = rswap_query_available_memory(rdma_session);
	if (unlikely(ret)) {
		pr_info("%s, request for chunk failed.\n", __func__);
		goto err;
	}

	ret = rswap_request_for_chunk(rdma_session);
	if (unlikely(ret)) {
		pr_info("%s, request for chunk failed.\n", __func__);
		goto err;
	}
	pr_info("%s,Exit the main() function with built RDMA conenction rdma_session_context:0x%llx .\n",
		__func__, (uint64_t)rdma_session);

	return ret;

err:
	pr_err("ERROR in %s \n", __func__);
	return ret;
}

void rswap_free_buffers(struct rdma_session_context *rdma_session)
{
	if (rdma_session == NULL)
		return;

	if (rdma_session->rdma_recv_req.recv_buf != NULL)
		kfree(rdma_session->rdma_recv_req.recv_buf);
	if (rdma_session->rdma_send_req.send_buf != NULL)
		kfree(rdma_session->rdma_send_req.send_buf);

	if (rdma_session->remote_mem_pool.chunks != NULL)
		kfree(rdma_session->remote_mem_pool.chunks);

	pr_debug("%s, Free RDMA buffers done. \n", __func__);
}

void rswap_free_rdma_structure(struct rdma_session_context *rdma_session)
{
	struct rswap_rdma_queue *rdma_queue;
	int i;

	if (rdma_session == NULL)
		return;

	for (i = 0; i < num_queues; i++) {
		rdma_queue = &(rdma_session->rdma_queues[i]);
		if (rdma_queue->cm_id != NULL) {
			rdma_destroy_id(rdma_queue->cm_id);
			pr_debug("%s, free rdma_queue[%d] rdma_cm_id done. \n",
				 __func__, i);
		}
		if (rdma_queue->qp != NULL) {
			ib_destroy_qp(rdma_queue->qp);
			pr_debug("%s, free rdma_queue[%d] ib_qp  done. \n",
				 __func__, i);
		}
		if (rdma_queue->cq != NULL) {
			ib_destroy_cq(rdma_queue->cq);
			pr_debug("%s, free rdma_queue[%d] ib_cq  done. \n",
				 __func__, i);
		}
	}

	if (rdma_session->rdma_dev->pd != NULL) {
		ib_dealloc_pd(rdma_session->rdma_dev->pd);
		pr_debug("%s, Free device PD  done. \n", __func__);
	}
	pr_debug("%s, Free RDMA structures,cm_id,qp,cq,pd done. \n", __func__);
}

int rswap_disconnect_and_collect_resource(
	struct rdma_session_context *rdma_session)
{
	int ret = 0;
	int i;
	struct rswap_rdma_queue *rdma_queue;

	for (i = 0; i < num_queues; i++) {
		rdma_queue = &(rdma_session->rdma_queues[i]);
		if (unlikely(rdma_queue->freed != 0)) {
			pr_warn("%s, rdma_queue[%d] already freed. \n",
				__func__, i);
			continue;
		}
		rdma_queue->freed++;

		if (rdma_queue->state != CM_DISCONNECT) {
			ret = rdma_disconnect(rdma_queue->cm_id);
			if (ret) {
				pr_err("%s, RDMA disconnect failed. \n",
				       __func__);
				goto err;
			}
		}
		pr_debug(
			"%s, RDMA queue[%d] disconnected, start to free resoutce. \n",
			__func__, i);
	}
	for (i = 0; i < num_queues; i++) {
		wait_event_interruptible(rdma_queue->sem,
					 rdma_queue->state == CM_DISCONNECT);
	}

	rswap_free_buffers(rdma_session);
	rswap_free_rdma_structure(rdma_session);
	pr_debug("%s, RDMA memory resouce freed. \n", __func__);
err:
	return ret;
}

char *rdma_cm_message_print(int cm_message_id)
{
	char *type2str[17] = { "RDMA_CM_EVENT_ADDR_RESOLVED",
			       "RDMA_CM_EVENT_ADDR_ERROR",
			       "RDMA_CM_EVENT_ROUTE_RESOLVED",
			       "RDMA_CM_EVENT_ROUTE_ERROR",
			       "RDMA_CM_EVENT_CONNECT_REQUEST",
			       "RDMA_CM_EVENT_CONNECT_RESPONSE",
			       "RDMA_CM_EVENT_CONNECT_ERROR",
			       "RDMA_CM_EVENT_UNREACHABLE",
			       "RDMA_CM_EVENT_REJECTED",
			       "RDMA_CM_EVENT_ESTABLISHED",
			       "RDMA_CM_EVENT_DISCONNECTED",
			       "RDMA_CM_EVENT_DEVICE_REMOVAL",
			       "RDMA_CM_EVENT_MULTICAST_JOIN",
			       "RDMA_CM_EVENT_MULTICAST_ERROR",
			       "RDMA_CM_EVENT_ADDR_CHANGE",
			       "RDMA_CM_EVENT_TIMEWAIT_EXIT",
			       "ERROR Message Type" };

	char *message_type_name = (char *)kzalloc(32, GFP_KERNEL); // 32 bytes
	strcpy(message_type_name,
	       type2str[cm_message_id < 16 ? cm_message_id : 16]);
	return message_type_name;
}

char *rdma_wc_status_name(int wc_status_id)
{
	char *type2str[23] = {
		"IB_WC_SUCCESS",	   "IB_WC_LOC_LEN_ERR",
		"IB_WC_LOC_QP_OP_ERR",	   "IB_WC_LOC_EEC_OP_ERR",
		"IB_WC_LOC_PROT_ERR",	   "IB_WC_WR_FLUSH_ERR",
		"IB_WC_MW_BIND_ERR",	   "IB_WC_BAD_RESP_ERR",
		"IB_WC_LOC_ACCESS_ERR",	   "IB_WC_REM_INV_REQ_ERR",
		"IB_WC_REM_ACCESS_ERR",	   "IB_WC_REM_OP_ERR",
		"IB_WC_RETRY_EXC_ERR",	   "IB_WC_RNR_RETRY_EXC_ERR",
		"IB_WC_LOC_RDD_VIOL_ERR",  "IB_WC_REM_INV_RD_REQ_ERR",
		"IB_WC_REM_ABORT_ERR",	   "IB_WC_INV_EECN_ERR",
		"IB_WC_INV_EEC_STATE_ERR", "IB_WC_FATAL_ERR",
		"IB_WC_RESP_TIMEOUT_ERR",  "IB_WC_GENERAL_ERR",
		"ERROR Message Type",
	};

	char *message_type_name = (char *)kzalloc(32, GFP_KERNEL); // 32 bytes
	strcpy(message_type_name,
	       type2str[wc_status_id < 22 ? wc_status_id : 22]);
	return message_type_name;
}

char *rdma_message_print(int message_id)
{
	char *message_type_name;
	char *type2str[12] = {
		"DONE",
		"GOT_CHUNKS",
		"GOT_SINGLE_CHUNK",
		"FREE_SIZE",
		"EVICT",
		"ACTIVITY",
		"STOP",
		"REQUEST_CHUNKS",
		"REQUEST_SINGLE_CHUNK",
		"QUERY",
		"AVAILABLE_TO_QUERY",
		"ERROR Message Type",
	};

	// message_id starts from 1
	message_id -= 1;

	message_type_name = (char *)kzalloc(32, GFP_KERNEL); // 32 bytes
	strcpy(message_type_name, type2str[message_id < 11 ? message_id : 11]);
	return message_type_name;
}

char *rdma_session_context_state_print(int id)
{
	char *rdma_seesion_state_name;

	char *state2str[20] = { "IDLE",
				"CONNECT_REQUEST",
				"ADDR_RESOLVED",
				"ROUTE_RESOLVED",
				"CONNECTED",
				"FREE_MEM_RECV",
				"RECEIVED_CHUNKS",
				"RDMA_BUF_ADV",
				"WAIT_OPS",
				"RECV_STOP",
				"RECV_EVICT",
				"RDMA_WRITE_RUNNING",
				"RDMA_READ_RUNNING",
				"SEND_DONE",
				"RDMA_DONE",
				"RDMA_READ_ADV",
				"RDMA_WRITE_ADV",
				"CM_DISCONNECT",
				"ERROR",
				"Un-defined state." };

	// message id starts from 1
	id -= 1;
	rdma_seesion_state_name = (char *)kzalloc(32, GFP_KERNEL); // 32 bytes.
	strcpy(rdma_seesion_state_name, state2str[id < 19 ? id : 19]);
	return rdma_seesion_state_name;
}

void print_scatterlist_info(struct scatterlist *sl_ptr, int nents)
{
	int i;

	pr_info("\n %s, %d entries , Start\n", __func__, nents);
	for (i = 0; i < nents; i++) {
		pr_info("%s, \n page_link(struct page*) : 0x%lx \n  offset : 0x%x bytes\n  length : 0x%x bytes \n dma_addr : 0x%llx \n ",
			__func__, sl_ptr[i].page_link, sl_ptr[i].offset,
			sl_ptr[i].length, sl_ptr[i].dma_address);
		//  }
	}
	pr_info(" %s End\n\n", __func__);
}

void print_critical_macros(void)
{
	pr_warn("\n %s, Warning, MACROS check ---->\n", __func__);

#ifdef RSWAP_KERNEL_SUPPORT
	pr_warn("%s, RSWAP_KERNEL_SUPPORT is defined. \n", __func__);
#endif

	pr_warn("\n MACROS check end <---- \n");

	return;
}
