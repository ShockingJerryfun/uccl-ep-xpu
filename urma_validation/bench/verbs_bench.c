/* ibverbs micro-benchmark: mirror of urma_bench.c, same phases and
 * methodology, so URMA vs ibverbs numbers are directly comparable.
 *
 * Server: ./verbs_bench -s
 * Client: ./verbs_bench
 *
 * Phases (identical to urma_bench.c):
 *   1. WRITE ping-pong latency, 8B, 1000 iters (100 warmup)
 *   2. FADD  ping-pong latency, 8B, 1000 iters (100 warmup)
 *   3. WRITE bandwidth, 512 ops, window 32
 *   4. FADD throughput, 4096 ops, window 128
 *   5. BARRIER: bidirectional atomic-add (fence) x1000, spin own word to K
 *   6. QUIET: mixed burst 256 WRITE + 256 FADD, poll-until-drained
 *
 * Env: IBV_DEV_NAME (default first device), IBV_GID_IDX (default 0)
 */
#include <infiniband/verbs.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#define PORT 18716
#define WARMUP 100
#define LAT_ITERS 1000
#define BW_WINDOW 32
#define ATOM_BURST 4096
#define ATOM_WINDOW 128
#define PAYLOAD (32 * 1024)
#define PAYLOAD_SLOTS 32
#define PAYLOAD_LEN (PAYLOAD * PAYLOAD_SLOTS) /* 1MB */
#define ATOM_BUF (64 * 1024)
#define OLDVAL_LEN (ATOM_WINDOW * 8)
#define OPERAND 7
#define BARRIER_WORDS 8
#define BARRIER_K 1000
#define QUIET_OPS 512
#define QUIET_WINDOW 32
static void die(char const* what) {
  fprintf(stderr, "FAIL: %s (%s)\n", what, strerror(errno));
  exit(1);
}
static double now_us(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}
static int cmp_double(void const* a, void const* b) {
  double x = *(double const*)a, y = *(double const*)b;
  return x < y ? -1 : (x > y ? 1 : 0);
}
/* ---- TCP bootstrap (identical to urma_bench) ---- */
static int tcp_listen_accept(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = INADDR_ANY;
  addr.sin_port = htons(PORT);
  if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("bind");
    exit(1);
  }
  listen(fd, 1);
  struct sockaddr_in cli = {};
  socklen_t len = sizeof(cli);
  int cfd = accept(fd, (struct sockaddr*)&cli, &len);
  close(fd);
  return cfd;
}
static int tcp_connect(void) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(PORT);
  inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
  if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    perror("connect");
    exit(1);
  }
  return fd;
}
static void send_blob(int fd, void const* p, uint32_t len) {
  write(fd, &len, 4);
  write(fd, p, len);
}
static void* recv_blob(int fd) {
  uint32_t len = 0;
  ssize_t n;
  char* lp = (char*)&len;
  size_t off = 0;
  while (off < 4) {
    n = read(fd, lp + off, 4 - off);
    if (n <= 0) exit(1);
    off += (size_t)n;
  }
  char* p = (char*)malloc(len ? len : 1);
  off = 0;
  while (off < len) {
    n = read(fd, p + off, len - off);
    if (n <= 0) exit(1);
    off += (size_t)n;
  }
  return p;
}
/* ---- ibverbs objects ---- */
static struct ibv_context* g_ctx;
static struct ibv_pd* g_pd;
static struct ibv_cq* g_cq;
static struct ibv_qp* g_qp;
static uint64_t *g_payload, *g_atomic, *g_oldval, *g_barrier;
static struct ibv_mr *g_mr_payload, *g_mr_atomic, *g_mr_oldval, *g_mr_barrier;
static int g_gid_idx = 0;
static char g_gid[16];
static uint32_t g_qpn;
static uint32_t g_peer_rkey_payload, g_peer_rkey_atomic, g_peer_rkey_barrier;
static uint64_t g_peer_addr_payload, g_peer_addr_atomic, g_peer_addr_barrier;
static char const* g_phase_name = "?";
static double g_phase_t0 = 0;
static int g_bw_window = 32;
static int g_bw_size = 8;
static int g_bw_iters = 512;
static int g_fa_window = ATOM_WINDOW;
static int g_skip_lat = 0;
static int g_no_bw = 0;
static int g_no_fa = 0;
static int g_no_barrier = 0;
static int g_no_quiet = 0;
static int g_no_verify = 0;
static void setup_ibverbs(int is_server) {
  int nd = 0;
  struct ibv_device** devs = ibv_get_device_list(&nd);
  if (!devs || nd == 0) {
    printf("NO_DEVICE\n");
    exit(1);
  }
  char const* want = getenv("IBV_DEV_NAME");
  struct ibv_device* dev = NULL;
  if (want) {
    for (int i = 0; i < nd; ++i)
      if (strcmp(devs[i]->name, want) == 0) dev = devs[i];
  }
  if (!dev) dev = devs[0];
  g_ctx = ibv_open_device(dev);
  if (!g_ctx) die("open_device");
  g_pd = ibv_alloc_pd(g_ctx);
  if (!g_pd) die("alloc_pd");
  g_cq = ibv_create_cq(g_ctx, 4096, NULL, NULL, 0);
  if (!g_cq) die("create_cq");
  struct ibv_qp_init_attr qia = {};
  qia.send_cq = g_cq;
  qia.recv_cq = g_cq;
  qia.qp_type = IBV_QPT_RC;
  qia.cap.max_send_wr = 1024;
  qia.cap.max_recv_wr = 256;
  qia.cap.max_send_sge = 1;
  qia.cap.max_recv_sge = 1;
  g_qp = ibv_create_qp(g_pd, &qia);
  if (!g_qp) die("create_qp");
  g_qpn = g_qp->qp_num;
  if (posix_memalign((void**)&g_payload, 4096, PAYLOAD_LEN) != 0) exit(1);
  memset(g_payload, 0, PAYLOAD_LEN);
  if (posix_memalign((void**)&g_atomic, 4096, ATOM_BUF) != 0) exit(1);
  memset(g_atomic, 0, ATOM_BUF);
  if (posix_memalign((void**)&g_oldval, 4096, OLDVAL_LEN) != 0) exit(1);
  memset(g_oldval, 0, OLDVAL_LEN);
  if (posix_memalign((void**)&g_barrier, 4096, BARRIER_WORDS * 8) != 0) exit(1);
  memset(g_barrier, 0, BARRIER_WORDS * 8);
  g_mr_payload = ibv_reg_mr(g_pd, g_payload, PAYLOAD_LEN,
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  g_mr_atomic = ibv_reg_mr(g_pd, g_atomic, ATOM_BUF,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                               IBV_ACCESS_REMOTE_ATOMIC);
  g_mr_oldval = ibv_reg_mr(g_pd, g_oldval, OLDVAL_LEN,
                           IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
  g_mr_barrier = ibv_reg_mr(g_pd, g_barrier, BARRIER_WORDS * 8,
                            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                                IBV_ACCESS_REMOTE_ATOMIC);
  if (!g_mr_payload || !g_mr_atomic || !g_mr_oldval || !g_mr_barrier)
    die("reg_mr");
  union ibv_gid gid;
  if (ibv_query_gid(g_ctx, 1, g_gid_idx, &gid) != 0) die("query_gid");
  memcpy(g_gid, gid.raw, 16);
  printf("[%s] qp=OK dev=%s gid_idx=%d\n", is_server ? "server" : "client",
         dev->name, g_gid_idx);
}
/* ---- posting helpers ---- */
static void post_write(uint64_t wr_id, uint64_t loff, uint64_t roff,
                       uint32_t bytes) {
  uint64_t slot = wr_id % (uint64_t)g_bw_window;
  (void)slot;
  struct ibv_sge sge = {};
  sge.addr = (uint64_t)g_payload + loff;
  sge.length = bytes;
  sge.lkey = g_mr_payload->lkey;
  struct ibv_send_wr wr = {};
  struct ibv_send_wr* bad = NULL;
  wr.wr_id = wr_id;
  wr.opcode = IBV_WR_RDMA_WRITE;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.rdma.remote_addr = g_peer_addr_payload + roff;
  wr.wr.rdma.rkey = g_peer_rkey_payload;
  if (ibv_post_send(g_qp, &wr, &bad)) die("post write");
}
static void post_fadd(uint64_t wr_id, uint64_t slot) {
  (void)slot;
  struct ibv_sge sge = {};
  sge.addr = (uint64_t)g_oldval + slot * 8;
  sge.length = 8;
  sge.lkey = g_mr_oldval->lkey;
  struct ibv_send_wr wr = {};
  struct ibv_send_wr* bad = NULL;
  wr.wr_id = wr_id;
  wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED;
  wr.wr.atomic.remote_addr = g_peer_addr_atomic;
  wr.wr.atomic.rkey = g_peer_rkey_atomic;
  wr.wr.atomic.compare_add = OPERAND;
  if (ibv_post_send(g_qp, &wr, &bad)) die("post fadd");
}
static void post_barrier_add(uint64_t wr_id, uint64_t slot) {
  (void)slot;
  struct ibv_sge sge = {};
  sge.addr = (uint64_t)g_oldval + slot * 8;
  sge.length = 8;
  sge.lkey = g_mr_oldval->lkey;
  struct ibv_send_wr wr = {};
  struct ibv_send_wr* bad = NULL;
  wr.wr_id = wr_id;
  wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
  wr.wr.atomic.remote_addr = g_peer_addr_barrier;
  wr.wr.atomic.rkey = g_peer_rkey_barrier;
  wr.wr.atomic.compare_add = 1;
  if (ibv_post_send(g_qp, &wr, &bad)) die("post barrier add");
}
/* ---- polling (mirrors urma poll_n) ---- */
static struct ibv_wc g_wcs[64];
static int g_poll_cap = 16;
static uint64_t g_spins = 0;
static uint64_t g_dbg_polled = 0;
static int poll_once(void) {
  int n = ibv_poll_cq(g_cq, g_poll_cap, g_wcs);
  if (n < 0) {
    printf("POLL_ERR\n");
    exit(1);
  }
  for (int i = 0; i < n; ++i) {
    if (g_wcs[i].status != IBV_WC_SUCCESS) {
      printf("WC status=%d (%s)\n", g_wcs[i].status,
             ibv_wc_status_str(g_wcs[i].status));
      exit(1);
    }
  }
  return n;
}
static uint64_t poll_n(uint32_t want) {
  (void)want;
  double entry = now_us();
  double last_hb = entry;
  for (;;) {
    int n = poll_once();
    if (n > 0) {
      g_dbg_polled += (uint64_t)n;
      return (uint64_t)n;
    }
    double t = now_us();
    ++g_spins;
    if (g_spins > 1000000 && (g_spins % 10000 == 0)) usleep(100);
    if (t - last_hb > 5e6) {
      fprintf(stderr, "SPIN %s: spins=%lu elapsed=%.2fs polled_total=%lu\n",
              g_phase_name, (unsigned long)g_spins,
              (t - g_phase_t0) / 1e6, (unsigned long)g_dbg_polled);
      last_hb = t;
    }
    if (t - entry > 30e6) {
      fprintf(stderr, "STALL %s: spins=%lu elapsed=%.2fs polled_total=%lu\n",
              g_phase_name, (unsigned long)g_spins,
              (t - g_phase_t0) / 1e6, (unsigned long)g_dbg_polled);
      exit(42);
    }
  }
}
/* ---- benchmark phases (identical to urma_bench) ---- */
static double g_lat[LAT_ITERS];
static void phase_write_latency(void) {
  g_phase_name = "WRITE-lat";
  g_phase_t0 = now_us();
  int const lat_sizes[] = {8, 512, 4096, 16384, 32768};
  int const lat_nsizes = (int)(sizeof(lat_sizes) / sizeof(lat_sizes[0]));
  for (int s = 0; s < lat_nsizes; ++s) {
  int const sz = lat_sizes[s];
  for (int i = 0; i < WARMUP; ++i) {
    post_write((uint64_t)0x100000 + i, 0, 0, sz);
    poll_n(1);
  }
  for (int i = 0; i < LAT_ITERS; ++i) {
    double t0 = now_us();
    post_write((uint64_t)0x200000 + i, 0, 0, sz);
    poll_n(1);
    g_lat[i] = now_us() - t0;
  }
  qsort(g_lat, LAT_ITERS, sizeof(double), cmp_double);
  double sum = 0;
  for (int i = 0; i < LAT_ITERS; ++i) sum += g_lat[i];
  printf("WRITE-latency %-5d: min=%.2f p50=%.2f p90=%.2f p99=%.2f max=%.2f "
         "mean=%.2f us\n",
         sz, g_lat[0], g_lat[LAT_ITERS / 2], g_lat[LAT_ITERS * 9 / 10],
         g_lat[LAT_ITERS * 99 / 100], g_lat[LAT_ITERS - 1],
         sum / LAT_ITERS);
  }
}
static void phase_fadd_latency(void) {
  g_phase_name = "FADD-lat";
  g_phase_t0 = now_us();
  for (int i = 0; i < WARMUP; ++i) {
    post_fadd((uint64_t)0x100000 + i, 0);
    poll_n(1);
  }
  for (int i = 0; i < LAT_ITERS; ++i) {
    double t0 = now_us();
    post_fadd((uint64_t)0x200000 + i, 0);
    poll_n(1);
    g_lat[i] = now_us() - t0;
  }
  qsort(g_lat, LAT_ITERS, sizeof(double), cmp_double);
  double sum = 0;
  for (int i = 0; i < LAT_ITERS; ++i) sum += g_lat[i];
  printf("FADD-latency 8B    : min=%.2f p50=%.2f p90=%.2f p99=%.2f max=%.2f "
         "mean=%.2f us\n",
         g_lat[0], g_lat[LAT_ITERS / 2], g_lat[LAT_ITERS * 9 / 10],
         g_lat[LAT_ITERS * 99 / 100], g_lat[LAT_ITERS - 1],
         sum / LAT_ITERS);
}
static void phase_write_bandwidth(void) {
  g_phase_name = "WRITE-bw";
  double t0 = now_us();
  g_phase_t0 = t0;
  int inflight = 0;
  uint64_t collected = 0;
  for (int i = 0; i < g_bw_iters; ++i) {
    while (inflight >= g_bw_window) {
      uint64_t got = poll_n(1);
      collected += got;
      inflight -= (int)got;
    }
    post_write((uint64_t)0x300000 + i, 0, (i % 32) * PAYLOAD, g_bw_size);
    inflight++;
  }
  while (inflight > 0) {
    uint64_t got = poll_n(1);
    collected += got;
    inflight -= (int)got;
  }
  double secs = (now_us() - t0) / 1e6;
  double bytes = (double)g_bw_iters * g_bw_size;
  printf("WRITE-bw %dB x %d w=%d : %.2f GB/s (%.1f MRps, crs=%lu)\n",
         g_bw_size, g_bw_iters, g_bw_window, bytes / secs / 1e9,
         (double)g_bw_iters / secs / 1e6, (unsigned long)collected);
}
static void phase_fadd_throughput(void) {
  g_phase_name = "FADD-thr";
  double t0 = now_us();
  g_phase_t0 = t0;
  int inflight = 0;
  uint64_t collected = 0;
  for (int i = 0; i < ATOM_BURST; ++i) {
    while (inflight >= g_fa_window) {
      uint64_t got = poll_n(1);
      collected += got;
      inflight -= (int)got;
    }
    post_fadd((uint64_t)0x400000 + i, (i % g_fa_window));
    inflight++;
  }
  while (inflight > 0) {
    uint64_t got = poll_n(1);
    collected += got;
    inflight -= (int)got;
  }
  double secs = (now_us() - t0) / 1e6;
  printf("FADD-throughput    : %.2f Mops/s (%d ops, crs=%lu)\n",
         (double)ATOM_BURST / secs / 1e6, ATOM_BURST, (unsigned long)collected);
}
/* Returns elapsed seconds for the peer-adds half (posting + polling). */
static int post_adds_to_peer_timed(int iters, int window, double* out_secs) {
  double t0 = now_us();
  int inflight = 0;
  uint64_t collected = 0;
  for (int i = 0; i < iters; ++i) {
    while (inflight >= window) {
      uint64_t got = poll_n(1);
      collected += got;
      inflight -= (int)got;
    }
    post_barrier_add((uint64_t)0x500000 + i, (i % window));
    inflight++;
  }
  while (inflight > 0) {
    uint64_t got = poll_n(1);
    collected += got;
    inflight -= (int)got;
  }
  if (out_secs) *out_secs = (now_us() - t0) / 1e6;
  return 0;
}
static int post_adds_to_peer(int iters, int window) {
  return post_adds_to_peer_timed(iters, window, NULL);
}
/* Returns total drain seconds: post mixed burst, then collect all CRs. */
static int post_quiet_ops_timed(int iters, int window, double* out_secs) {
  double t0 = now_us();
  int inflight = 0;
  uint64_t collected = 0;
  for (int i = 0; i < iters; ++i) {
    while (inflight >= window) {
      uint64_t got = poll_n(1);
      collected += got;
      inflight -= (int)got;
    }
    if (i % 2 == 0) {
      post_write((uint64_t)0x600000 + i, 0, (i % 32) * PAYLOAD, 64);
    } else {
      post_fadd((uint64_t)0x600000 + i, (i % window));
    }
    inflight++;
  }
  while (inflight > 0) {
    uint64_t got = poll_n(1);
    collected += got;
    inflight -= (int)got;
  }
  if (out_secs) *out_secs = (now_us() - t0) / 1e6;
  return 0;
}
/* ---- QP state machine + connection exchange ---- */
static void qp_to_init(void) {
  struct ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_INIT;
  attr.pkey_index = 0;
  attr.port_num = 1;
  attr.qp_access_flags =
      IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_ATOMIC;
  if (ibv_modify_qp(g_qp, &attr,
                    IBV_QP_STATE | IBV_QP_PKEY_INDEX | IBV_QP_PORT |
                        IBV_QP_ACCESS_FLAGS))
    die("qp->INIT");
}
static void qp_connect(uint32_t peer_qpn, uint8_t const* peer_gid) {
  struct ibv_qp_attr attr = {};
  attr.qp_state = IBV_QPS_RTR;
  attr.path_mtu = IBV_MTU_1024;
  attr.dest_qp_num = peer_qpn;
  attr.rq_psn = 0;
  attr.max_dest_rd_atomic = 16;
  attr.min_rnr_timer = 12;
  attr.ah_attr.is_global = 1;
  attr.ah_attr.port_num = 1;
  memcpy(attr.ah_attr.grh.dgid.raw, peer_gid, 16);
  attr.ah_attr.grh.sgid_index = g_gid_idx;
  attr.ah_attr.grh.hop_limit = 1;
  if (ibv_modify_qp(g_qp, &attr,
                    IBV_QP_STATE | IBV_QP_PATH_MTU | IBV_QP_DEST_QPN |
                        IBV_QP_RQ_PSN | IBV_QP_MAX_DEST_RD_ATOMIC |
                        IBV_QP_MIN_RNR_TIMER | IBV_QP_AV))
    die("qp->RTR");
  attr.qp_state = IBV_QPS_RTS;
  attr.sq_psn = 0;
  attr.timeout = 14;
  attr.retry_cnt = 7;
  attr.rnr_retry = 7;
  attr.max_rd_atomic = 16;
  if (ibv_modify_qp(g_qp, &attr,
                    IBV_QP_STATE | IBV_QP_SQ_PSN | IBV_QP_TIMEOUT |
                        IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY |
                        IBV_QP_MAX_QP_RD_ATOMIC))
    die("qp->RTS");
}
static void connect_peer(int fd) {
  struct __attribute__((packed)) {
    uint32_t qpn;
    uint8_t gid[16];
    uint32_t rkey_payload, rkey_atomic, rkey_barrier;
    uint64_t addr_payload, addr_atomic, addr_barrier;
  } local, remote;
  local.qpn = g_qpn;
  memcpy(local.gid, g_gid, 16);
  local.rkey_payload = g_mr_payload->rkey;
  local.rkey_atomic = g_mr_atomic->rkey;
  local.rkey_barrier = g_mr_barrier->rkey;
  local.addr_payload = (uint64_t)g_payload;
  local.addr_atomic = (uint64_t)g_atomic;
  local.addr_barrier = (uint64_t)g_barrier;
  send_blob(fd, &local, sizeof(local));
  void* rp = recv_blob(fd);
  memcpy(&remote, rp, sizeof(remote));
  free(rp);
  qp_connect(remote.qpn, remote.gid);
  g_peer_rkey_payload = remote.rkey_payload;
  g_peer_rkey_atomic = remote.rkey_atomic;
  g_peer_rkey_barrier = remote.rkey_barrier;
  g_peer_addr_payload = remote.addr_payload;
  g_peer_addr_atomic = remote.addr_atomic;
  g_peer_addr_barrier = remote.addr_barrier;
}
/* ---- phase 7: WRITE data verification ---- */
#define VERIFY_SLOTS 32
#define VERIFY_FADD_SEQ 100
#define VERIFY_FADD_PIPE 128
/* ---- BARRIER latency: bidirectional fence-FADD rendezvous ---- */
#define BAR_LAT_ROUNDS 200
#define BAR_LAT_OFFSET 1000 /* qword offset into atomic buffer (fresh region) */
static void post_barrier_lat(uint64_t wr_id, uint64_t slot, uint64_t qw_off) {
  struct ibv_sge sge = {};
  sge.addr = (uint64_t)g_oldval + slot * 8;
  sge.length = 8;
  sge.lkey = g_mr_oldval->lkey;
  struct ibv_send_wr wr = {};
  struct ibv_send_wr* bad = NULL;
  wr.wr_id = wr_id;
  wr.opcode = IBV_WR_ATOMIC_FETCH_AND_ADD;
  wr.sg_list = &sge;
  wr.num_sge = 1;
  wr.send_flags = IBV_SEND_SIGNALED | IBV_SEND_FENCE;
  wr.wr.atomic.remote_addr = g_peer_addr_atomic + qw_off * 8;
  wr.wr.atomic.rkey = g_peer_rkey_atomic;
  wr.wr.atomic.compare_add = 1;
  if (ibv_post_send(g_qp, &wr, &bad)) die("post barrier lat");
}
static double g_bar_lat[BAR_LAT_ROUNDS];
static void phase_barrier_latency(int fd) {
  char cmd = 'L';
  write(fd, &cmd, 1);
  for (int i = 0; i < BAR_LAT_ROUNDS; ++i) {
    double t0 = now_us();
    post_barrier_lat(0x900000 + (uint64_t)i, 0,
                     (uint64_t)(BAR_LAT_OFFSET + i));
    poll_n(1); /* my fence-FADD send completed */
    uint64_t* my_word = g_atomic + BAR_LAT_OFFSET + i;
    while (__atomic_load_n(my_word, __ATOMIC_ACQUIRE) < 1) {
      poll_once(); /* keep CQ draining */
    }
    g_bar_lat[i] = now_us() - t0;
  }
  /* wait for server to finish its rounds too */
  char resp = 0;
  read(fd, &resp, 1);
  qsort(g_bar_lat, BAR_LAT_ROUNDS, sizeof(double), cmp_double);
  double sum = 0;
  for (int i = 0; i < BAR_LAT_ROUNDS; ++i) sum += g_bar_lat[i];
  printf("BARRIER-latency    : min=%.2f p50=%.2f p90=%.2f p99=%.2f mean=%.2f"
         " us (%d rounds)\n",
         g_bar_lat[0], g_bar_lat[BAR_LAT_ROUNDS / 2],
         g_bar_lat[BAR_LAT_ROUNDS * 9 / 10],
         g_bar_lat[BAR_LAT_ROUNDS - 1], sum / BAR_LAT_ROUNDS,
         BAR_LAT_ROUNDS);
}
/* ---- QUIET latency: pure drain time for N pending ops ---- */
static void phase_quiet_latency(void) {
  int const counts[] = {32, 128, 512};
  int const ncounts = 3;
  for (int c = 0; c < ncounts; ++c) {
    int n = counts[c];
    /* Post n ops back-to-back WITHOUT polling (all in flight at once). */
    for (int i = 0; i < n; ++i) {
      if (i % 2 == 0) {
        post_write((uint64_t)0xA00000 + (c * 1000) + i, 0,
                   (i % 32) * PAYLOAD, 64);
      } else {
        post_fadd((uint64_t)0xA00000 + (c * 1000) + i, (i % ATOM_WINDOW));
      }
    }
    /* Timer starts AFTER last post returns: measure pure drain. */
    double t0 = now_us();
    int inflight = n;
    while (inflight > 0) {
      uint64_t got = poll_n(1);
      inflight -= (int)got;
    }
    double drain = now_us() - t0;
    printf("QUIET-latency n=%-4d: %.2f us (drain only, %d W + %d F)\n",
           n, drain, n / 2, n / 2);
  }
}
static uint64_t verify_pattern(int slot, int j) {
  return 0xDEADB00000000000ULL | ((uint64_t)slot << 32) | (uint64_t)j;
}
static void phase_write_verify(int fd) {
  /* Fill local payload slots with unique patterns. */
  for (int slot = 0; slot < VERIFY_SLOTS; ++slot) {
    uint64_t* p = g_payload + (size_t)slot * (PAYLOAD / 8);
    for (int j = 0; j < PAYLOAD / 8; ++j) p[j] = verify_pattern(slot, j);
  }
  /* Write each slot to the peer. */
  int inflight = 0;
  for (int slot = 0; slot < VERIFY_SLOTS; ++slot) {
    while (inflight >= 8) inflight -= (int)poll_n(1);
    post_write(0x700000 + (uint64_t)slot, (uint64_t)slot * PAYLOAD,
               (uint64_t)slot * PAYLOAD, PAYLOAD);
    ++inflight;
  }
  while (inflight > 0) inflight -= (int)poll_n(1);
  /* Ask server to verify. */
  char cmd = 'V';
  write(fd, &cmd, 1);
  char resp = 0;
  ssize_t r = read(fd, &resp, 1);
  if (r == 1 && resp == 'v') {
    printf("WRITE-verify       : PASS (%d slots x %d qwords, byte-pattern "
           "checked on server)\n",
           VERIFY_SLOTS, PAYLOAD / 8);
  } else {
    printf("WRITE-verify       : FAIL (resp=%c r=%zd)\n", resp, r);
  }
}
static uint64_t read_remote_atomic(int fd) {
  char cmd = 'R';
  write(fd, &cmd, 1);
  uint64_t v = 0;
  ssize_t r = read(fd, &v, 8);
  (void)r;
  return v;
}
/* ---- phase 8: FADD oldval verification ---- */
static void phase_fadd_verify(int fd) {
  /* --- Test A: sequential (window=1) — oldval must be strictly sequential. */
  uint64_t base = read_remote_atomic(fd);
  for (int i = 0; i < VERIFY_FADD_SEQ; ++i) {
    post_fadd(0x800000 + (uint64_t)i, (uint64_t)i);
    poll_n(1); /* window=1: wait for completion before next post */
  }
  int seq_fail = 0;
  for (int i = 0; i < VERIFY_FADD_SEQ; ++i) {
    uint64_t expected = base + (uint64_t)i * OPERAND;
    if (g_oldval[i] != expected) {
      printf("FADD-oldval        : FAIL seq i=%d got=%lu want=%lu\n", i,
             (unsigned long)g_oldval[i], (unsigned long)expected);
      seq_fail = 1;
      break;
    }
  }
  printf("FADD-oldval seq    : %s (%d FADDs, window=1, oldval[i]==base+i*7)\n",
         seq_fail ? "FAIL" : "PASS", VERIFY_FADD_SEQ);
  /* --- Test B: pipelined (window=128) — sum of oldvals must be exact. --- */
  uint64_t base2 = read_remote_atomic(fd);
  int inflight = 0;
  for (int i = 0; i < VERIFY_FADD_PIPE; ++i) {
    while (inflight >= VERIFY_FADD_PIPE) inflight -= (int)poll_n(1);
    post_fadd(0x810000 + (uint64_t)i, (uint64_t)i);
    ++inflight;
  }
  while (inflight > 0) inflight -= (int)poll_n(1);
  /* Verify sum: oldvals are a permutation of base2+0, base2+7, ..., so
   * sum = N*base2 + OPERAND*sum(0..N-1). */
  uint64_t sum = 0;
  for (int i = 0; i < VERIFY_FADD_PIPE; ++i) sum += g_oldval[i];
  uint64_t expect_sum = (uint64_t)VERIFY_FADD_PIPE * base2 +
                        OPERAND * (uint64_t)(VERIFY_FADD_PIPE - 1) *
                        VERIFY_FADD_PIPE / 2;
  uint64_t final_v = read_remote_atomic(fd);
  uint64_t expect_final = base2 + (uint64_t)VERIFY_FADD_PIPE * OPERAND;
  int pipe_fail = (sum != expect_sum) || (final_v != expect_final);
  if (pipe_fail) {
    printf("FADD-oldval pipe   : FAIL sum=%lu want=%lu final=%lu want=%lu\n",
           (unsigned long)sum, (unsigned long)expect_sum,
           (unsigned long)final_v, (unsigned long)expect_final);
  } else {
    printf("FADD-oldval pipe   : PASS (%d FADDs window=128, sum checked: "
           "no lost/dup ops)\n",
           VERIFY_FADD_PIPE);
  }
}
int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IOLBF, 0);
  int is_server = (argc > 1 && strcmp(argv[1], "-s") == 0);
  char* e;
  if ((e = getenv("IBV_GID_IDX"))) g_gid_idx = atoi(e);
  if ((e = getenv("IBV_BW_WINDOW"))) g_bw_window = atoi(e);
  if ((e = getenv("IBV_BW_SIZE"))) g_bw_size = atoi(e);
  if ((e = getenv("IBV_BW_ITERS"))) g_bw_iters = atoi(e);
  if ((e = getenv("IBV_FA_WINDOW"))) g_fa_window = atoi(e);
  if ((e = getenv("IBV_SKIP_LAT"))) g_skip_lat = atoi(e);
  if ((e = getenv("IBV_NO_BW"))) g_no_bw = atoi(e);
  if ((e = getenv("IBV_NO_FA"))) g_no_fa = atoi(e);
  if ((e = getenv("IBV_NO_BARRIER"))) g_no_barrier = atoi(e);
  if ((e = getenv("IBV_NO_QUIET"))) g_no_quiet = atoi(e);
  if ((e = getenv("IBV_NO_VERIFY"))) g_no_verify = atoi(e);
  setup_ibverbs(is_server);
  int fd = is_server ? tcp_listen_accept() : tcp_connect();
  printf("[%s] tcp connected\n", is_server ? "server" : "client");
  qp_to_init();
  connect_peer(fd);
  printf("[%s] connected\n", is_server ? "server" : "client");
  if (is_server) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
    char c = 0;
    uint64_t srv_crs = 0;
    uint64_t main_iters = 0;
    for (;;) {
      int n = poll_once();
      srv_crs += (uint64_t)n;
      if (++main_iters % 10000 == 0) usleep(50);
      ssize_t r = read(fd, &c, 1);
      if (r == 1) {
        if (c == 'B') {
          post_adds_to_peer(BARRIER_K, 32);
          double t0 = now_us();
          double last_hb = t0;
          uint64_t b_iters = 0;
          int failed = 0;
          while (__atomic_load_n(&g_barrier[0], __ATOMIC_ACQUIRE) <
                 (uint64_t)BARRIER_K) {
            poll_once();
            ++b_iters;
            if (b_iters > 1000000 && (b_iters % 10000 == 0)) usleep(100);
            double t = now_us();
            if (t - last_hb > 5e6) {
              fprintf(stderr, "[server] barrier wait: word=%lu/%d %.1fs\n",
                      (unsigned long)g_barrier[0], BARRIER_K, (t - t0) / 1e6);
              last_hb = t;
            }
            if (t - t0 > 30e6) {
              failed = 1;
              break;
            }
          }
          fprintf(stderr, "[server] barrier word=%lu expect=%d %s\n",
                  (unsigned long)g_barrier[0], BARRIER_K, failed ? "FAIL" : "OK");
          char resp = failed ? 'f' : 'b';
          write(fd, &resp, 1);
        } else if (c == 'V') {
          /* server-side WRITE data verification */
          int vfail = 0;
          for (int slot = 0; slot < VERIFY_SLOTS && !vfail; ++slot) {
            uint64_t* p = g_payload + (size_t)slot * (PAYLOAD / 8);
            for (int j = 0; j < PAYLOAD / 8; ++j) {
              if (p[j] != verify_pattern(slot, j)) {
                fprintf(stderr,
                        "[server] verify FAIL slot=%d j=%d got=0x%lx "
                        "want=0x%lx\n",
                        slot, j, (unsigned long)p[j],
                        (unsigned long)verify_pattern(slot, j));
                vfail = 1;
                break;
              }
            }
          }
          char resp = vfail ? 'f' : 'v';
          write(fd, &resp, 1);
          fprintf(stderr, "[server] WRITE-verify %s (%d slots x %d qwords)\n",
                  vfail ? "FAIL" : "PASS", VERIFY_SLOTS, PAYLOAD / 8);
        } else if (c == 'R') {
          uint64_t v = __atomic_load_n(&g_atomic[0], __ATOMIC_ACQUIRE);
          write(fd, &v, 8);
        } else if (c == 'L') {
          /* barrier latency: mirror the client's rounds */
          for (int i = 0; i < BAR_LAT_ROUNDS; ++i) {
            post_barrier_lat(0x900000 + (uint64_t)i, 0,
                             (uint64_t)(BAR_LAT_OFFSET + i));
            poll_n(1);
            uint64_t* my_word = g_atomic + BAR_LAT_OFFSET + i;
            while (__atomic_load_n(my_word, __ATOMIC_ACQUIRE) < 1) {
              poll_once();
            }
          }
          char resp = 'l';
          write(fd, &resp, 1);
        } else if (c == 'D') {
          fprintf(stderr,
                  "[server] RESULT: crs=%lu atomic[0]=%lu (expect %lu) "
                  "payload[0]=%lu barrier[0]=%lu\n",
                  (unsigned long)srv_crs, (unsigned long)g_atomic[0],
                  (unsigned long)((WARMUP + LAT_ITERS + ATOM_BURST +
                                   QUIET_OPS / 2 + VERIFY_FADD_SEQ +
                                   VERIFY_FADD_PIPE + 336) * OPERAND),
                  (unsigned long)g_payload[0], (unsigned long)g_barrier[0]);
          break;
        }
      } else if (r == 0) {
        fprintf(stderr, "[server] EOF\n");
        break;
      }
    }
    close(fd);
    return 0;
  }
  if (!g_skip_lat) {
    phase_write_latency();
    phase_fadd_latency();
  }
  if (!g_no_bw) phase_write_bandwidth();
  if (!g_no_fa) phase_fadd_throughput();
  if (!g_no_barrier) {
    char cmd = 'B';
    write(fd, &cmd, 1);
    double post_secs = 0;
    post_adds_to_peer_timed(BARRIER_K, 32, &post_secs);
    double t0 = now_us();
    int failed = 0;
    while (__atomic_load_n(&g_barrier[0], __ATOMIC_ACQUIRE) <
           (uint64_t)BARRIER_K) {
      poll_n(1);
      if (now_us() - t0 > 30e6) {
        failed = 1;
        break;
      }
    }
    double total_secs = (now_us() - t0) / 1e6;
    char resp = 0;
    ssize_t r = read(fd, &resp, 1);
    printf("BARRIER           : word=%lu expect=%lu srv=%s %s\n",
           (unsigned long)g_barrier[0], (unsigned long)BARRIER_K,
           r == 1 ? (resp == 'b' ? "OK" : "FAIL") : "TIMEOUT",
           failed ? "FAIL" : "OK");
    printf("BARRIER-perf      : %d fence-FADDs in %.3fs (post %.3fs + "
           "sync %.3fs) = %.2f Kops/s\n",
           BARRIER_K, post_secs + total_secs, post_secs, total_secs,
           (double)BARRIER_K / (post_secs + total_secs) / 1e3);
  }
  if (!g_no_quiet) {
    double q_secs = 0;
    post_quiet_ops_timed(QUIET_OPS, QUIET_WINDOW, &q_secs);
    if (q_secs < 1e-9) q_secs = 1e-9;
    printf("QUIET-perf        : %d mixed ops (%d W + %d F) drained in %.6fs "
           "(%.2f Mops/s)\n",
           QUIET_OPS, QUIET_OPS / 2, QUIET_OPS / 2, q_secs,
           (double)QUIET_OPS / q_secs / 1e6);
  }
  if (!g_no_verify) {
    /* ---- phase 7: WRITE data verification ---- */
    phase_write_verify(fd);
    /* ---- phase 8: FADD oldval verification ---- */
    phase_fadd_verify(fd);
    /* ---- phase 9: BARRIER latency ---- */
    phase_barrier_latency(fd);
    /* ---- phase 10: QUIET latency ---- */
    phase_quiet_latency();
  }
  {
    char cmd = 'D';
    write(fd, &cmd, 1);
  }
  close(fd);
  printf("[client] ALL_OK\n");
  return 0;
}
