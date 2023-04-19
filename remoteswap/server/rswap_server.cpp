#include "rswap_server.hpp"
#include <string.h>

struct context *global_rdma_ctx = NULL;
int rdma_queue_count = 0;
size_t region_num = 0;

int online_cores = 0;
int rdma_num_queues = 0;

inline enum rdma_queue_type get_qp_type(int idx) {
  unsigned type = idx / online_cores;

  if (type < NUM_QP_TYPE) {
    return (enum rdma_queue_type)type;
  } else {
    printf("wrong rdma queue type %d\n", idx / online_cores);
    return QP_STORE;
  }

  return QP_STORE;
}

inline struct rswap_rdma_queue *get_rdma_queue(unsigned int cpu,
                                               enum rdma_queue_type type) {
  if (type < NUM_QP_TYPE) {
    return &global_rdma_ctx->rdma_queues[cpu + type * online_cores];
  } else {
    printf("wrong rdma queue type %d\n", type);
    return NULL;
  }
  return NULL;
}

int main(int argc, char *argv[]) {
  struct sockaddr_in6 addr;
  struct rdma_cm_event *event = NULL;
  struct rdma_cm_id *listener = NULL;
  struct rdma_event_channel *ec = NULL;

  int mem_size_in_gb;
  uint16_t port = 0;
  char ip_str[INET_ADDRSTRLEN];

  if (argc < 5) {
    printf("Usage: %s <ip> <port> <memory size in GB> <#(cores) on CPU "
           "server>\n",
           argv[0]);
    exit(-1);
  }
  strcpy(ip_str, argv[1]);
  port = atoi(argv[2]);
  mem_size_in_gb = atoi(argv[3]);
  region_num = mem_size_in_gb / REGION_SIZE_GB;
  online_cores = atoi(argv[4]);
  rdma_num_queues = online_cores * NUM_QP_TYPE;

  fprintf(stderr, "%s, trying to bind to %s:%d.\n", __func__, ip_str, port);

  memset(&addr, 0, sizeof(addr));
  addr.sin6_family = AF_INET6;
  inet_pton(AF_INET6, ip_str, &addr.sin6_addr);
  addr.sin6_port = htons(port);

  ec = rdma_create_event_channel();
  if (!ec) {
    fprintf(stderr, "rdma_create_event_channel failed %p.\n", ec);
  }
  if (rdma_create_id(ec, &listener, NULL, RDMA_PS_TCP)) {
    fprintf(stderr, "rdma_create_id failed.\n");
  }
  int ret = 0;
  if (rdma_bind_addr(listener, (struct sockaddr *)&addr)) {
    fprintf(stderr, "rdma_bind_addr failed %d.\n", ret);
  }
  if (rdma_listen(listener, 10)) {
    fprintf(stderr, "rdma_listen failed,return non-zero\n");
  }
  port = ntohs(rdma_get_src_port(listener));
  fprintf(stderr, "listening on port %d.\n", port);

  global_rdma_ctx = (struct context *)calloc(1, sizeof(struct context));
  global_rdma_ctx->rdma_queues = (struct rswap_rdma_queue *)calloc(
      rdma_num_queues, sizeof(struct rswap_rdma_queue));
  global_rdma_ctx->connected = 0;
  global_rdma_ctx->server_state = S_WAIT;
  init_memory_pool(global_rdma_ctx);

  while (rdma_get_cm_event(ec, &event) == 0) {
    struct rdma_cm_event event_copy;
    memcpy(&event_copy, event, sizeof(*event));
    rdma_ack_cm_event(event);
    if (on_cm_event(&event_copy))
      break;
  }

  fprintf(stderr, "%s, RDMA cma thread exit.\n", __func__);
  rdma_destroy_event_channel(ec);

  return 0;
}

