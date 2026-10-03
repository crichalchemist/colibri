# RADV visit 1 — results

Run 2026-10-03 on the same iMac booted into Ubuntu 26.04.1 (kernel 7.0.0-38), AMD Radeon Pro 580X (Ellesmere 1002:67df, RADV POLARIS10), Mesa 26.0.8-1ubuntu0.3, libvulkan 1.4.341, shaderc 2026.1, gcc 15.2. Every GPU run pinned with `VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json`.

Verdict in one line: **unmodified dev passes on RADV; the fix branches change nothing on RADV; A and B are MoltenVK's.** Two RADV findings outside the handoff's expectations are recorded under step 4 and step 5.

## Deviations from HANDOFF.md as written

- The ICD file is `radeon_icd.json`, not `radeon_icd.x86_64.json`. The handoff's export points at a file that does not exist on Ubuntu 26.04; as written it would enumerate zero devices. Corrected in every command here.
- No HFS+ mount was needed. `~/Desktop/IDEAS/colibri` already held every SHA in the table, and all four branch heads matched it exactly (`vulkan-moltenvk-xbatch af326667`, `vulkan-moltenvk-subgroup 89bda174`, `vulkan-moltenvk-fixes 0baf0c63`, `upstream/dev 77647eeb`). The repro sources were at `~/Desktop/IDEAS/colibri-logs/xbatch/repro/`. Work was done in a detached worktree `~/colibri-radv` so the user's dirty `main` checkout was never touched.
- `vulkan-tools` and `vulkan-validationlayers` were not installed; the user installed them mid-visit (1.4.341.0). Steps 0 and 3 ran after that.
- Step 1 ran all **four** `mvk_shared_module` arrangements (the handoff lists three) and added `dynoff twin 0/1/2`, which ROOT_CAUSE.md names as the A control for this visit.
- Step 4's `mimo-qwenimage` needed `diffusers`, absent on this box. Installed into a venv (`~/venv-colibri`, system site-packages so torch is reused) and rerun with `PY=`: first with PyPI 0.40.0 (too old for the QwenImage 2.1 classes), then with the git SHA the generator pins.
- Two extra `glm` runs (on dev, and a comb rerun) were added to attribute a step 4 failure.

## 0. Environment (`vulkaninfo.txt`)

`vulkaninfo --summary`: `deviceName = AMD Radeon Pro 580X (RADV POLARIS10) | driverName = radv | driverInfo = Mesa 26.0.8-1ubuntu0.3 | apiVersion = 1.4.335`. **Pass.**

BAR: `mem_info_vis_vram_total = 268435456` of `mem_info_vram_total = 8589934592`; lspci BAR0 `Memory at c0000000 (64-bit, prefetchable) [size=256M]`. The engine agrees: `[VK] warning: only 256 of 7936 MB VRAM is host-visible (Resizable BAR appears disabled)`.

Subgroup size control on this device: `minSubgroupSize = maxSubgroupSize = 64`, `subgroupSizeControl = true`, `computeFullSubgroups = true`. This matters for step 2: PR-B's `sg_vary_ok()` returns true here, so its flags genuinely engage.

## 1. Standalone repros (`repro.sg.txt`, `repro.shared_module.txt`, `repro.dynoff.txt`)

All built with 0 warnings under gcc 15 `-Wall -Wextra`.

| run | result | expected | ok |
|---|---|---|---|
| sgprobe vary=0/1/2 | `reported 64, counted 64 -> CONSISTENT` ×3, rc=0 | same | ✓ |
| sgshuffle vary=0/1/2 | `shuffleXor(lane, 32) partner right 1`, butterfly `32` and `64`, CONSISTENT ×3 | same | ✓ |
| mvk_shared_module `0 0` / `0 1` / `1 0` / `1 1` | `W=256 -> PASS` ×4 | PASS ×3 (handoff) / ×4 (README) | ✓ |
| dynoff `4 160 0 0 0 <twin>` twin=0/1/2 | `-> PASS` ×3, every window ok, `0 of 160 wrong` per expert | PASS (README) | ✓ |

Device line in every run: `AMD Radeon Pro 580X (RADV POLARIS10) | driver radv Mesa 26.0.8-1ubuntu0.3 | subgroupSize 64 | ops 0x6ff | stages 0x3f` (MoltenVK reported `stages 0x32`; harmless).

Reading: RADV on this silicon is wave64-only and reports it; the `ALLOW_VARYING`/`REQUIRE_FULL` flags change nothing. The `0 0` shared-module arrangement and `dynoff twin=1` — both of which FAIL under MoltenVK argument buffers — PASS here. **A and B are MoltenVK's.**

## 2. Four trees: tier test and harness (`<name>.{build,ccbuild,tier,harness}.log`)

| name | HEAD | tier-build warnings | harness-build warnings | tier last line | harness last line | `xbatch fmt=` lines `(GEMM from 16)` |
|---|---|---|---|---|---|---|
| dev | 77647eeb | 0 | 0 | PASS | PASS | 15/15 |
| pra | af326667 | 0 | 0 | PASS | PASS | 15/15 |
| prb | 89bda174 | 0 | 0 | PASS | PASS | 15/15 |
| comb | 0baf0c63 | 0 | 0 | PASS | PASS | 15/15 |

**dev passes on RADV** — the handoff's most important question. colibri and Polaris do not own any part of A or B.

"Change nothing" verified beyond PASS/PASS: with timings stripped, the four `harness.log` files are **byte-identical** (0 diff lines each vs dev; every `xbatch` maxrel equal). The four `tier.log` files differ only in the `(N% of device time hidden)` overlap telemetry; expert counts, residency, uploads, evictions and fragmentation are identical. Since `sg_vary_ok()` is true here (step 0), PR-B's flags are applied on RADV and still change no number.

