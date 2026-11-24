# URMA Transport — Validation Notes & Working Recipe
Date: 2026-10-12 · Host: 141.61.84.247 (openEuler aarch64, A100, netlab kernel)
## Summary
`test_urma_loop.c` performs a full two-process URMA data-path loopback
(server + client over TCP bootstrap, mirroring UCCL's own bootstrap):
* exchange `urma_rjetty_t` / `urma_seg_t` blobs over TCP,
* `urma_import_jetty` + `urma_bind_jetty` + `urma_import_seg`,
* `URMA_OPC_WRITE` and `URMA_OPC_FADD` posted **both** directions,
* completions polled via `urma_poll_jfc`.
**Result: ALL_OK both directions** — WRITE carries the peer's magic value,
FADD adds 7 to the peer's atomic buffer and returns old value, all CEs
report `URMA_CR_SUCCESS` (status=0).
```
[server] post_write rc=0 / post_fadd rc=0
[server] cr user_ctx=0x100 status=0   (WRITE)
[server] cr user_ctx=0x200 status=0   (FADD)
[server] RESULT: g_main[1]=0x1111222233334444 g_atomic[0]=7
[client] RESULT: g_main[1]=0xaaaabbbbccccdddd g_atomic[0]=7
```
## Working object-creation recipe (per process)
```c
urma_init(nullptr);                       /* must run BEFORE get_device_list */
urma_create_context(dev, eid_index);      /* udma device, NOT bonding on this host */
jfce = urma_create_jfce(ctx);
urma_jfc_cfg_t jfc_cfg = { .depth = 4096, .jfce = jfce };
jfc = urma_create_jfc(ctx, &jfc_cfg);
urma_jfr_cfg_t jfr_cfg = { .depth = 1, .trans_mode = URMA_TM_RC,
                           .max_sge = 1, .jfc = jfc, .token_value.token = 0 };
jfr = urma_create_jfr(ctx, &jfr_cfg);
urma_jetty_cfg_t jcfg = {};
jcfg.flag.bs.share_jfr = 1;               /* REQUIRED on this driver */
jcfg.jfs_cfg.depth = 1024;
jcfg.jfs_cfg.trans_mode = URMA_TM_RC;     /* RC; UM import is rejected */
jcfg.jfs_cfg.max_sge = 1;
jcfg.jfs_cfg.rnr_retry = URMA_TYPICAL_RNR_RETRY;
jcfg.jfs_cfg.err_timeout = URMA_TYPICAL_ERR_TIMEOUT;
jcfg.jfs_cfg.jfc = jfc;
jcfg.shared.jfr = jfr;
jcfg.shared.jfc = jfc;
jetty = urma_create_jetty(ctx, &jcfg);
```
## Verified facts on this platform
1. **`urma_init()` must precede `urma_get_device_list()`** — otherwise the
   device list is empty (7 devices found after init).
2. **`jfce` is required** in `jfc_cfg` for a usable JFC.
3. **`share_jfr = 1` + a real JFR** is required by `urma_create_jetty`;
   without it the call fails with `EINVAL` (22) in user space.
   JFR must use `token_value.token = 0` and works with `depth = 1`.
4. **Trans mode must be `URMA_TM_RC`** for `urma_import_jetty` on the udma
   driver: the rjetty driver-extension dword at offset 40 is `1`, and
   liburma's UM import path rejects that combination client-side
   (returns NULL without errno). RC import succeeds. Env override:
   `UCCL_URMA_TM` (`rc`/`um`).
5. **`urma_bind_jetty` is required after import** for RC before posting.
6. **Segment access bits**: register/import with
   `URMA_ACCESS_READ | URMA_ACCESS_WRITE`; FADD-target segments additionally
   need `URMA_ACCESS_ATOMIC` **both at register and at import** time.
   The FADD old-value writeback segment also needs `READ|WRITE`
   (plain `LOCAL_ONLY` yields `URMA_CR_LOC_ACCESS_ERR`).
7. **Blob sizes**: `urma_get_rjetty` → 48 B on udma devices but **651 B on
   bonding_dev_0**; `urma_get_seg_ctx` → up to **1028 B**. The connection
   blob fields therefore need `kMaxUrmaBlob = 2048`.
8. **MVA**: on udma devices the imported segment exposes `mva = 0`, so the
   remote address must come from `tseg->seg.ubva.va` (+offset fallback is
   required).
9. **Device selection**: `bonding_dev_0` advertises only a placeholder EID
   (`42454944...`, "BEID"); real EIDs live on `udmac0d1e2/3/4` etc.
   Default selection skips bonding devices; use `UCCL_URMA_DEV_NAME` to
   pin explicitly.
## Files
* `test_urma_loop.c` — two-process loopback (validated, ALL_OK).
* `test_urma_smoke.c` — single-process object-lifecycle smoke test.
* `urma_transport.cpp/.hpp` — UCCL transport implementing this recipe.
## Remaining limitations
* GPU-dependent UCCL EP end-to-end tests remain deferred (machine currently
  runs the netlab kernel without a working nvidia driver).
* The official `urma_sample` hangs in its EID/TCP exchange phase on this
  host (single-node, no fabric config); our loop bypasses that by
  exchanging blobs directly.