void init_memory_pool(struct context *rdma_ctx) {
  int i;
  rdma_ctx->mem_pool =
      (struct rdma_mem_pool *)calloc(1, sizeof(struct rdma_mem_pool));

  size_t REGION_SIZE = ONE_GB * REGION_SIZE_GB;
  size_t heap_size = REGION_SIZE * region_num;
  void *heap_start = malloc(heap_size);
  print_debug(stderr, "%s, Register Semeru Space: 0x%llx, size : 0x%llx. \n",
              __func__, (unsigned long long)heap_start,
              (unsigned long long)heap_size);

  rdma_ctx->mem_pool->heap_start = (char *)heap_start;
  rdma_ctx->mem_pool->region_num = region_num;

  rdma_ctx->mem_pool->region_list[0] = (char *)heap_start;
  rdma_ctx->mem_pool->region_mapped_size[0] = REGION_SIZE;
  rdma_ctx->mem_pool->cache_status[0] = -1;

  print_debug(stderr,
              "%s, Prepare to register memory Region[%d] (Meta DATA)  : "
              "0x%llx, size 0x%lx\n",
              __func__, 0,
              (unsigned long long)rdma_ctx->mem_pool->region_list[0],
              (size_t)rdma_ctx->mem_pool->region_mapped_size[0]);

  for (i = 1; i < rdma_ctx->mem_pool->region_num; i++) {
    rdma_ctx->mem_pool->region_list[i] =
        rdma_ctx->mem_pool->region_list[i - 1] + REGION_SIZE;
    rdma_ctx->mem_pool->region_mapped_size[i] = REGION_SIZE;
    rdma_ctx->mem_pool->cache_status[i] = -1;

    print_debug(stderr,
                "%s, Prepare to register memory Region[%d] (Object DATA) : "
                "0x%llx, size 0x%lx\n",
                __func__, i,
                (unsigned long long)rdma_ctx->mem_pool->region_list[i],
                (size_t)rdma_ctx->mem_pool->region_mapped_size[i]);
  }
  print_debug(stderr, "Registered %llu GB (whole head) as RDMA Buffer\n",
              (unsigned long long)heap_size / ONE_GB);

  return;
}

int on_cm_event(struct rdma_cm_event *event) {
  int r = 0;
  struct rswap_rdma_queue *rdma_queue =
      (struct rswap_rdma_queue *)event->id->context;

  if (event->event == RDMA_CM_EVENT_CONNECT_REQUEST) {
    print_debug(stderr, "Get RDMA_CM_EVENT_CONNECT_REQUEST\n");
    r = on_connect_request(event->id);
  } else if (event->event == RDMA_CM_EVENT_ESTABLISHED) {
    print_debug(stderr, "Get RDMA_CM_EVENT_ESTABLISHED\n");
    r = rdma_connected(rdma_queue);
  } else if (event->event == RDMA_CM_EVENT_DISCONNECTED) {
    print_debug(stderr, "Get RDMA_CM_EVENT_DISCONNECTED\n");
    r = on_disconnect(rdma_queue);
  } else {
    die("on_cm_event: unknown event.");
  }

  return r;
}

int on_connect_request(struct rdma_cm_id *id) {
  struct rdma_conn_param cm_params;
  struct rswap_rdma_queue *rdma_queue;

  rdma_queue = &(global_rdma_ctx->rdma_queues[rdma_queue_count]);
  rdma_queue->q_index = rdma_queue_count;
  rdma_queue->type = get_qp_type(rdma_queue_count);
  rdma_queue_count++;
  rdma_queue->cm_id = id;

  fprintf(stderr, "%s, rdma_queue[%d] received connection request.\n", __func__,
          rdma_queue_count - 1);
  build_connection(rdma_queue);
  build_params(&cm_params);
  TEST_NZ(rdma_accept(id, &cm_params));

  rdma_queue->connected = 1;
  fprintf(stderr, "%s, rdma_queue[%d] sends ACCEPT back to CPU server.\n",
          __func__, rdma_queue_count - 1);
  return 0;
}

