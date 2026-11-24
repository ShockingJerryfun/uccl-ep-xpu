/* Standalone URMA smoke test (config extracted from official urma_sample):
 * jfce -> jfc -> jfr -> jetty -> register seg -> rjetty/seg ctx -> loopback
 * import -> post a WRITE and an FADD against our own jetty. */
#include <ub/umdk/urma/urma_api.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static void check(int rc, char const* what) {
  if (rc != URMA_SUCCESS) {
    fprintf(stderr, "FAIL: %s rc=%d (%s)\n", what, rc, strerror(rc));
    exit(1);
  }
}
int main(void) {
  check(urma_init(nullptr), "urma_init");
  int num_devices = 0;
  urma_device_t** devs = urma_get_device_list(&num_devices);
  if (!devs || num_devices == 0) {
    printf("NO_DEVICES\n");
    return 0;
  }
  printf("devices=%d\n", num_devices);
  char const* want = getenv("URMA_DEV");
  urma_device_t* dev = devs[0];
  if (want) {
    urma_device_t* d = urma_get_device_by_name((char*)want);
    if (!d) {
      printf("DEVICE_NOT_FOUND: %s\n", want);
      return 1;
    }
    dev = d;
  }
  printf("using device: %s\n", dev->name);
  urma_context_t* ctx = urma_create_context(dev, 0);
  if (!ctx) {
    printf("CREATE_CONTEXT_FAILED\n");
    return 1;
  }
  printf("context=OK\n");
  urma_jfce_t* jfce = urma_create_jfce(ctx);
  if (!jfce) {
    printf("CREATE_JFCE_FAILED\n");
    return 1;
  }
  printf("jfce=OK\n");
  urma_jfc_cfg_t jfc_cfg = {};
  jfc_cfg.depth = 4096;
  jfc_cfg.jfce = jfce;
  urma_jfc_t* jfc = urma_create_jfc(ctx, &jfc_cfg);
  if (!jfc) {
    printf("CREATE_JFC_FAILED\n");
    return 1;
  }
  printf("jfc=OK\n");
  urma_jfr_cfg_t jfr_cfg = {};
  jfr_cfg.depth = 1;
  jfr_cfg.trans_mode = URMA_TM_RC;
  jfr_cfg.max_sge = 1;
  jfr_cfg.jfc = jfc;
  jfr_cfg.token_value.token = 0;
  urma_jfr_t* jfr = urma_create_jfr(ctx, &jfr_cfg);
  if (!jfr) {
    printf("CREATE_JFR_FAILED errno=%d\n", errno);
    return 1;
  }
  printf("jfr=OK\n");
  urma_jetty_cfg_t jetty_cfg = {};
  jetty_cfg.flag.bs.share_jfr = 1; /* URMA_SHARE_JFR */
  jetty_cfg.jfs_cfg.depth = 1024;
  jetty_cfg.jfs_cfg.trans_mode = URMA_TM_RC;
  jetty_cfg.jfs_cfg.max_sge = 1;
  jetty_cfg.jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
  jetty_cfg.jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
  jetty_cfg.jfs_cfg.jfc = jfc;
  jetty_cfg.shared.jfr = jfr;
  jetty_cfg.shared.jfc = jfc;
  urma_jetty_t* jetty = urma_create_jetty(ctx, &jetty_cfg);
  if (!jetty) {
    printf("CREATE_JETTY_FAILED errno=%d (%s)\n", errno, strerror(errno));
    return 1;
  }
  printf("jetty=OK (UM, share_jfr)\n");
  static uint64_t buf[64] __attribute__((aligned(4096)));
  urma_seg_cfg_t seg_cfg = {};
  seg_cfg.va = (uint64_t)buf;
  seg_cfg.len = sizeof(buf);
  seg_cfg.flag.bs.access = URMA_ACCESS_READ | URMA_ACCESS_WRITE |
                           URMA_ACCESS_ATOMIC;
  urma_target_seg_t* tseg = urma_register_seg(ctx, &seg_cfg);
  if (!tseg) {
    printf("REGISTER_SEG_FAILED\n");
    return 1;
  }
  printf("register_seg=OK va=0x%lx\n", (unsigned long)seg_cfg.va);
  urma_rjetty_t* rjetty = nullptr;
  uint32_t rjetty_len = 0;
  check(urma_get_rjetty(jetty, &rjetty, &rjetty_len), "urma_get_rjetty");
  printf("rjetty=OK len=%u\n", rjetty_len);
  urma_seg_t* seg_ctx = nullptr;
  uint32_t seg_len = 0;
  check(urma_get_seg_ctx(tseg, &seg_ctx, &seg_len), "urma_get_seg_ctx");
  printf("seg_ctx=OK len=%u\n", seg_len);
  /* Loopback: import our own jetty+seg (same process, same device). */
  urma_token_t token = {};
  urma_target_jetty_t* tjetty = urma_import_jetty(ctx, rjetty, &token);
  if (!tjetty) {
    printf("IMPORT_JETTY_FAILED errno=%d (%s)\n", errno, strerror(errno));
    return 1;
  }
  printf("import_jetty=OK\n");
  urma_import_seg_flag_t iflag = {};
  iflag.bs.mapping = URMA_SEG_MAPPED;
  iflag.bs.access = URMA_ACCESS_WRITE | URMA_ACCESS_ATOMIC;
  urma_target_seg_t* itseg = urma_import_seg(ctx, seg_ctx, &token, 0, iflag);
  if (!itseg) {
    printf("IMPORT_SEG_FAILED errno=%d (%s)\n", errno, strerror(errno));
    return 1;
  }
  printf("import_seg=OK mva=0x%lx ubva.va=0x%lx\n",
         (unsigned long)itseg->mva, (unsigned long)itseg->seg.ubva.va);
  /* Post a loopback WRITE and an FADD, then poll the JFC. */
  buf[0] = 0;
  urma_jfs_wr_t wr = {};
  urma_sge_t src = {};
  urma_sge_t dst = {};
  urma_sge_t dst2 = {};
  src.addr = (uint64_t)&buf[8];
  src.len = 8;
  src.tseg = tseg;
  dst.addr = itseg->mva ? itseg->mva : itseg->seg.ubva.va;
  dst.len = 8;
  dst.tseg = itseg;
  wr.opcode = URMA_OPC_WRITE;
  wr.flag.bs.complete_enable = 1;
  wr.flag.bs.place_order = URMA_STRONG_ORDER;
  wr.tjetty = tjetty;
  wr.user_ctx = 0x1111;
  wr.rw.src.sge = &src;
  wr.rw.src.num_sge = 1;
  wr.rw.dst.sge = &dst;
  wr.rw.dst.num_sge = 1;
  urma_jfs_wr_t* bad = nullptr;
  int rc = urma_post_jetty_send_wr(jetty, &wr, &bad);
  printf("post_write rc=%d\n", rc);
  static uint64_t old_val[8] __attribute__((aligned(4096)));
  urma_seg_cfg_t old_cfg = {};
  old_cfg.va = (uint64_t)old_val;
  old_cfg.len = sizeof(old_val);
  old_cfg.flag.bs.access = URMA_ACCESS_LOCAL_ONLY;
  urma_target_seg_t* old_tseg = urma_register_seg(ctx, &old_cfg);
  urma_sge_t src2 = {};
  src2.addr = (uint64_t)&old_val[0];
  src2.len = 8;
  src2.tseg = old_tseg;
  dst2.addr = (itseg->mva ? itseg->mva : itseg->seg.ubva.va) + 8;
  dst2.len = 8;
  dst2.tseg = itseg;
  urma_jfs_wr_t awr = {};
  awr.opcode = URMA_OPC_FADD;
  awr.flag.bs.complete_enable = 1;
  awr.flag.bs.place_order = URMA_STRONG_ORDER;
  awr.tjetty = tjetty;
  awr.user_ctx = 0x2222;
  awr.faa.dst = &dst2;
  awr.faa.src = &src2;
  awr.faa.operand = 42;
  rc = urma_post_jetty_send_wr(jetty, &awr, &bad);
  printf("post_fadd rc=%d\n", rc);
  urma_cr_t crs[16];
  for (int spin = 0; spin < 200; ++spin) {
    int n = urma_poll_jfc(jfc, 16, crs);
    if (n > 0) {
      for (int i = 0; i < n; ++i) {
        printf("cr: user_ctx=0x%llx status=%d\n",
               (unsigned long long)crs[i].user_ctx, (int)crs[i].status);
      }
      if (n >= 2) break;
    } else {
      usleep(50000);
    }
  }
  printf("buf[0]=%lu buf[1]=%lu (expect 0x1111111111111111? no: src val / +42)\n",
         (unsigned long)buf[0], (unsigned long)buf[1]);
  (void)urma_unimport_seg(itseg);
  (void)urma_unimport_jetty(tjetty);
  urma_put_rjetty(rjetty);
  urma_put_seg_ctx(seg_ctx);
  (void)urma_unregister_seg(old_tseg);
  (void)urma_unregister_seg(tseg);
  (void)urma_delete_jetty(jetty);
  (void)urma_delete_jfr(jfr);
  (void)urma_delete_jfc(jfc);
  (void)urma_delete_jfce(jfce);
  (void)urma_delete_context(ctx);
  (void)urma_uninit();
  urma_free_device_list(devs);
  printf("ALL_OK\n");
  return 0;
}
