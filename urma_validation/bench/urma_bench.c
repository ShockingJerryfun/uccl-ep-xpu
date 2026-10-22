/* URMA micro-benchmark: WRITE / FADD / BARRIER / QUIET at the URMA
 * transport level (no GPU involved).
 *
 * Server: ./urma_bench -s
 * Client: ./urma_bench
 *
 * Phases (client drives, server passive except barrier/quiet handshake):
 *   1. WRITE ping-pong latency, 8B, 1000 iters (100 warmup)
 *   2. FADD  ping-pong latency, 8B, 1000 iters (100 warmup)
 *   3. WRITE bandwidth, 32KB x 512, window 32
 *   4. FADD throughput, 4096 ops, window 128
 *   5. BARRIER: bidirectional barrier_atomic_add (fence=ENABLE) x1000,
 *      each side spins its own local barrier word to K
 *   6. QUIET: mixed burst 256 WRITE + 256 FADD, then poll-until-drained;
 *      server verifies atomic[0] == expected, client verifies CR count
 */
#include <ub/umdk/urma/urma_api.h>
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
static void check_urma(urma_status_t rc, char const* what) {
  if (rc != URMA_SUCCESS) {
    fprintf(stderr, "URMA FAIL: %s rc=%d (%s)\n", what, (int)rc,
            strerror((int)rc));
    exit(1);
  }
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
/* ---- TCP bootstrap ---- */
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
/* ---- URMA objects ---- */
static urma_context_t* g_ctx;
static urma_jfce_t* g_jfce;
static urma_jfc_t* g_jfc;
static urma_jfr_t* g_jfr;
static urma_jetty_t* g_jetty;
static uint64_t* g_payload;
static uint64_t* g_atomic;
static uint64_t* g_oldval;
static uint64_t* g_barrier; /* local barrier words, atomically added by peer */
static urma_target_seg_t* g_payload_tseg;
static urma_target_seg_t* g_atomic_tseg;
static urma_target_seg_t* g_oldval_tseg;
static urma_target_seg_t* g_barrier_tseg;
static urma_rjetty_t* g_rjetty;
static uint32_t g_rjetty_len;
static urma_seg_t* g_payload_seg;
static uint32_t g_payload_seg_len;
static urma_seg_t* g_atomic_seg;
static uint32_t g_atomic_seg_len;
static urma_seg_t* g_barrier_seg;
static uint32_t g_barrier_seg_len;
static urma_target_jetty_t* g_peer_tjetty;
static urma_target_seg_t* g_peer_payload;
static urma_target_seg_t* g_peer_atomic;
static urma_target_seg_t* g_peer_barrier;
static uint64_t remote_addr(urma_target_seg_t const* t, uint64_t off) {
  return t->mva ? t->mva + off : t->seg.ubva.va + off;
}
static void setup_urma(int is_server) {
  check_urma(urma_init(NULL), "urma_init");
  int nd = 0;
  urma_device_t** devs = urma_get_device_list(&nd);
  char const* want = getenv("URMA_DEV_NAME");
  urma_device_t* dev = NULL;
  if (want) dev = urma_get_device_by_name((char*)want);
  if (!dev && devs && nd > 0) {
    for (int i = 0; i < nd; ++i) {
      if (devs[i] && strstr(devs[i]->name, "bonding") == NULL) {
        dev = devs[i];
        break;
      }
    }
    if (!dev) dev = devs[0];
  }
  if (!dev) { printf("NO_DEVICE\n"); exit(1); }
  g_ctx = urma_create_context(dev, 0);
  if (!g_ctx) { printf("CTX_FAIL\n"); exit(1); }
  g_jfce = urma_create_jfce(g_ctx);
  if (!g_jfce) { printf("JFCE_FAIL\n"); exit(1); }
  urma_jfc_cfg_t jfc_cfg = {};
  jfc_cfg.depth = 4096;
  jfc_cfg.jfce = g_jfce;
  g_jfc = urma_create_jfc(g_ctx, &jfc_cfg);
  if (!g_jfc) { printf("JFC_FAIL\n"); exit(1); }
  urma_jfr_cfg_t jfr_cfg = {};
  jfr_cfg.depth = 256;
  jfr_cfg.trans_mode = URMA_TM_RC;
  jfr_cfg.max_sge = 1;
  jfr_cfg.jfc = g_jfc;
  jfr_cfg.token_value.token = 0;
  g_jfr = urma_create_jfr(g_ctx, &jfr_cfg);
  if (!g_jfr) { printf("JFR_FAIL\n"); exit(1); }
  urma_jetty_cfg_t jcfg = {};
  jcfg.flag.bs.share_jfr = 1;
  jcfg.jfs_cfg.depth = 1024;
  jcfg.jfs_cfg.trans_mode = URMA_TM_RC;
  jcfg.jfs_cfg.max_sge = 1;
  jcfg.jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
  jcfg.jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
  jcfg.jfs_cfg.jfc = g_jfc;
  jcfg.shared.jfr = g_jfr;
  jcfg.shared.jfc = g_jfc;
  g_jetty = urma_create_jetty(g_ctx, &jcfg);
  if (!g_jetty) { printf("JETTY_FAIL\n"); exit(1); }
  printf("[%s] jetty=OK dev=%s trans_mode=RC\n", is_server ? "server" : "client",
         dev->name);
  if (posix_memalign((void**)&g_payload, 4096, PAYLOAD_LEN) != 0) {
    printf("ALLOC_FAIL\n");
    exit(1);
  }
  memset(g_payload, 0, PAYLOAD_LEN);
  if (posix_memalign((void**)&g_atomic, 4096, ATOM_BUF) != 0) {
    printf("ALLOC_FAIL\n");
    exit(1);
  }
  memset(g_atomic, 0, ATOM_BUF);
  if (posix_memalign((void**)&g_oldval, 4096, OLDVAL_LEN) != 0) {
    printf("ALLOC_FAIL\n");
    exit(1);
  }
  memset(g_oldval, 0, OLDVAL_LEN);
  if (posix_memalign((void**)&g_barrier, 4096, BARRIER_WORDS * 8) != 0) {
    printf("ALLOC_FAIL\n");
    exit(1);
  }
  memset(g_barrier, 0, BARRIER_WORDS * 8);
  urma_seg_cfg_t pcfg = {};
  pcfg.va = (uint64_t)g_payload;
  pcfg.len = PAYLOAD_LEN;
  pcfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
  g_payload_tseg = urma_register_seg(g_ctx, &pcfg);
  if (!g_payload_tseg) { printf("REG_PAYLOAD_FAIL\n"); exit(1); }
  urma_seg_cfg_t acfg = {};
  acfg.va = (uint64_t)g_atomic;
  acfg.len = ATOM_BUF;
  acfg.flag.bs.access =
      URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
  g_atomic_tseg = urma_register_seg(g_ctx, &acfg);
  if (!g_atomic_tseg) { printf("REG_ATOMIC_FAIL\n"); exit(1); }
  urma_seg_cfg_t ocfg = {};
  ocfg.va = (uint64_t)g_oldval;
  ocfg.len = OLDVAL_LEN;
  ocfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
  g_oldval_tseg = urma_register_seg(g_ctx, &ocfg);
  if (!g_oldval_tseg) { printf("REG_OLDVAL_FAIL\n"); exit(1); }
  urma_seg_cfg_t bcfg = {};
  bcfg.va = (uint64_t)g_barrier;
  bcfg.len = BARRIER_WORDS * 8;
  bcfg.flag.bs.access =
      URMA_ACCESS_READ | URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
  g_barrier_tseg = urma_register_seg(g_ctx, &bcfg);
  if (!g_barrier_tseg) { printf("REG_BARRIER_FAIL\n"); exit(1); }
  check_urma(urma_get_rjetty(g_jetty, &g_rjetty, &g_rjetty_len), "get_rjetty");
  check_urma(urma_get_seg_ctx(g_payload_tseg, &g_payload_seg, &g_payload_seg_len),
             "get_seg(payload)");
  check_urma(urma_get_seg_ctx(g_atomic_tseg, &g_atomic_seg, &g_atomic_seg_len),
             "get_seg(atomic)");
  check_urma(urma_get_seg_ctx(g_barrier_tseg, &g_barrier_seg, &g_barrier_seg_len),
             "get_seg(barrier)");
}
static void connect_peer(int fd) {
  send_blob(fd, g_rjetty, g_rjetty_len);
  send_blob(fd, g_payload_seg, g_payload_seg_len);
  send_blob(fd, g_atomic_seg, g_atomic_seg_len);
  send_blob(fd, g_barrier_seg, g_barrier_seg_len);
  urma_rjetty_t* peer_rjetty = (urma_rjetty_t*)recv_blob(fd);
  urma_seg_t* peer_payload_seg = (urma_seg_t*)recv_blob(fd);
  urma_seg_t* peer_atomic_seg = (urma_seg_t*)recv_blob(fd);
  urma_seg_t* peer_barrier_seg = (urma_seg_t*)recv_blob(fd);
  urma_token_t token = {};
  g_peer_tjetty = urma_import_jetty(g_ctx, peer_rjetty, &token);
  if (!g_peer_tjetty) { printf("IMPORT_JETTY_FAIL\n"); exit(1); }
  check_urma(urma_bind_jetty(g_jetty, g_peer_tjetty), "bind_jetty");
  urma_import_seg_flag_t flag = {};
  flag.bs.mapping = URMA_SEG_MAPPED;
  flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
  g_peer_payload = urma_import_seg(g_ctx, peer_payload_seg, &token, 0, flag);
  if (!g_peer_payload) { printf("IMPORT_PAYLOAD_FAIL\n"); exit(1); }
  urma_import_seg_flag_t aflag = flag;
  aflag.bs.access |= URMA_ACCESS_ATOMIC;
  g_peer_atomic = urma_import_seg(g_ctx, peer_atomic_seg, &token, 0, aflag);
  if (!g_peer_atomic) { printf("IMPORT_ATOMIC_FAIL\n"); exit(1); }
  urma_import_seg_flag_t bflag = aflag;
  g_peer_barrier = urma_import_seg(g_ctx, peer_barrier_seg, &token, 0, bflag);
  if (!g_peer_barrier) { printf("IMPORT_BARRIER_FAIL\n"); exit(1); }
}
/* ---- posting helpers ---- */
static int g_bw_window = 32;
static int g_bw_size = 8;
static int g_bw_iters = 512;
static int g_no_ce = 0;
#define SGE_MAX 256
static urma_sge_t g_sge_wsrc[SGE_MAX], g_sge_wdst[SGE_MAX];
static urma_sge_t g_sge_asrc[SGE_MAX], g_sge_adst[SGE_MAX];
static urma_sge_t g_sge_bsrc[SGE_MAX], g_sge_bdst[SGE_MAX];
static void post_write(uint64_t wr_id, uint64_t loff, uint64_t roff,
                       uint32_t bytes) {
  uint64_t slot = wr_id % (uint64_t)g_bw_window;
  g_sge_wsrc[slot].addr = (uint64_t)g_payload + loff;
  g_sge_wsrc[slot].len = bytes;
  g_sge_wsrc[slot].tseg = g_payload_tseg;
  g_sge_wdst[slot].addr = remote_addr(g_peer_payload, roff);
  g_sge_wdst[slot].len = bytes;
  g_sge_wdst[slot].tseg = g_peer_payload;
  urma_jfs_wr_t wr = {};
  wr.opcode = URMA_OPC_WRITE;
  wr.flag.bs.complete_enable = 1;
  wr.flag.bs.place_order = URMA_STRONG_ORDER;
  wr.tjetty = g_peer_tjetty;
  wr.user_ctx = wr_id;
  wr.rw.src.sge = &g_sge_wsrc[slot];
  wr.rw.src.num_sge = 1;
  wr.rw.dst.sge = &g_sge_wdst[slot];
  wr.rw.dst.num_sge = 1;
  urma_jfs_wr_t* bad = NULL;
  check_urma(urma_post_jetty_send_wr(g_jetty, &wr, &bad), "post write");
}
static void post_fadd(uint64_t wr_id, uint64_t slot) {
  g_sge_asrc[slot].addr = (uint64_t)g_oldval + slot * 8;
  g_sge_asrc[slot].len = 8;
  g_sge_asrc[slot].tseg = g_oldval_tseg;
  g_sge_adst[slot].addr = remote_addr(g_peer_atomic, 0);
  g_sge_adst[slot].len = 8;
  g_sge_adst[slot].tseg = g_peer_atomic;
  urma_jfs_wr_t wr = {};
  wr.opcode = URMA_OPC_FADD;
  wr.flag.bs.complete_enable = g_no_ce ? 0 : 1;
  wr.flag.bs.place_order = URMA_STRONG_ORDER;
  wr.tjetty = g_peer_tjetty;
  wr.user_ctx = wr_id;
  wr.faa.dst = &g_sge_adst[slot];
  wr.faa.src = &g_sge_asrc[slot];
  wr.faa.operand = OPERAND;
  urma_jfs_wr_t* bad = NULL;
  check_urma(urma_post_jetty_send_wr(g_jetty, &wr, &bad), "post fadd");
}
/* Mirror UrmaTransport::post_barrier_atomic_add: FADD with fence=ENABLE. */
static int g_barrier_fence = 1;
static int g_barrier_k = 1000;
static void post_barrier_add(uint64_t wr_id, uint64_t slot) {
  g_sge_bsrc[slot].addr = (uint64_t)g_oldval + slot * 8;
  g_sge_bsrc[slot].len = 8;
  g_sge_bsrc[slot].tseg = g_oldval_tseg;
  g_sge_bdst[slot].addr = remote_addr(g_peer_barrier, 0);
  g_sge_bdst[slot].len = 8;
  g_sge_bdst[slot].tseg = g_peer_barrier;
  urma_jfs_wr_t wr = {};
  wr.opcode = URMA_OPC_FADD;
  wr.flag.bs.complete_enable = 1;
  wr.flag.bs.place_order = URMA_STRONG_ORDER;
  wr.flag.bs.fence =
      g_barrier_fence ? URMA_FENCE_ENABLE : URMA_FENCE_DISABLE;
  wr.tjetty = g_peer_tjetty;
  wr.user_ctx = wr_id;
  wr.faa.dst = &g_sge_bdst[slot];
  wr.faa.src = &g_sge_bsrc[slot];
  wr.faa.operand = 1;
  urma_jfs_wr_t* bad = NULL;
  check_urma(urma_post_jetty_send_wr(g_jetty, &wr, &bad), "post barrier add");
}
static urma_cr_t g_crs[64];
static void check_crs(int n) {
  for (int i = 0; i < n; ++i) {
    if (g_crs[i].status != URMA_CR_SUCCESS) {
      printf("CE status=%d (NON-SUCCESS)\n", (int)g_crs[i].status);
      exit(1);
    }
  }
}
/* Drain up to 16 CRs; return how many collected. */
static int g_poll_cap = 16;
static int g_skip_lat = 0;
static int g_fa_window = ATOM_WINDOW;
static int g_no_bw = 0;
static int g_no_fa = 0;
static int g_no_barrier = 0;
static int g_no_quiet = 0;
static int g_no_verify = 0;
static char const* g_phase_name = "?";
static double g_phase_t0 = 0;
static uint64_t g_spins = 0;
static uint64_t g_dbg_posted = 0;
static uint64_t g_dbg_collected = 0;
static uint64_t g_dbg_polled = 0;
static int poll_once(void) {
  int n = urma_poll_jfc(g_jfc, g_poll_cap, g_crs);
  if (n < 0) {
    printf("POLL_ERR rc=%d\n", n);
    exit(1);
  }
  check_crs(n);
  return n;
}
static int poll_n(uint32_t want) {
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
    /* back off if starved for a while: let the driver make progress */
    if (g_spins > 1000000 && (g_spins % 10000 == 0)) usleep(100);
    if (t - last_hb > 5e6) {
      fprintf(stderr,
              "SPIN %s: spins=%lu elapsed=%.2fs posted=%lu collected=%lu "
              "polled_total=%lu\n",
              g_phase_name ? g_phase_name : "?", (unsigned long)g_spins,
              (t - g_phase_t0) / 1e6, (unsigned long)g_dbg_posted,
              (unsigned long)g_dbg_collected, (unsigned long)g_dbg_polled);
      last_hb = t;
    }
    if (t - entry > 30e6) {
      fprintf(stderr,
              "STALL %s: spins=%lu elapsed=%.2fs posted=%lu collected=%lu "
              "polled_total=%lu\n",
              g_phase_name ? g_phase_name : "?",
              (unsigned long)g_spins, (t - g_phase_t0) / 1e6,
              (unsigned long)g_dbg_posted, (unsigned long)g_dbg_collected,
              (unsigned long)g_dbg_polled);
      exit(42);
    }
  }
}
/* ---- benchmark phases ---- */
static double g_lat[LAT_ITERS];
static void phase_write_latency(void) {
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
/* Proper outstanding accounting: decrement by the returned batch size. */
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
      if (collected % 64 == 0)
        fprintf(stderr, "bw: crs=%lu i=%d t=%.2fs\n", (unsigned long)collected,
                i, (now_us() - t0) / 1e6);
    }
    post_write((uint64_t)0x300000 + i, 0, (i % 32) * PAYLOAD, g_bw_size);
    inflight++;
  }
  while (inflight > 0) {
    uint64_t got = poll_n(1);
    collected += got;
    inflight -= (int)got;
  }
  double t1 = now_us();
  double secs = (t1 - t0) / 1e6;
  double bytes = (double)g_bw_iters * g_bw_size;
  printf("WRITE-bw %dB x %d w=%d : %.2f GB/s (%.1f MRps, crs=%lu)\n",
         g_bw_size, g_bw_iters, g_bw_window, bytes / secs / 1e9,
         (double)g_bw_iters / secs / 1e6, (unsigned long)collected);
}
static void phase_fadd_throughput(void) {
  g_phase_name = "FADD-thr";
  double t0 = now_us();
  g_phase_t0 = t0;
  g_dbg_posted = 0;
  g_dbg_collected = 0;
  if (g_no_ce || (getenv("URMA_NO_CE") && atoi(getenv("URMA_NO_CE")))) {
    /* data-path-only: no completions requested; server verifies value.
     * JFS slots are never freed without completions, so stay well below
     * the jetty's JFS depth (1024). */
    g_no_ce = 1;
    int const n = 512;
    for (int i = 0; i < n; ++i) {
      post_fadd((uint64_t)0x400000 + i, (i % g_fa_window));
      if ((i + 1) % g_fa_window == 0) usleep(10000);
    }
    g_no_ce = 0;
    sleep(2);
    double secs = (now_us() - t0) / 1e6;
    printf("FADD-throughput    : %d ops posted (no-CE) in %.2fs\n",
           n, secs);
    return;
  }
  int inflight = 0;
  uint64_t collected = 0;
  for (int i = 0; i < ATOM_BURST; ++i) {
    while (inflight >= g_fa_window) {
      uint64_t got = poll_n(1);
      collected += got;
      g_dbg_collected = collected;
      inflight -= (int)got;
      if (collected % 128 == 0)
        fprintf(stderr, "fadd: crs=%lu i=%d t=%.2fs\n", (unsigned long)collected,
                i, (now_us() - t0) / 1e6);
    }
    post_fadd((uint64_t)0x400000 + i, (i % g_fa_window));
    g_dbg_posted = (uint64_t)i + 1;
    inflight++;
  }
  while (inflight > 0) {
    uint64_t got = poll_n(1);
    collected += got;
    g_dbg_collected = collected;
    inflight -= (int)got;
  }
  double t1 = now_us();
  double secs = (t1 - t0) / 1e6;
  printf("FADD-throughput    : %.2f Mops/s (%d ops, crs=%lu)\n",
         (double)ATOM_BURST / secs / 1e6, ATOM_BURST,
         (unsigned long)collected);
}
/* ---- phase 5: BARRIER ---- */
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
/* ---- phase 6: QUIET ---- */
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
/* ---- phase 7: WRITE data verification ---- */
#define VERIFY_SLOTS 32
#define VERIFY_FADD_SEQ 100
#define VERIFY_FADD_PIPE 128
/* ---- BARRIER latency: bidirectional fence-FADD rendezvous ---- */
#define BAR_LAT_ROUNDS 200
#define BAR_LAT_OFFSET 1000 /* qword offset into atomic buffer (fresh region) */
static void post_barrier_lat(uint64_t wr_id, uint64_t slot, uint64_t qw_off) {
  g_sge_bsrc[slot].addr = (uint64_t)g_oldval + slot * 8;
  g_sge_bsrc[slot].len = 8;
  g_sge_bsrc[slot].tseg = g_oldval_tseg;
  g_sge_bdst[slot].addr = remote_addr(g_peer_atomic, qw_off * 8);
  g_sge_bdst[slot].len = 8;
  g_sge_bdst[slot].tseg = g_peer_atomic;
  urma_jfs_wr_t wr = {};
  wr.opcode = URMA_OPC_FADD;
  wr.flag.bs.complete_enable = 1;
  wr.flag.bs.place_order = URMA_STRONG_ORDER;
  wr.flag.bs.fence =
      g_barrier_fence ? URMA_FENCE_ENABLE : URMA_FENCE_DISABLE;
  wr.tjetty = g_peer_tjetty;
  wr.user_ctx = wr_id;
  wr.faa.dst = &g_sge_bdst[slot];
  wr.faa.src = &g_sge_bsrc[slot];
  wr.faa.operand = 1;
  urma_jfs_wr_t* bad = NULL;
  check_urma(urma_post_jetty_send_wr(g_jetty, &wr, &bad), "post barrier lat");
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
      poll_once(); /* keep JFC draining */
    }
    g_bar_lat[i] = now_us() - t0;
  }
  /* wait for server to finish its rounds too */
  char resp = 0;
  read(fd, &resp, 1);
  qsort(g_bar_lat, BAR_LAT_ROUNDS, sizeof(double), cmp_double);
  double sum = 0;
  for (int i = 0; i < BAR_LAT_ROUNDS; ++i) sum += g_bar_lat[i];
  printf("BARRIER-latency    : min=%.2f p50=%.2f p90=%.2f p99=%.2f mean=%.2f "
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
  /* --- Test B: pipelined (window=128) — sum of oldvals must be exact. ---
   * All 128 FADDs in flight at once; individual order is not guaranteed
   * but every value base+0, base+7, ..., base+127*7 must appear exactly
   * once in oldval[0..127].  Check via sum (and final remote value). */
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
  if ((e = getenv("URMA_POLL_CAP"))) g_poll_cap = atoi(e);
  if ((e = getenv("URMA_SKIP_LAT"))) g_skip_lat = atoi(e);
  if ((e = getenv("URMA_FA_WINDOW"))) g_fa_window = atoi(e);
  if ((e = getenv("URMA_BARRIER_FENCE"))) g_barrier_fence = atoi(e);
  if ((e = getenv("URMA_BARRIER_K"))) g_barrier_k = atoi(e);
  if ((e = getenv("URMA_NO_BW"))) g_no_bw = atoi(e);
  if ((e = getenv("URMA_NO_FA"))) g_no_fa = atoi(e);
  if ((e = getenv("URMA_NO_BARRIER"))) g_no_barrier = atoi(e);
  if ((e = getenv("URMA_NO_QUIET"))) g_no_quiet = atoi(e);
  if ((e = getenv("URMA_NO_VERIFY"))) g_no_verify = atoi(e);
  if ((e = getenv("URMA_BW_SIZE"))) g_bw_size = atoi(e);
  if ((e = getenv("URMA_BW_WINDOW"))) g_bw_window = atoi(e);
  setup_urma(is_server);
  int fd = is_server ? tcp_listen_accept() : tcp_connect();
  printf("[%s] tcp connected\n", is_server ? "server" : "client");
  connect_peer(fd);
  printf("[%s] connected\n", is_server ? "server" : "client");
  if (is_server) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
    char c = 0;
    uint64_t srv_crs = 0;
    uint64_t main_iters = 0;
    int barrier_done = 0;
    for (;;) {
      int n = poll_once();
      srv_crs += (uint64_t)n;
      if (++main_iters % 10000 == 0) usleep(50);
      ssize_t r = read(fd, &c, 1);
      if (r == 1) {
        if (c == 'B') {
          /* server's barrier half: adds to client's barrier word */
          post_adds_to_peer(g_barrier_k, 32);
          /* spin own word to K, 30s timeout */
          double t0 = now_us();
          double last_hb = t0;
          uint64_t b_iters = 0;
          int failed = 0;
          while (__atomic_load_n(&g_barrier[0], __ATOMIC_ACQUIRE) <
                 (uint64_t)g_barrier_k) {
            poll_once();
            ++b_iters;
            if (b_iters > 1000000 && (b_iters % 10000 == 0)) usleep(100);
            double t = now_us();
            if (t - last_hb > 5e6) {
              fprintf(stderr, "[server] barrier wait: word=%lu/%d %.1fs\n",
                      (unsigned long)g_barrier[0], g_barrier_k,
                      (t - t0) / 1e6);
              last_hb = t;
            }
            if (t - t0 > 30e6) { failed = 1; break; }
          }
          fprintf(stderr, "[server] barrier word=%lu expect=%d %s\n",
                 (unsigned long)g_barrier[0], (unsigned long)g_barrier_k,
                 failed ? "FAIL" : "OK");
          char resp = failed ? 'f' : 'b';
          write(fd, &resp, 1);
          barrier_done = 1;
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
          fprintf(stderr, "[server] RESULT: crs=%lu atomic[0]=%lu (expect %lu) "
                  "payload[0]=%lu barrier[0]=%lu\n",
                 (unsigned long)srv_crs,
                 (unsigned long)g_atomic[0],
                 (unsigned long)((WARMUP + LAT_ITERS + ATOM_BURST +
                                  QUIET_OPS / 2 + VERIFY_FADD_SEQ +
                                  VERIFY_FADD_PIPE + 336) * OPERAND),
                 (unsigned long)g_payload[0],
                 (unsigned long)g_barrier[0]);
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
    /* ---- phase 5: BARRIER ---- */
    {
      char cmd = 'B';
      write(fd, &cmd, 1);
      double post_secs = 0;
      post_adds_to_peer_timed(g_barrier_k, 32, &post_secs);
      double t0 = now_us();
      int failed = 0;
      while (__atomic_load_n(&g_barrier[0], __ATOMIC_ACQUIRE) <
             (uint64_t)g_barrier_k) {
        poll_n(1);
        if (now_us() - t0 > 30e6) { failed = 1; break; }
      }
      double total_secs = (now_us() - t0) / 1e6;
      char resp = 0;
      ssize_t r = read(fd, &resp, 1);
      printf("BARRIER           : word=%lu expect=%lu srv=%s %s\n",
             (unsigned long)g_barrier[0], (unsigned long)g_barrier_k,
             r == 1 ? (resp == 'b' ? "OK" : "FAIL") : "TIMEOUT",
             failed ? "FAIL" : "OK");
      printf("BARRIER-perf      : %d fence-FADDs in %.3fs (post %.3fs + "
             "sync %.3fs) = %.2f Kops/s\n",
             g_barrier_k, post_secs + total_secs, post_secs, total_secs,
             (double)g_barrier_k / (post_secs + total_secs) / 1e3);
    }
  }
  if (!g_no_quiet) {
    /* ---- phase 6: QUIET ---- */
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
  (void)urma_unimport_seg(g_peer_atomic);
  (void)urma_unimport_seg(g_peer_payload);
  (void)urma_unimport_seg(g_peer_barrier);
  (void)urma_unimport_jetty(g_peer_tjetty);
  urma_put_rjetty(g_rjetty);
  urma_put_seg_ctx(g_payload_seg);
  urma_put_seg_ctx(g_atomic_seg);
  urma_put_seg_ctx(g_barrier_seg);
  (void)urma_unregister_seg(g_oldval_tseg);
  (void)urma_unregister_seg(g_atomic_tseg);
  (void)urma_unregister_seg(g_payload_tseg);
  (void)urma_unregister_seg(g_barrier_tseg);
  (void)urma_delete_jetty(g_jetty);
  (void)urma_delete_jfr(g_jfr);
  (void)urma_delete_jfc(g_jfc);
  (void)urma_delete_jfce(g_jfce);
  (void)urma_delete_context(g_ctx);
  (void)urma_uninit();
  printf("[client] ALL_OK\n");
  return 0;
}