void build_connection(struct rswap_rdma_queue *rdma_queue) {
  struct ibv_qp_init_attr qp_attr;

  get_device_info(rdma_queue);
  build_qp_attr(rdma_queue, &qp_attr);

  TEST_NZ(rdma_create_qp(rdma_queue->cm_id, global_rdma_ctx->rdma_dev->pd,
                         &qp_attr));
  rdma_queue->qp = rdma_queue->cm_id->qp;

  rdma_queue->cm_id->context = rdma_queue;
  rdma_queue->rdma_session = global_rdma_ctx;
}

void get_device_info(struct rswap_rdma_queue *rdma_queue) {
  if (global_rdma_ctx->rdma_dev == NULL) {
    global_rdma_ctx->rdma_dev =
        (struct rswap_rdma_dev *)calloc(1, sizeof(struct rswap_rdma_dev));
    global_rdma_ctx->rdma_dev->ctx = rdma_queue->cm_id->verbs;

    TEST_Z(global_rdma_ctx->rdma_dev->pd =
               ibv_alloc_pd(rdma_queue->cm_id->verbs));
    TEST_Z(global_rdma_ctx->comp_channel =
               ibv_create_comp_channel(rdma_queue->cm_id->verbs));

    TEST_Z(rdma_queue->cq = ibv_create_cq(rdma_queue->cm_id->verbs, 10, NULL,
                                          global_rdma_ctx->comp_channel,
                                          0)); /* cqe=10 is arbitrary */
    TEST_NZ(ibv_req_notify_cq(rdma_queue->cq, 0));

    TEST_NZ(pthread_create(&global_rdma_ctx->cq_poller_thread, NULL, poll_cq,
                           NULL));

    register_rdma_comm_buffer(global_rdma_ctx);
  }
}

void build_qp_attr(struct rswap_rdma_queue *rdma_queue,
                   struct ibv_qp_init_attr *qp_attr) {
  memset(qp_attr, 0, sizeof(*qp_attr));
  qp_attr->send_cq = rdma_queue->cq;
  qp_attr->recv_cq = rdma_queue->cq;
  qp_attr->qp_type = IBV_QPT_RC;
  qp_attr->cap.max_send_wr = 16;
  qp_attr->cap.max_recv_wr = 16;
  qp_attr->cap.max_send_sge = MAX_REQUEST_SGL;
  qp_attr->cap.max_recv_sge = MAX_REQUEST_SGL;
}

void *poll_cq(void *ctx) {
  struct ibv_cq *cq;
  struct ibv_wc wc;

  while (1) {
    TEST_NZ(ibv_get_cq_event(global_rdma_ctx->comp_channel, &cq, &ctx));
    ibv_ack_cq_events(cq, 1);
    TEST_NZ(ibv_req_notify_cq(cq, 0));

    while (ibv_poll_cq(cq, 1, &wc))
      handle_cqe(&wc);
  }

  return NULL;
}

void register_rdma_comm_buffer(struct context *rdma_session) {
  rdma_session->send_msg = (struct message *)calloc(1, sizeof(struct message));
  rdma_session->recv_msg = (struct message *)calloc(1, sizeof(struct message));

  TEST_Z(rdma_session->send_mr =
             ibv_reg_mr(rdma_session->rdma_dev->pd, rdma_session->send_msg,
                        sizeof(struct message),
                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));

  TEST_Z(rdma_session->recv_mr =
             ibv_reg_mr(rdma_session->rdma_dev->pd, rdma_session->recv_msg,
                        sizeof(struct message),
                        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ));

  fprintf(stderr, "%s, Reserve 2-sided rdma buffer done.\n", __func__);
}

void post_receives(struct rswap_rdma_queue *rdma_queue) {
  struct ibv_recv_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;
  struct context *rdma_session = rdma_queue->rdma_session;

  wr.wr_id = (uintptr_t)rdma_queue;
  wr.next = NULL;
  wr.sg_list = &sge;
  wr.num_sge = 1;

  sge.addr = (uintptr_t)rdma_session->recv_msg;
  sge.length = (uint32_t)sizeof(struct message);
  sge.lkey = rdma_session->recv_mr->lkey;

  TEST_NZ(ibv_post_recv(rdma_queue->qp, &wr, &bad_wr));
}

