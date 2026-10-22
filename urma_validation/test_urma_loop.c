/* Two-process URMA loopback test mirroring UCCL's bootstrap:
 * TCP-exchange rjetty/seg blobs, import, post WRITE + FADD, poll, verify. */
#include <ub/umdk/urma/urma_api.h>
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#define PORT 18715
static void check(int rc, char const* what) {
  if (rc != URMA_SUCCESS) {
    fprintf(stderr, "FAIL: %s rc=%d (%s)\n", what, rc, strerror(rc));
    exit(1);
  }
}
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
static uint64_t g_main[128] __attribute__((aligned(4096)));
static uint64_t g_atomic[16] __attribute__((aligned(4096)));
static uint64_t g_oldval[16] __attribute__((aligned(4096)));
int main(int argc, char** argv) {
  int is_server = (argc > 1 && strcmp(argv[1], "-s") == 0);
  check(urma_init(nullptr), "urma_init");
  int nd = 0;
  urma_device_t** devs = urma_get_device_list(&nd);
  urma_device_t* dev = urma_get_device_by_name((char*)"udmac0d1e2");
  if (!dev) dev = devs[0];
  urma_context_t* ctx = urma_create_context(dev, 0);
  if (!ctx) { printf("CTX_FAIL\n"); return 1; }
  urma_jfce_t* jfce = urma_create_jfce(ctx);
  if (!jfce) { printf("JFCE_FAIL\n"); return 1; }
  urma_jfc_cfg_t jfc_cfg = {};
  jfc_cfg.depth = 4096;
  jfc_cfg.jfce = jfce;
  urma_jfc_t* jfc = urma_create_jfc(ctx, &jfc_cfg);
  if (!jfc) { printf("JFC_FAIL\n"); return 1; }
  urma_jfr_cfg_t jfr_cfg = {};
  jfr_cfg.depth = 1;
  jfr_cfg.trans_mode = URMA_TM_RC;
  jfr_cfg.max_sge = 1;
  jfr_cfg.jfc = jfc;
  urma_jfr_t* jfr = urma_create_jfr(ctx, &jfr_cfg);
  if (!jfr) { printf("JFR_FAIL\n"); return 1; }
  urma_jetty_cfg_t jcfg = {};
  jcfg.flag.bs.share_jfr = 1;
  jcfg.jfs_cfg.depth = 1024;
  jcfg.jfs_cfg.trans_mode = URMA_TM_RC;
  jcfg.jfs_cfg.max_sge = 1;
  jcfg.jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
  jcfg.jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
  jcfg.jfs_cfg.jfc = jfc;
  jcfg.shared.jfr = jfr;
  jcfg.shared.jfc = jfc;
  urma_jetty_t* jetty = urma_create_jetty(ctx, &jcfg);
  if (!jetty) { printf("JETTY_FAIL\n"); return 1; }
  urma_seg_cfg_t mcfg = {};
  mcfg.va = (uint64_t)g_main;
  mcfg.len = sizeof(g_main);
  mcfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
  urma_target_seg_t* main_tseg = urma_register_seg(ctx, &mcfg);
  if (!main_tseg) { printf("REG_MAIN_FAIL\n"); return 1; }
  urma_seg_cfg_t acfg = {};
  acfg.va = (uint64_t)g_atomic;
  acfg.len = sizeof(g_atomic);
  acfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE |
                         URMA_ACCESS_ATOMIC;
  urma_target_seg_t* atomic_tseg = urma_register_seg(ctx, &acfg);
  if (!atomic_tseg) { printf("REG_ATOMIC_FAIL\n"); return 1; }
  urma_seg_cfg_t ocfg = {};
  ocfg.va = (uint64_t)g_oldval;
  ocfg.len = sizeof(g_oldval);
  ocfg.flag.bs.access = URMA_ACCESS_LOCAL_ONLY;
  urma_target_seg_t* old_tseg = urma_register_seg(ctx, &ocfg);
  if (!old_tseg) { printf("REG_OLD_FAIL\n"); return 1; }
  /* Get exchange blobs. */
  urma_rjetty_t* rjetty = nullptr;
  uint32_t rjetty_len = 0;
  check(urma_get_rjetty(jetty, &rjetty, &rjetty_len), "get_rjetty");
  urma_seg_t* mseg = nullptr;
  uint32_t mseg_len = 0;
  check(urma_get_seg_ctx(main_tseg, &mseg, &mseg_len), "get_seg(main)");
  urma_seg_t* aseg = nullptr;
  uint32_t aseg_len = 0;
  check(urma_get_seg_ctx(atomic_tseg, &aseg, &aseg_len), "get_seg(atomic)");
  printf("[%s] blobs: rjetty=%u mseg=%u aseg=%u\n",
         is_server ? "server" : "client", rjetty_len, mseg_len, aseg_len);
  /* TCP exchange. */
  int fd = is_server ? tcp_listen_accept() : tcp_connect();
  printf("[%s] tcp connected\n", is_server ? "server" : "client");
  urma_rjetty_t* r_rjetty = nullptr;
  urma_seg_t* r_mseg = nullptr;
  urma_seg_t* r_aseg = nullptr;
  if (is_server) {
    send_blob(fd, rjetty, rjetty_len);
    send_blob(fd, mseg, mseg_len);
    send_blob(fd, aseg, aseg_len);
    r_rjetty = (urma_rjetty_t*)recv_blob(fd);
    r_mseg = (urma_seg_t*)recv_blob(fd);
    r_aseg = (urma_seg_t*)recv_blob(fd);
  } else {
    send_blob(fd, rjetty, rjetty_len);
    send_blob(fd, mseg, mseg_len);
    send_blob(fd, aseg, aseg_len);
    r_rjetty = (urma_rjetty_t*)recv_blob(fd);
    r_mseg = (urma_seg_t*)recv_blob(fd);
    r_aseg = (urma_seg_t*)recv_blob(fd);
  }
  printf("[%s] exchange done\n", is_server ? "server" : "client");
  urma_token_t token = {};
  urma_target_jetty_t* tjetty = urma_import_jetty(ctx, r_rjetty, &token);
  if (!tjetty) {
    printf("[%s] IMPORT_JETTY_FAILED errno=%d\n", is_server ? "server" : "client", errno);
    return 1;
  }
  printf("[%s] import_jetty=OK\n", is_server ? "server" : "client");
  /* RC mode requires binding the local jetty to the imported target. */
  check(urma_bind_jetty(jetty, tjetty), "urma_bind_jetty");
  printf("[%s] bind_jetty=OK\n", is_server ? "server" : "client");
  urma_import_seg_flag_t iflag = {};
  iflag.bs.mapping = URMA_SEG_MAPPED;
  iflag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE;
  urma_target_seg_t* rmain = urma_import_seg(ctx, r_mseg, &token, 0, iflag);
  if (!rmain) { printf("[%s] IMPORT_MAIN_FAILED\n", is_server ? "server" : "client"); return 1; }
  urma_target_seg_t* ratomic = urma_import_seg(ctx, r_aseg, &token, 0, iflag);
  if (!ratomic) { printf("[%s] IMPORT_ATOMIC_FAILED\n", is_server ? "server" : "client"); return 1; }
  printf("[%s] import_segs=OK (mva=0x%lx va=0x%lx)\n", is_server ? "server" : "client",
         (unsigned long)rmain->mva, (unsigned long)rmain->seg.ubva.va);
  /* Post WRITE (magic) + FADD (+7) to the peer. */
  g_main[0] = is_server ? 0xAAAABBBBCCCCDDDDULL : 0x1111222233334444ULL;
  uint64_t peer_magic =
       is_server ? 0x1111222233334444ULL : 0xAAAABBBBCCCCDDDDULL;
  uint64_t base = rmain->mva ? rmain->mva : rmain->seg.ubva.va;
  urma_sge_t src = {}, dst = {}, wsrc = {}, wdst = {};
  src.addr = (uint64_t)&g_main[0]; /* within registered main segment */
  src.len = 8;
  src.tseg = main_tseg;
  dst.addr = base + 8; /* peer's g_main[1] */
  dst.len = 8;
  dst.tseg = rmain;
  urma_jfs_wr_t* bad = nullptr;
  urma_jfs_wr_t wwr = {};
  wwr.opcode = URMA_OPC_WRITE;
  wwr.flag.bs.complete_enable = 1;
  wwr.flag.bs.place_order = URMA_STRONG_ORDER;
  wwr.tjetty = tjetty;
  wwr.user_ctx = 0x100;
  wwr.rw.src.sge = &src;
  wwr.rw.src.num_sge = 1;
  wwr.rw.dst.sge = &dst;
  wwr.rw.dst.num_sge = 1;
  int rc = urma_post_jetty_send_wr(jetty, &wwr, &bad);
  printf("[%s] post_write rc=%d\n", is_server ? "server" : "client", rc);
  uint64_t atomic_base = ratomic->mva ? ratomic->mva : ratomic->seg.ubva.va;
  wsrc.addr = (uint64_t)&g_oldval[0];
  wsrc.len = 8;
  wsrc.tseg = old_tseg;
  wdst.addr = atomic_base; /* peer's g_atomic[0] */
  wdst.len = 8;
  wdst.tseg = ratomic;
  urma_jfs_wr_t awr = {};
  awr.opcode = URMA_OPC_FADD;
  awr.flag.bs.complete_enable = 1;
  awr.flag.bs.place_order = URMA_STRONG_ORDER;
  awr.tjetty = tjetty;
  awr.user_ctx = 0x200;
  awr.faa.dst = &wdst;
  awr.faa.src = &wsrc;
  awr.faa.operand = 7;
  rc = urma_post_jetty_send_wr(jetty, &awr, &bad);
  printf("[%s] post_fadd rc=%d\n", is_server ? "server" : "client", rc);
  /* Poll for our own 2 completions and wait for peer's ops to land. */
  urma_cr_t crs[16];
  int total = 0;
  for (int spin = 0; spin < 400 && total < 2; ++spin) {
    int n = urma_poll_jfc(jfc, 16, crs);
    if (n > 0) {
      for (int i = 0; i < n; ++i) {
        printf("[%s] cr user_ctx=0x%llx status=%d\n", is_server ? "server" : "client",
               (unsigned long long)crs[i].user_ctx, (int)crs[i].status);
      }
      total += n;
    } else {
      usleep(50000);
    }
  }
  for (int w = 0; w < 50; ++w) {
    if (g_main[1] != 0 && g_atomic[0] != 0) break;
    usleep(100000);
  }
  printf("[%s] RESULT: g_main[1]=0x%lx (want 0x%lx) g_atomic[0]=%lu (want 7)\n",
         is_server ? "server" : "client", (unsigned long)g_main[1],
         (unsigned long)peer_magic, (unsigned long)g_atomic[0]);
  (void)urma_unimport_seg(ratomic);
  (void)urma_unimport_seg(rmain);
  (void)urma_unimport_jetty(tjetty);
  urma_put_rjetty(rjetty);
  urma_put_seg_ctx(mseg);
  urma_put_seg_ctx(aseg);
  urma_free_device_list(devs);
  close(fd);
  if (g_main[1] == peer_magic && g_atomic[0] == 7) {
    printf("[%s] ALL_OK\n", is_server ? "server" : "client");
    return 0;
  }
  return 1;
}