## 3. Validation layers on comb (`comb.layers.{harness,tier}.log`)

- `Insert instance layer "VK_LAYER_KHRONOS_validation"`: 1 in each log. ✓
- VUID census: `16 VUID-vkDestroyDevice-device-05137` and nothing else. `VUID-VkDeviceCreateInfo-pProperties-04451` (known on dev under MoltenVK) does not appear — it is a portability-subset VUID, not applicable on RADV. ✓
- No validation text mentions subgroup size, `ALLOW_VARYING`, `REQUIRE_FULL`, stage flags or `subgroupSizeControl`. ✓
- Both runs PASS on the Radeon. ✓

## 4. Per-engine sweep on comb (`sweep.txt`, `sweep.<mode>.log`)

Every `[VK] ready` line across all sweep logs names `AMD Radeon Pro 580X (RADV POLARIS10)`; llvmpipe never appears.

| mode | exit | note |
|---|---|---|
| shader | 0 | PASS |
| qwen | 0 | 106 s |
| inkling-olmoe | 0 | 34 s |
| mimo-qwenimage | 1 → see rerun | first run: mimo half fully passed (all `OK mimo` lines); qwenimage fixture generator died on `ModuleNotFoundError: No module named 'diffusers'` |
| mimo-qwenimage rerun (`PY=~/venv-colibri/bin/python`, PyPI diffusers 0.40.0) | 1 | mimo half passes again; qwenimage dies on `ImportError: cannot import name 'AutoencoderKLQwenImage21' from 'diffusers'`. The generator pins `git+https://github.com/huggingface/diffusers@80c7ed262aeffbeb43ef13ae04baeb9b84515a69` (`make_qwenimage_tiny.py:40`, CI "Install the pinned diffusers reference"); PyPI 0.40.0 predates the QwenImage 2.1 classes. `sweep.mimo-qwenimage.rerun.log` |
| mimo-qwenimage rerun2 (pinned diffusers 0.41.0.dev0 @ 80c7ed26) | **0** | mimo 4/4 `OK`, qwenimage 5/5 `OK` incl. `OK qwenimage picture: 6 of 262144 bytes differ from the CPU's, max 1`, `Ran 9 tests … OK`, 0 FAIL, 29 s. Caveat: this mode prints no `[VK] ready` banner (neither run did), so device identity for it rests on the device-vs-CPU comparison lines, not the banner. `sweep.mimo-qwenimage.rerun2.log` |
| kimi | 0 | 15 s |
| deepseek | 0 | 72 s |
| glm | **1** | 28 `OK colibri …` checks pass, then `FAIL: glm53 decode, cold: no matmul ran on the device` |

**Finding G (glm53 on RADV).** `[VK] glm53: device ready, dense matrices on the device (default)` and `[VK] tier glm53: on … device 16 of 32 routed experts` are followed by `[VK] glm53: 0 matmuls on the GPU`; tokens still equal the CPU's, the failing assertion is that the device ran any dense matmul. Attribution runs:
- `glm` on unmodified **dev** (77647eeb): exit 1, same line — `sweep.glm.dev.log`. **The failure predates the fix branches.**
- `glm` on comb, second run: exit 1, same line — `sweep.glm.comb-rerun.log`. **Deterministic, not flaky** (3 of 3 runs across two trees).
Not investigated further on this visit (handoff: report, don't fix). CI runs this mode under Lavapipe; whether it passes there and only fails on RADV is the next question for whoever picks it up.

## 5. Baseline: full harness on dev (`dev.harness-full.log`)

Last line `FAIL`, rc=1. Walked every threshold site in `backend_vulkan.c` (dev) against the log: the **only** case over its limit is `qprep fmt=1 S=11 I=6144 … maxrel q 0.005456 kv 0.00115` (limit `mk > 1e-3`, `backend_vulkan.c:3320`); its S=1/S=2 siblings are kv ≈ 9e-5. No `^ …` explanation lines, no `failed` lines. **RADV shows exactly the known macOS failure and nothing else.** Near-threshold but passing: `absorb+project fused` for `absorb fmt=1 S=1 H=8` at maxrel 0.002994.

Throughput record (not a gate): `BATCHED int4 S=1 6144->2048 (our gate/up): 0.6301 ms/matmul`, `2048->6144 (our down): 0.5947 ms/matmul` (N=64, one submit); `FULL VK expert_group fmt=2 32 experts | per-call 1.8641 GPU-only 1.8557 ms/expert`.

## Files

`vulkaninfo.txt`, `repro.sg.txt`, `repro.shared_module.txt`, `repro.dynoff.txt`, `{dev,pra,prb,comb}.{build,ccbuild,tier,harness}.log`, `comb.layers.{build,harness,tier}.log`, `sweep.txt`, `sweep.{shader,qwen,inkling-olmoe,mimo-qwenimage,kimi,deepseek,glm}.log`, `sweep.glm.dev.log`, `sweep.glm.comb-rerun.log`, `sweep.mimo-qwenimage.rerun.log`, `sweep.mimo-qwenimage.rerun2.log`, `dev.harness-full.{build,}.log`, `pip-diffusers.log`, `pip-diffusers-pinned.log`, `venv.log`, and the runners `run_tree.sh`, `sweep_families.sh`, `chain2.sh`, `chain3.sh`, `chain4.sh` (each `chainN.txt` is that runner's one-line-per-stage status). `repro/` is the rebuilt copy of the repro sources (Linux binaries and .spv).