void build_params(struct rdma_conn_param *params) {
  memset(params, 0, sizeof(*params));

  params->initiator_depth = params->responder_resources = 1;
  params->rnr_retry_count = 7; /* infinite retry */
}

void handle_cqe(struct ibv_wc *wc) {

  struct rswap_rdma_queue *rdma_queue =
      (struct rswap_rdma_queue *)(uintptr_t)wc->wr_id;
  struct context *rdma_session = rdma_queue->rdma_session;

  if (wc->status != IBV_WC_SUCCESS)
    die("handle_cqe: status is not IBV_WC_SUCCESS.");

  if (wc->opcode == IBV_WC_RECV) {
    switch (rdma_session->recv_msg->type) {
    case QUERY:
      fprintf(stderr, "%s, QUERY \n", __func__);
      send_free_mem_size(rdma_queue);
      post_receives(rdma_queue);
      break;

    case REQUEST_CHUNKS:
      fprintf(stderr,
              "%s, REQUEST_CHUNKS, Send available Regions to CPU server.\n",
              __func__);
      rdma_session->server_state = S_BIND;
      send_regions(rdma_queue);
      post_receives(rdma_queue);
      break;

    case REQUEST_SINGLE_CHUNK:
    case ACTIVITY:
    case DONE:
      fprintf(stderr,
              "%s, REQUEST_SINGLE_CHUNK, ACTIVITY, DONE : TO BE DONE.\n",
              __func__);
      break;

    default:
      fprintf(stderr, "Recived error message type : %d.\n",
              rdma_session->recv_msg->type);
      die("unknow received message type\n");
    }
  } else if (wc->opcode == IBV_WC_SEND) {
    fprintf(stderr, "%s, 2-sided RDMA message sent done ?\n", __func__);
  } else {
    fprintf(stderr, "%s, recived wc.opcode %d\n", __func__, wc->opcode);
  }
}

int rdma_connected(struct rswap_rdma_queue *rdma_queue) {
  int i;
  int ret = 0;
  bool succ = true;

  struct context *rdma_session = (struct context *)rdma_queue->rdma_session;

  if (rdma_session->connected == 0) {
    rdma_session->connected = 1;
    fprintf(stderr, "%s, connection build. Register heap as RDMA buffer.\n",
            __func__);
    for (i = 0; i < rdma_session->mem_pool->region_num; i++) {
      rdma_session->mem_pool->mr_buffer[i] = ibv_reg_mr(
          rdma_session->rdma_dev->pd, rdma_session->mem_pool->region_list[i],
          (size_t)rdma_session->mem_pool->region_mapped_size[i],
          IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
              IBV_ACCESS_REMOTE_READ);

      if (rdma_session->mem_pool->mr_buffer[i] != NULL) {
        print_debug(
            stderr,
            "Register Region[%d] : 0x%llx to RDMA Buffer[%d] : 0x%llx, "
            "rkey: "
            "0x%llx, mapped_size 0x%lx done \n",
            i, (unsigned long long)rdma_session->mem_pool->region_list[i], i,
            (unsigned long long)rdma_session->mem_pool->mr_buffer[i],
            (unsigned long long)rdma_session->mem_pool->mr_buffer[i]->rkey,
            (unsigned long)rdma_session->mem_pool->region_mapped_size[i]);
      } else {
        print_debug(
            stderr,
            "%s, region[%d], 0x%lx is registered wrongly, with NULL. \n",
            __func__, i, (size_t)rdma_session->mem_pool->region_list[i]);
        print_debug(stderr, "ERROR in %s, %s\n", __func__, strerror(errno));
        succ = false;
      }
    }
    if (!succ)
      goto err;
  }

  inform_memory_pool_available(rdma_queue);
  post_receives(rdma_queue);

err:
  return ret;
}

void inform_memory_pool_available(struct rswap_rdma_queue *rdma_queue) {

  rdma_queue->rdma_session->send_msg->type = AVAILABLE_TO_QUERY;
  fprintf(stderr,
          "%s , rdma_queue [%d] Inform CPU server that memory server is "
          "prepared well for serving.\n",
          __func__, rdma_queue->q_index);
  send_message(rdma_queue);
}

