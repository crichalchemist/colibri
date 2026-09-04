# Qwen3.6 Vulkan expert tier and staged device-local uploads

Date: 2026-09-03
Status: approved design, not yet implemented
Target branch: `dev` (upstream `JustVugg/colibri`)
Author's hardware: AMD RX 470/480/570/580 (Polaris, gfx803, 8 GB VRAM, 256 MB
host-visible BAR), Mesa RADV 25.2, i7-7700K, 62 GB RAM, no CUDA.

## Problem

`qwen36` has a VRAM expert tier only for CUDA (`c/qwen36_tier.c`, built with
`make qwen36 CUDA=1`). The shared Vulkan backend (`c/backend_vulkan.c`) already
serves expert groups for the GLM and Kimi K3 engines, so AMD and Intel owners
can run those on the GPU but not Qwen3.6.

A second, deeper gap blocks validation on most pre-2020 discrete cards: every
weight tensor the Vulkan backend uploads lives in memory that is both
host-visible and device-local. Without Resizable BAR that combination exists
only inside a 256 MB window; the driver places everything beyond it in system
RAM, the tier *reports* resident experts, and every access crosses PCIe. The
backend warns about this at init and `docs/vulkan.md` measured it as slower
than the CPU path. The author's card is in exactly this state.

## Goal

1. `make qwen36 VK=1` produces an engine whose routed experts can be served
   from VRAM through the existing Vulkan expert-group path, with the same
   placement semantics the CUDA tier documents: placement changes speed, never
   routing or precision.
2. The Vulkan backend keeps real VRAM residency on discrete cards without
   Resizable BAR, by uploading weights through a host staging buffer into plain
   device-local memory.
3. Both changes are validated on Polaris/gfx803, which `docs/vulkan.md` lists
   as never tested on real hardware.

## Non-goals

- A backend-neutral tier abstraction. The maintainer rejected unifying the
  backends (issue 753); this design keeps the per-backend shape.
- Multi-device Vulkan for the tier. The backend's second-device context stays
  as it is and the tier is single-device.
- Reclaiming freed arena slices in the Vulkan weight allocator. The bump
  allocator's documented no-reclaim rule stands; the tier is designed around
  it.
- Runtime LFRU swaps on Vulkan. See "Placement policy".
- Any change to the CUDA tier's behaviour.

## Design

### 1. Backend: staged device-local uploads (`c/backend_vulkan.c`)

**Memory-type selection.** After the existing `pick_memtype` and
`pick_memtype_cached`, init selects a third type: `DEVICE_LOCAL` and not
`HOST_VISIBLE`, on the largest device-local heap. Staged mode is enabled when
that type exists **and** the host-visible device-local slice is smaller than a
quarter of total device-local memory (the same condition the current
"Resizable BAR appears disabled" warning uses). `COLI_VK_STAGED=1` forces it on
and `COLI_VK_STAGED=0` forces it off, so owners of ReBAR cards can exercise the
path. Unified-memory APUs have no such memory type and are unaffected. The init
banner states the mode.

**Allocation.** `arena_suballoc` gains a parallel chain of 256 MB blocks in the
device-local type. Those blocks are never mapped (`base == NULL`) and their
buffers are created with `STORAGE | TRANSFER_DST`. Bump semantics and the
no-reclaim rule are unchanged. A tensor records which chain it came from only
through the arena; `ColiVkTensor` does not change shape.

**Upload.** `upload_tensor` is the single entry point for resident weights
(dense matmuls, expert tiers, shared experts) and stays so. In staged mode it:

1. reserves a host staging buffer (`HOST_VISIBLE | HOST_COHERENT`, preferring a
   type that is *not* device-local so it does not consume the BAR window;
   created with `TRANSFER_SRC`; grown on demand like the existing scratches);
2. writes the padded rows and the scale block into it exactly as the mapped
   path writes them today;
3. records two `vkCmdCopyBuffer` regions on a dedicated upload command buffer
   from its own command pool, submits on the compute queue under the submit
   lock, and waits on a dedicated fence outside the lock.

Scratch buffers, the KV mirror, per-layer norm weights, and every readback
buffer keep their current memory types. The doc's measured rules about
write-combined and cached memory still govern them.

**Locking.** Two file-static `pthread_mutex_t`:

- `g_arena_mx` around arena block creation and slice bumping;
- `g_submit_mx` around every `vkQueueSubmit` in the file (main pipeline,
  expert group, attention, q-prep, and the new upload path).

Fence waits are never taken under a lock. This is what makes a background
upload thread safe next to the decode thread; `vkQueueSubmit` on one queue
from two threads is a spec violation today, it just never happened because no
engine submitted from a second thread.

**Public additions to `backend_vulkan.h`:**

```c
int  coli_vk_staged(void);                       /* 1 when staged mode is active */
const char *coli_vk_default_spv(char *buf, size_t n);
```

