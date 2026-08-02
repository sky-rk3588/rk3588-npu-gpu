# Mali-G610 (RK3588) at 1 GHz over SCMI/PVTPLL

Mainline ties the GPU core clock to `&cru CLK_GPU` (NPLL parent, tops
out at 850 MHz). The SCMI path goes through the PVTPLL and reaches
1 GHz — but **switching only the core clock bricks the boot**: the old
CRU clock also gates the Mali, so panthor must take it as a bus clock;
without that, the first `gpu_read()` wedges the AXI bus and the board
hangs with no output at all.

The correct, complete fix is Jonas Karlman's four-patch series (in
`patches/`, upstream commit hashes in the filenames):

1. bus clock handling in panthor
2. SError guard (no SCMI rate writes while the power domain is down)
3. dt-bindings update
4. DT: GPU node onto the SCMI clock

## Deploy rules that matter (learned the hard way)

- **Kernel first, DTB second.** Old kernel + new DTB = silent brick.
  New kernel + old DTB = safe (behaves like stock).
- After deploy: governor `simple_ondemand`, min 300 MHz / max 1 GHz.
  The `performance` governor on panthor causes visible stutter — don't.
- Full tested procedure with parachutes: `DEPLOY-PLAN.md` (Serbian;
  paths are board-specific, adapt).

## Measured (Orange Pi 5 Plus, 800 → 1000 MHz)

- glmark2 +12.5%, vkmark +6.5%
- Reference scores at 1 GHz: glmark2-es2-wayland **3121**, vkmark
  **4076–4093**, clpeak FP32 484 GFLOPS / FP16 887 GFLOPS,
  global memory bandwidth ~25 GB/s (the same LPDDR4X wall the NPU
  hits — see `../npu/data/`).
- Note: rusticl/clpeak reports "Clock frequency: 800 MHz" — that is a
  static property, not the real clock.