void send_free_mem_size(struct rswap_rdma_queue *rdma_queue) {
  int i;
  struct context *rdma_session = rdma_queue->rdma_session;

  rdma_session->send_msg->mapped_chunk = rdma_session->mem_pool->region_num;

  for (i = 0; i < rdma_session->mem_pool->region_num; i++) {
    rdma_session->send_msg->buf[i] = 0x0;
    rdma_session->send_msg->rkey[i] = 0x0;
  }

  rdma_session->send_msg->type = FREE_SIZE;
  fprintf(stderr,
          "%s , Send free memory information to CPU server, %d Chunks.\n",
          __func__, rdma_session->send_msg->mapped_chunk);
  send_message(rdma_queue);
}

void send_regions(struct rswap_rdma_queue *rdma_queue) {
  int i;
  struct context *rdma_session = rdma_queue->rdma_session;

  rdma_session->send_msg->mapped_chunk = rdma_session->mem_pool->region_num;

  for (i = 0; i < rdma_session->mem_pool->region_num; i++) {
    rdma_session->send_msg->buf[i] =
        (uint64_t)rdma_session->mem_pool->mr_buffer[i]->addr;
    rdma_session->send_msg->mapped_size[i] =
        (uint64_t)rdma_session->mem_pool->region_mapped_size[i];
    rdma_session->send_msg->rkey[i] =
        rdma_session->mem_pool->mr_buffer[i]->rkey;
  }

  rdma_session->send_msg->type = GOT_CHUNKS;
  fprintf(stderr, "%s , Send registered Java heap to CPU server, %d chunks.\n",
          __func__, rdma_session->send_msg->mapped_chunk);
  send_message(rdma_queue);
}

void send_message(struct rswap_rdma_queue *rdma_queue) {
  struct ibv_send_wr wr, *bad_wr = NULL;
  struct ibv_sge sge;
  struct context *rdma_session = rdma_queue->rdma_session;

  memset(&wr, 0, sizeof(wr));

  wr.wr_id = (uintptr_t)rdma_queue;
  wr.opcode = IBV_WR_SEND;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;

  sge.addr = (uintptr_t)rdma_session->send_msg;
  sge.length = (uint32_t)sizeof(struct message);
  fprintf(stderr, "%s, message size = %lu\n", __func__, sizeof(struct message));
  sge.lkey = rdma_session->send_mr->lkey;

  while (!rdma_session->connected)
    ;

  TEST_NZ(ibv_post_send(rdma_queue->qp, &wr, &bad_wr));
}

int on_disconnect(struct rswap_rdma_queue *rdma_queue) {
  if (rdma_queue->connected == 1) {
    rdma_destroy_qp(rdma_queue->cm_id);
    rdma_destroy_id(rdma_queue->cm_id);
    fprintf(stderr, "%s, free rdma_queue[%d].\n", __func__,
            rdma_queue->q_index);
    rdma_queue->connected = 0;

    rdma_queue_count--;
  }

  if (rdma_queue_count == 0) {
    destroy_connection(rdma_queue->rdma_session);
  }

  return 0;
}

void destroy_connection(struct context *rdma_session) {
  int i = 0;

  ibv_dereg_mr(rdma_session->send_mr);
  ibv_dereg_mr(rdma_session->recv_mr);
  free(rdma_session->send_msg);
  free(rdma_session->recv_msg);

  for (i = 0; i < rdma_session->mem_pool->region_num; i++) {
    if (rdma_session->mem_pool->mr_buffer[i] == NULL) {
      continue;
    }

    ibv_dereg_mr(rdma_session->mem_pool->mr_buffer[i]);
  }

  free(global_rdma_ctx);

  fprintf(stderr, "%s, Memory server stopped.\n", __func__);
  exit(0);
}

void die(const char *reason) {
  fprintf(stderr, "ERROR, %s\n", reason);
  exit(EXIT_FAILURE);
}