`coli_vk_default_spv` resolves `COLI_VK_SHADERS` (file or directory), then
`shaders/qmatmul.spv` beside the executable, then the CWD-relative default.
`colibri.c` and `kimi_k3.c` each carry a private copy of this lookup; they are
left untouched in this PR and noted for later cleanup.

**Accounting.** `coli_vk_mem_budget` already sums device-local heaps, so
staged tensors show up in the budget the tier reads. `used_bytes` and
`tensor_count` accounting is unchanged.

### 2. Tier: backend shim in `c/qwen36_tier.c`

The file keeps its placement logic (slot table, upload queue and thread,
warmstart planning, heat file, hit/miss statistics) unchanged. A compile-time
backend block near the top replaces the direct CUDA calls:

```c
#if defined(COLI_CUDA)
  /* CUDA shim (existing behaviour) */
#elif defined(COLI_VULKAN)
  /* Vulkan shim */
#endif
```

If both are defined, CUDA wins; the doc says so. The shim covers:

| operation | CUDA | Vulkan |
|---|---|---|
| env gate | `COLI_CUDA=1` | `COLI_VULKAN=1` |
| init | `coli_cuda_init(devs, n)` | `coli_vk_init(coli_vk_default_spv(...))` |
| device count | `coli_cuda_device_count()` | 1 |
| free memory | `coli_cuda_mem_info` | `coli_vk_mem_budget`; fallback 4 GB with a warning when the budget extension is absent |
| upload | `coli_cuda_tensor_upload[_g]` | `coli_vk_tensor_ensure` (fmt 2 per-row, fmt 4 grouped) |
| free | `coli_cuda_tensor_free` | `coli_vk_tensor_free` |
| issue | `coli_cuda_expert_group_issue` | `coli_vk_expert_group_issue` |
| take | `coli_cuda_expert_group_take(dev)` returns device-owned `y` | `coli_vk_expert_group_take(y)` into a tier-owned `[32*D]` buffer |
| stats | `coli_cuda_stats`, `coli_cuda_group_stats` | `coli_vk_mem_info` |
| shutdown | `coli_cuda_shutdown` | `coli_vk_shutdown` |

Tensor handles in `QSlot` become `void *`. The nibble conversion in `stage()`
(two's-complement to offset-binary, `XOR 0x88`) stays: the Vulkan `i4()`
decoder is `nibble - 8`, identical to CUDA fmt 2. Group issue passes
`rows[c] = 1` for every expert and replicates `x` per expert, as today; the
backend's limits (`count <= 64`, `D <= 6144`) hold for Qwen3.6-35B-A3B
(`D = 2048`, `topk <= 32`).

**Placement policy on Vulkan: fill once.** `qt_lfru_tick_locked` compiles out
under Vulkan. Residency is decided at warmstart, ordered by `HEAT_FILE` when
present and natural order otherwise, up to the budget. Heat still accumulates
and still saves at shutdown, so a second run starts with the hot set. Reason:
the Vulkan arena never reclaims a freed slice, so each runtime swap would leak
one expert of VRAM until the heap was exhausted. The two existing Vulkan tiers
(GLM, Kimi K3) already fill once for the same reason.

**Budget.** `VK_EXPERT_GB` (GB, or `auto`). `auto` is device budget minus 1 GB
headroom, matching `CUDA_EXPERT_GB`'s auto rule. Per-expert bytes are computed
as today.

New tier query for the engine banner:

```c
const char *qt_backend_name(void);   /* "CUDA" | "Vulkan" */
```

### 3. Engine: `c/qwen36.c`

Two changes only:

- the startup banner prints `[gpu] MoE experts -> <backend> VRAM tier` through
  `qt_backend_name()`;
- when `COLI_VULKAN` is set on a build without the tier, the engine prints one
  notice (`[qwen36] COLI_VULKAN set but this binary was built without VK=1;
  running on CPU`) and continues. This closes the silent-ignore defect the
  maintainer described in issue 894 for this engine.

`qwen36_tier.h` changes its guard from `#ifdef COLI_CUDA` to
`#if defined(COLI_CUDA) || defined(COLI_VULKAN)`; the inline stubs apply only
when neither is defined.

### 4. Build: `c/Makefile`

```make
ifeq ($(CUDA),1)
QWEN36_TIER_SRC = qwen36_tier.c
QWEN36_CFLAGS   = $(CFLAGS)
QWEN36_LDFLAGS  = $(LDFLAGS)
else ifeq ($(VK),1)
QWEN36_TIER_SRC = qwen36_tier.c
QWEN36_CFLAGS   = $(NOCUDA_CFLAGS)      # keeps -DCOLI_VULKAN
QWEN36_LDFLAGS  = $(NOCUDA_LDFLAGS)     # keeps -lvulkan
else
...
endif
qwen36$(EXE): ... $(QWEN36_TIER_SRC) $(CUDA_OBJ) $(VK_OBJ) $(VK_SPV)
	$(CC) $(QWEN36_CFLAGS) qwen36.c $(QWEN36_TIER_SRC) $(CUDA_OBJ) $(VK_OBJ) -o $@ $(QWEN36_LDFLAGS)
```

The four test targets that compile `qwen36.c` (`test_qwen36_ctx`,
`test_qwen36_dense_batch`, `test_qwen36_json_escape`,
`test_qwen36_cache_index`) and `bench_qwen36_dense_batch` gain `$(VK_OBJ)` the
same way. `tests/test_makefile_cuda_scope.py` is re-run to confirm the CUDA
scoping rules still hold.

### 5. Environment variables

| variable | default | meaning |
|---|---|---|
| `COLI_VK_STAGED` | auto | Force staged device-local uploads on (`1`) or off (`0`) for any Vulkan engine. Auto: on when host-visible VRAM is under a quarter of total. |
| `VK_EXPERT_GB` | `auto` | Qwen3.6 Vulkan tier VRAM budget in GB. `auto` = device budget minus 1 GB. |

Reused unchanged: `COLI_VULKAN`, `COLI_VK_SHADERS`, `HEAT_FILE`,
`QT_NO_WARMSTART`, `COLI_KEEP_INT8`.

### 6. Docs

- `docs/vulkan.md`: new "Staged uploads (no Resizable BAR)" section; the
  "Discrete cards need Resizable BAR" paragraph becomes "Resizable BAR is
  faster; staged mode makes it optional", with the measured numbers kept.
  Polaris moves from "not yet validated" to the measured table once numbers
  exist.
- `docs/qwen36-cuda-tier.md` is renamed `docs/qwen36-tier.md` with a Vulkan
  subsection: build line, fill-once behaviour, `VK_EXPERT_GB`, CUDA precedence
  when both are built. `docs/qwen36.md` and any links are updated.
- `docs/ENVIRONMENT.md`: two new rows, and a Vulkan line under the Qwen3.6
  engine section.
- `CHANGELOG.md`: one entry.

Benchmark numbers in the PR follow `docs/vulkan.md`'s rules: frozen
`HEAT_FILE` across runs, `DRAFT` pinned on both arms, GPU clocks pinned or
disclosed, hardware and commit listed.

## Testing

**Layer 1: backend exactness.** The standalone harness
(`gcc -O3 -DVK_TEST backend_vulkan.c -o test_vk -lvulkan -lm && ./test_vk
shaders/qmatmul.spv`) already compares every primitive against a CPU
reference. It runs twice, once with `COLI_VK_STAGED=0` and once with
`COLI_VK_STAGED=1`, and the harness prints the active mode. This proves the
staged path bit-equivalent to the mapped path on the same card before any tier
code exists.

**Layer 2: tier correctness.** New `c/tests/test_qwen36_tier_vk.c`, built only
under `VK=1`. Using the eight-expert tiny fixture geometry, it: initialises the
tier; warmstarts a subset within a small `VK_EXPERT_GB`; issues a group for a
fixed routing; takes the result; and compares the accumulated output with the
CPU int8 expert path within float tolerance. It asserts the fill-once rule by
running enough ticks to cross the old swap threshold and confirming the
resident set is unchanged. Without a usable Vulkan device it prints a skip
message and exits 0, matching how the repo treats GPU tests on CPU hosts. Test
names describe outcomes (`resident_experts_match_cpu_path`,
`residency_frozen_after_warmstart`).

**Layer 3: engine level.** `make qwen36-tiny-check` passes in a plain build and
in a `VK=1` build. Then, on the author's RX 580 with the 20 GB container:
greedy decode compared token-for-token against the CPU run under a frozen heat
file, plus throughput, TTFT, and the tier's hit rate recorded for the PR.

**Definition of done.**

- `make qwen36`, `make qwen36 VK=1`, and (on a CUDA host or in CI) `make qwen36
  CUDA=1` build with zero warnings.
- Layers 1 to 3 pass; `make check` is green.
- PR against `dev` with the template's Summary, Validation, and Compatibility
  sections filled, including the Polaris measurements.

## Risks and open questions

- **Staged upload throughput on Polaris.** PCIe 3.0 x16 copies at roughly
  12 GB/s; the 20 GB container's expert set fills an 8 GB budget in seconds.
  Acceptable for a warmstart-only tier.
- **Queue-submit contention.** The upload thread's submits interleave with
  decode submits under one lock. Uploads happen only at warmstart on Vulkan,
  so decode never waits on the lock in steady state.
- **`VK_EXT_memory_budget` absence.** Older Mesa builds may not expose it; the
  4 GB fallback with a warning keeps the tier usable and the user can set
  `VK_EXPERT_GB` explicitly.
- **Mixed-tier greedy stability.** Issue 510 established that experts served
  from different kernel families can flip near-tie argmax steps. The
  validation protocol freezes placement so run-to-run comparison is
  meaningful; a single-token divergence between CPU and Vulkan arms is
  reported, not hidden.
