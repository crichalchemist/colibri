# Qwen3.6 Vulkan Expert Tier Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `make qwen36 VK=1` serves hot routed experts from VRAM through the shared Vulkan backend, and that backend keeps real VRAM residency on discrete cards without Resizable BAR.

**Architecture:** `c/backend_vulkan.c` gains a device-local weight arena filled through a host staging buffer and `vkCmdCopyBuffer`, chosen automatically when host-visible VRAM is small, plus two mutexes so a background upload thread is safe next to the decode thread. `c/qwen36_tier.c` keeps all placement logic and gains a compile-time backend shim (CUDA or Vulkan); on Vulkan the tier is single-device and fills once at warmstart. The engine, Makefile, docs and CI are wired accordingly.

**Tech Stack:** C11, Vulkan 1.2 (libvulkan, `glslc` from shaderc), GNU make, pthreads. No new dependencies.

**Spec:** `docs/superpowers/specs/2026-09-03-qwen36-vulkan-tier-design.md`

## Global Constraints

- Every build must produce **0 warnings** (the repo's review bar). Flags are the Makefile's `-Wall -Wextra -Wno-unused-parameter -Wno-misleading-indentation -Wno-unused-function`.
- The default CPU build stays dependency-free: nothing under `#ifdef COLI_VULKAN` may leak into a plain build.
- The CUDA tier's behaviour must not change. There is no CUDA toolkit on the dev box; the CUDA side of `qwen36_tier.c` is checked with `gcc -fsyntax-only -DCOLI_CUDA`.
- Work happens on branch `qwen36-vulkan-tier` (tracks `upstream/dev`). Push to `origin` (the fork). PR goes to `JustVugg/colibri` branch `dev`.
- Commit messages end with the session trailer:
  ```
  Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N
  ```
- Never `git add -A` or `git add .`: `.remember/` and `.serena/` are untracked local dirs that must stay out of git.
- Match the file's existing style exactly (K&R braces, 4-space indent, `/* */` comments in the tier, `//` in the backend where the surrounding code uses them).
- The dev box: RX 580 (gfx803, 8 GB, 256 MB host-visible BAR), Mesa RADV, no CUDA. `glslc` must be installed before Task 3's harness runs (Task 0).

---

## File map

| File | Change | Responsibility |
|---|---|---|
| `c/backend_vulkan.c` | modify | locks, staged device-local arena + upload, `coli_vk_staged`, `coli_vk_default_spv`, harness banner |
| `c/backend_vulkan.h` | modify | declare the two new public functions |
| `c/qwen36_tier.h` | modify | guard becomes CUDA-or-Vulkan; `qt_backend_name` |
| `c/qwen36_tier.c` | modify | backend shim; single-device + fill-once under Vulkan |
| `c/qwen36.c` | modify | backend-aware banner; notice when `COLI_VULKAN` is set on a build without the tier |
| `c/Makefile` | modify | `VK=1` branch for `qwen36`, five test targets, new test rule |
| `c/tests/test_qwen36_tier_vk.c` | create | tier correctness against a CPU reference; fill-once assertion |
| `docs/vulkan.md` | modify | staged uploads section; ReBAR paragraph rewritten |
| `docs/qwen36-tier.md` | rename from `docs/qwen36-cuda-tier.md` + modify | Vulkan subsection |
| `docs/qwen36.md` | modify | link + one sentence |
| `docs/ENVIRONMENT.md` | modify | `COLI_VK_STAGED`, `VK_EXPERT_GB`, Qwen3.6 Vulkan line |
| `CHANGELOG.md` | modify | one Unreleased entry |
| `.github/workflows/ci.yml` | modify | Vulkan job also builds `qwen36 VK=1` and runs the tier test on Lavapipe |

---

### Task 0: Toolchain check on the dev box

**Files:** none.

- [ ] **Step 1: Check for glslc**

Run: `which glslc || echo MISSING`
Expected on this box: `MISSING`.

- [ ] **Step 2: Ask the user before installing**

Installing a system package is outside the repo. Ask: "May I run `sudo apt-get install -y glslc libvulkan-dev`?" and stop until answered. (`libvulkan-dev` provides `/usr/include/vulkan/vulkan.h`, also missing here; `mesa-vulkan-drivers` is already present.)

- [ ] **Step 3: Verify the baseline builds and the harness passes before any change**

Run:
```bash
cd c && make colibri VK=1 2>&1 | tail -3
gcc -O3 -DVK_TEST backend_vulkan.c -o /tmp/test_vk -lvulkan -lm && /tmp/test_vk shaders/qmatmul.spv 2>&1 | tail -15
```
Expected: build with 0 warnings; the harness prints per-case `maxrel` lines and ends without `FAIL`. Record the `[VK] ready:` line and the ReBAR warning text; they are the "before" evidence for the PR. If the harness fails on the untouched tree, stop and report; that is a pre-existing Polaris problem and changes the plan.

---

### Task 1: Serialise queue submits and arena allocation

**Files:**
- Modify: `c/backend_vulkan.c` (includes at top; `arena_suballoc`; the seven production `vkQueueSubmit` sites)

**Interfaces:**
- Produces: `static VkResult vk_submit(VkQueue q, const VkSubmitInfo *si, VkFence f)` — the only way production code submits on `G.queue` from here on. `static int arena_suballoc(size_t bytes, VkBuffer *buf, void **ptr)` keeps its signature but is now lock-protected.

- [ ] **Step 1: Add the include and the two locks**

After `#include <time.h>` at the top of `c/backend_vulkan.c` add:

```c
#include <pthread.h>

/* Thread safety (qwen36 tier): an upload thread may call coli_vk_tensor_ensure while the
 * decode thread submits expert groups. vkQueueSubmit on one VkQueue from two threads is a
 * spec violation, and the weight arena is a linked list. Every production submit on
 * G.queue goes through vk_submit; arena_suballoc takes g_arena_mx. Fence waits are never
 * taken under either lock. The VK_TEST harness is single-threaded and submits directly. */
static pthread_mutex_t g_submit_mx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_arena_mx  = PTHREAD_MUTEX_INITIALIZER;
static VkResult vk_submit(VkQueue q, const VkSubmitInfo *si, VkFence f) {
    pthread_mutex_lock(&g_submit_mx);
    VkResult r = vkQueueSubmit(q, 1, si, f);
    pthread_mutex_unlock(&g_submit_mx);
    return r;
}
```

- [ ] **Step 2: Route the seven production submits through it**

Run:
```bash
cd c && grep -c 'VKCHECK(vkQueueSubmit(G.queue, 1, &si, ' backend_vulkan.c
```
Expected: `7`. Then:
```bash
sed -i 's/VKCHECK(vkQueueSubmit(G.queue, 1, &si, /VKCHECK(vk_submit(G.queue, \&si, /' backend_vulkan.c
grep -n 'vkQueueSubmit(G.queue' backend_vulkan.c
```
Expected: the remaining matches are all inside the `#ifdef VK_TEST` block (line numbers above the `#ifdef VK_TEST` line, which is ~1584 before this task; confirm with `grep -n '#ifdef VK_TEST' backend_vulkan.c`). The `G2.queue` submit is untouched (it is a different VkDevice and VkQueue).

- [ ] **Step 3: Lock the arena**

Rename the existing function `static int arena_suballoc(size_t bytes, VkBuffer *buf, void **ptr)` to `arena_suballoc_locked` and add a wrapper right after its closing brace:

```c
static int arena_suballoc(size_t bytes, VkBuffer *buf, void **ptr) {
    pthread_mutex_lock(&g_arena_mx);
    int r = arena_suballoc_locked(bytes, buf, ptr);
    pthread_mutex_unlock(&g_arena_mx);
    return r;
}
```
The `VKCHECK` early returns inside the locked body are why the lock lives in the wrapper.

- [ ] **Step 4: Build, harness, no behaviour change**

Run:
```bash
cd c && make colibri VK=1 2>&1 | grep -E 'warning|error' ; echo "exit=$?"
gcc -O3 -DVK_TEST backend_vulkan.c -o /tmp/test_vk -lvulkan -lm && /tmp/test_vk shaders/qmatmul.spv 2>&1 | grep -cE 'FAIL'
```
Expected: no warning/error lines (grep exit 1), `0` FAIL lines. `-lpthread` is not needed: glibc 2.34+ has pthreads in libc, and the Makefile already passes `-pthread`.

- [ ] **Step 5: Commit**

```bash
git add c/backend_vulkan.c
git commit -m "vk: serialise queue submits and arena allocation

Prepares the backend for a second thread uploading weights while the decode
thread submits expert groups (qwen36 tier). No behaviour change on one thread.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

---

### Task 2: Staged-mode decision, `coli_vk_staged`, upload command buffer

**Files:**
- Modify: `c/backend_vulkan.c` (struct `G`; after `pick_memtype_cached`; the ReBAR block in `coli_vk_init`; after the fence creation in `coli_vk_init`; `coli_vk_shutdown`; harness `main`)
- Modify: `c/backend_vulkan.h`

**Interfaces:**
- Produces: `int coli_vk_staged(void)`; struct fields `G.staged`, `G.memtype_dl`, `G.memtype_stage`, `G.up_cpool`, `G.up_cmd`, `G.up_fence`, `G.stage`.
- Env: `COLI_VK_STAGED` = `1` force on, `0` force off, unset = auto.

- [ ] **Step 1: Write the failing check**

Append to the harness `main` in `c/backend_vulkan.c`, right after `if (!coli_vk_init(spv)) { printf("vk init failed\n"); return 1; }`:

```c
    printf("weights: %s\n", coli_vk_staged() ? "staged device-local" : "mapped host-visible");
```

Run: `cd c && gcc -O3 -DVK_TEST backend_vulkan.c -o /tmp/test_vk -lvulkan -lm 2>&1 | head -3`
Expected: `error: implicit declaration of function 'coli_vk_staged'`.

- [ ] **Step 2: Declare it**

In `c/backend_vulkan.h`, after `int  coli_vk_mem_budget(double *used_gb, double *budget_gb);` add:

```c
/* 1 when resident weights go to plain DEVICE_LOCAL memory through a host staging buffer
 * (discrete card without Resizable BAR, or COLI_VK_STAGED=1); 0 = mapped host-visible
 * uploads as before. Decided once in coli_vk_init. */
int  coli_vk_staged(void);
```

- [ ] **Step 3: Add the state**

In `struct G` (the `static struct { ... } G;` block), after the line `uint32_t memtype_cached;     // HOST_CACHED — for buffers the CPU reads back (outputs)` add:

```c
    /* Staged weight path: on a discrete card without Resizable BAR the HOST_VISIBLE|
     * DEVICE_LOCAL type is a ~256 MB window and everything past it silently lands in
     * system RAM. When staged==1, weight arenas use memtype_dl (DEVICE_LOCAL, not
     * host-visible) and are filled by vkCmdCopyBuffer from `stage` (memtype_stage,
     * host-visible, preferably NOT device-local so it never eats the BAR) on the
     * dedicated up_cmd/up_fence. Scratches, KV mirror and readbacks are unchanged. */
    int staged; uint32_t memtype_dl, memtype_stage;
    VkCommandPool up_cpool; VkCommandBuffer up_cmd; VkFence up_fence;
```

and after `Scratch x, y, h;   /* h = fused gate+up hidden output */` add:

```c
    Scratch stage;     /* host staging buffer for the staged weight path (TRANSFER_SRC) */
```

- [ ] **Step 4: Add the two memory-type pickers**

After the `pick_memtype_cached` function add:

```c
/* DEVICE_LOCAL and NOT host-visible, on the largest device-local heap: the target of the
 * staged weight path. -1 when no such type exists (unified-memory APUs, Lavapipe). */
static int pick_memtype_dl(VkPhysicalDevice phys) {
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(phys, &m);
    int best = -1; VkDeviceSize bestheap = 0;
    for (uint32_t i = 0; i < m.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = m.memoryTypes[i].propertyFlags;
        if (!(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
        VkDeviceSize hs = m.memoryHeaps[m.memoryTypes[i].heapIndex].size;
        if (hs > bestheap) { bestheap = hs; best = (int)i; }
    }
    return best;
}
/* Host-visible+coherent type for the staging buffer, preferring one that is NOT
 * device-local so staging never consumes the BAR window; falls back to G.memtype. */
static int pick_memtype_stage(VkPhysicalDevice phys) {
    VkPhysicalDeviceMemoryProperties m;
    vkGetPhysicalDeviceMemoryProperties(phys, &m);
    for (uint32_t i = 0; i < m.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = m.memoryTypes[i].propertyFlags;
        if ((f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) && (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) &&
            !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return (int)i;
    }
    return (int)G.memtype;
}
```

- [ ] **Step 5: Decide the mode inside the ReBAR block**

In `coli_vk_init`, replace the ReBAR block's two-branch warning. The current code is:

```c
        VkMemoryPropertyFlags cf = mp.memoryTypes[G.memtype].propertyFlags;
        VkDeviceSize hv_dl = mp.memoryHeaps[mp.memoryTypes[G.memtype].heapIndex].size;
        if (dl_max && !(cf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            fprintf(stderr, "[VK] warning: no host-visible+device-local memory type — weight tiers "
                    "will live in system RAM and every access crosses PCIe (expect slower than "
                    "CPU-only). On a discrete card, enable Resizable BAR in the BIOS.\n");
        else if (dl_max && hv_dl * 4 < dl_max)
            fprintf(stderr, "[VK] warning: only %llu of %llu MB VRAM is host-visible (Resizable BAR "
                    "appears disabled) — allocations beyond the %llu MB window fall back to system "
                    "RAM and will be slow. Enable Resizable BAR / Smart Access Memory in the BIOS.\n",
                    (unsigned long long)(hv_dl >> 20), (unsigned long long)(dl_max >> 20),
                    (unsigned long long)(hv_dl >> 20));
```

Replace it with:

```c
        VkMemoryPropertyFlags cf = mp.memoryTypes[G.memtype].propertyFlags;
        VkDeviceSize hv_dl = mp.memoryHeaps[mp.memoryTypes[G.memtype].heapIndex].size;
        int small_bar = dl_max && (!(cf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) || hv_dl * 4 < dl_max);
        /* Staged device-local weights: auto when the host-visible slice is small,
         * COLI_VK_STAGED=1/0 forces either way (1 lets ReBAR owners validate the path). */
        int dl = pick_memtype_dl(G.phys);
        const char *st = getenv("COLI_VK_STAGED");
        int want = st && *st ? (*st == '1') : small_bar;
        if (want && dl < 0) {
            fprintf(stderr, "[VK] COLI_VK_STAGED requested but no device-local-only memory type "
                    "(unified memory?) — mapped uploads\n");
            want = 0;
        }
        if (want) {
            G.staged = 1; G.memtype_dl = (uint32_t)dl; G.memtype_stage = (uint32_t)pick_memtype_stage(G.phys);
            fprintf(stderr, "[VK] weights: staged device-local uploads (host-visible VRAM %llu of %llu MB; "
                    "memtype %u via staging memtype %u)\n", (unsigned long long)(hv_dl >> 20),
                    (unsigned long long)(dl_max >> 20), G.memtype_dl, G.memtype_stage);
        } else if (dl_max && !(cf & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            fprintf(stderr, "[VK] warning: no host-visible+device-local memory type — weight tiers "
                    "will live in system RAM and every access crosses PCIe (expect slower than "
                    "CPU-only). On a discrete card, enable Resizable BAR in the BIOS or set "
                    "COLI_VK_STAGED=1.\n");
        else if (small_bar)
            fprintf(stderr, "[VK] warning: only %llu of %llu MB VRAM is host-visible (Resizable BAR "
                    "appears disabled) — allocations beyond the %llu MB window fall back to system "
                    "RAM and will be slow. Enable Resizable BAR / Smart Access Memory in the BIOS, "
                    "or unset COLI_VK_STAGED=0.\n",
                    (unsigned long long)(hv_dl >> 20), (unsigned long long)(dl_max >> 20),
                    (unsigned long long)(hv_dl >> 20));
```

- [ ] **Step 6: Create the upload command buffer and fence**

In `coli_vk_init`, right after `VKCHECK(vkCreateFence(G.dev, &fi, NULL, &G.eg_fence), "eg fence");` add:

```c
    if (G.staged) {   /* own pool: command buffers from one pool need external sync too */
        VkCommandPoolCreateInfo upci = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = G.qfam};
        VKCHECK(vkCreateCommandPool(G.dev, &upci, NULL, &G.up_cpool), "up cmdPool");
        VkCommandBufferAllocateInfo ubi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .commandPool = G.up_cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
        VKCHECK(vkAllocateCommandBuffers(G.dev, &ubi, &G.up_cmd), "up cmdBuf");
        VKCHECK(vkCreateFence(G.dev, &fi, NULL, &G.up_fence), "up fence");
    }
```

- [ ] **Step 7: The accessor, and shutdown cleanup**

After `int coli_vk_available(void) { return G.ready; }` add:

```c
int coli_vk_staged(void) { return G.ready && G.staged; }
```

In `coli_vk_shutdown`, after the line that destroys `G.y2`, add:

```c
    if (G.stage.buf) { vkDestroyBuffer(G.dev, G.stage.buf, NULL); vkFreeMemory(G.dev, G.stage.mem, NULL); }
    if (G.up_fence) vkDestroyFence(G.dev, G.up_fence, NULL);
    if (G.up_cpool) vkDestroyCommandPool(G.dev, G.up_cpool, NULL);
```

- [ ] **Step 8: Build and run the harness in both modes**

Run:
```bash
cd c && gcc -O3 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -DVK_TEST backend_vulkan.c -o /tmp/test_vk -lvulkan -lm 2>&1 | grep -E 'warning|error'
COLI_VK_STAGED=0 /tmp/test_vk shaders/qmatmul.spv 2>&1 | grep -E '^weights:|\[VK\] (weights|warning)|FAIL'
COLI_VK_STAGED=1 /tmp/test_vk shaders/qmatmul.spv 2>&1 | grep -E '^weights:|\[VK\] (weights|warning)|FAIL'
```
Expected: no warnings. First run prints `weights: mapped host-visible` and the small-BAR warning. Second prints the `[VK] weights: staged device-local uploads` banner and `weights: staged device-local`. No `FAIL` in either (uploads still take the mapped path at this point, so results are unchanged). Also `make colibri VK=1` with 0 warnings.

- [ ] **Step 9: Commit**

```bash
git add c/backend_vulkan.c c/backend_vulkan.h
git commit -m "vk: decide staged device-local mode at init (COLI_VK_STAGED)

Picks a DEVICE_LOCAL-only memory type and a non-BAR staging type, creates the
upload command buffer/fence, and reports the mode. Uploads still use the mapped
path; the next commit switches them.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

---

### Task 3: Device-local arena and staged upload path

**Files:**
- Modify: `c/backend_vulkan.c` (`alloc_hostvis_mt`, `VkWArena`/`arena_suballoc_locked`/`arena_suballoc`, `upload_tensor`)

**Interfaces:**
- Consumes: `vk_submit`, `vk_fence_wait(VkDevice, VkFence)`, `G.staged`, `G.memtype_dl`, `G.memtype_stage`, `G.up_cmd`, `G.up_fence`, `G.stage` from Tasks 1–2.
- Produces: `static int arena_suballoc(size_t bytes, VkBuffer *buf, void **ptr, int dl)` (fourth arg: 1 = device-local chain, `*ptr` set to NULL); `static int alloc_buf_mt(size_t, VkBuffer*, VkDeviceMemory*, void**, uint32_t memtype, VkBufferUsageFlags)`; `static int stage_copy(ColiVkTensor*, const void*, const float*, size_t cpu_rb, size_t stride, size_t sb)`.

- [ ] **Step 1: A buffer allocator with a usage flag**

Rename `alloc_hostvis_mt` to `alloc_buf_mt` and give it a trailing `VkBufferUsageFlags usage` parameter used in `.usage = usage`. Directly below it add back the old name as a wrapper so every existing caller compiles unchanged:

```c
static int alloc_hostvis_mt(size_t bytes, VkBuffer *buf, VkDeviceMemory *mem, void **ptr, uint32_t memtype) {
    return alloc_buf_mt(bytes, buf, mem, ptr, memtype, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
}
```

- [ ] **Step 2: Second arena chain**

Change the arena declarations to:

```c
typedef struct VkWArena { VkDeviceMemory mem; uint8_t *base; size_t cap, off; struct VkWArena *next; } VkWArena;
static VkWArena *g_warena;      /* mapped host-visible chain (G.memtype) */
static VkWArena *g_warena_dl;   /* staged device-local chain (G.memtype_dl); base == NULL */
```

Give `arena_suballoc_locked` a fourth parameter `int dl` and make these edits inside it:

1. The buffer create info's usage becomes
   `.usage = dl ? (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT) : VK_BUFFER_USAGE_STORAGE_BUFFER_BIT`.
2. Add `uint32_t mt = dl ? G.memtype_dl : G.memtype; VkWArena **head = dl ? &g_warena_dl : &g_warena;` before the memory-type-bits check, and use `mt` in that check (`req.memoryTypeBits & (1u << mt)`).
3. `VkWArena *a = g_warena;` becomes `VkWArena *a = *head;`.
4. In the new-block path: `.memoryTypeIndex = mt`; replace the combined allocate-and-map condition with:
   ```c
        if (vkAllocateMemory(G.dev, &ai, NULL, &a->mem) != VK_SUCCESS ||
            (!dl && vkMapMemory(G.dev, a->mem, 0, cap, 0, (void **)&a->base) != VK_SUCCESS)) {
   ```
   and `a->next = g_warena; g_warena = a;` becomes `a->next = *head; *head = a;`.
5. `if (ptr) *ptr = a->base + off;` becomes `if (ptr) *ptr = dl ? NULL : a->base + off;`.

Update the wrapper:

```c
static int arena_suballoc(size_t bytes, VkBuffer *buf, void **ptr, int dl) {
    pthread_mutex_lock(&g_arena_mx);
    int r = arena_suballoc_locked(bytes, buf, ptr, dl);
    pthread_mutex_unlock(&g_arena_mx);
    return r;
}
```

Update the comment above the arena to mention the second chain: append to the existing block comment the sentence `The staged path keeps a second chain in DEVICE_LOCAL-only memory (never mapped); slices there are filled by stage_copy.`

Fix the two existing callers in `upload_tensor` to pass `0` as the new fourth argument for now (they are rewritten in Step 4).

- [ ] **Step 3: Staging buffer and copy**

Add after `arena_suballoc`:

```c
/* Grow-on-demand host staging buffer for the staged weight path (4 MB granularity so
 * the per-expert tier uploads never reallocate). Callers hold g_upload_mx. */
static int stage_reserve(size_t bytes) {
    if (G.stage.cap >= bytes) return 1;
    if (G.stage.buf) { vkDestroyBuffer(G.dev, G.stage.buf, NULL); vkFreeMemory(G.dev, G.stage.mem, NULL); }
    G.stage.buf = VK_NULL_HANDLE; G.stage.cap = 0; G.stage.ptr = NULL;
    size_t cap = (bytes + ((size_t)4 << 20) - 1) & ~(((size_t)4 << 20) - 1);
    if (!alloc_buf_mt(cap, &G.stage.buf, &G.stage.mem, &G.stage.ptr, G.memtype_stage,
                      VK_BUFFER_USAGE_TRANSFER_SRC_BIT)) return 0;
    G.stage.cap = cap;
    return 1;
}

/* Staged upload: pack the padded rows and the scale block into the staging buffer, copy
 * both into the tensor's device-local slices on the upload command buffer, wait. One
 * upload at a time (g_upload_mx); the submit is serialised with decode by vk_submit. */
static pthread_mutex_t g_upload_mx = PTHREAD_MUTEX_INITIALIZER;
static int stage_copy(ColiVkTensor *t, const void *weights, const float *scales,
                      size_t cpu_rb, size_t stride, size_t sb) {
    if (!stage_reserve(t->wbytes + sb)) return 0;
    uint8_t *p = G.stage.ptr;
    memset(p, 0, t->wbytes);
    for (int o = 0; o < t->O; o++)
        memcpy(p + (size_t)o * stride, (const uint8_t *)weights + (size_t)o * cpu_rb, cpu_rb);
    memcpy(p + t->wbytes, scales, sb);
    VKCHECK(vkResetCommandBuffer(G.up_cmd, 0), "up resetCmd");
    VkCommandBufferBeginInfo begin = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    VKCHECK(vkBeginCommandBuffer(G.up_cmd, &begin), "up beginCmd");
    VkBufferCopy cw = {.srcOffset = 0, .dstOffset = 0, .size = t->wbytes};
    VkBufferCopy cs = {.srcOffset = t->wbytes, .dstOffset = 0, .size = sb};
    vkCmdCopyBuffer(G.up_cmd, G.stage.buf, t->wbuf, 1, &cw);
    vkCmdCopyBuffer(G.up_cmd, G.stage.buf, t->sbuf, 1, &cs);
    VkMemoryBarrier mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, .dstAccessMask = VK_ACCESS_SHADER_READ_BIT};
    vkCmdPipelineBarrier(G.up_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         0, 1, &mb, 0, NULL, 0, NULL);
    VKCHECK(vkEndCommandBuffer(G.up_cmd), "up endCmd");
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &G.up_cmd};
    VKCHECK(vkResetFences(G.dev, 1, &G.up_fence), "up resetFence");
    VKCHECK(vk_submit(G.queue, &si, G.up_fence), "up queueSubmit");
    if (vk_fence_wait(G.dev, G.up_fence) != VK_SUCCESS) {
        fprintf(stderr, "[VK] staged upload fence wait failed — disabling GPU offload\n");
        G.ready = 0; return 0;
    }
    return 1;
}
```

`vk_fence_wait` is defined later in the file than `upload_tensor`; add a forward declaration `static VkResult vk_fence_wait(VkDevice dev, VkFence f);` above `stage_copy`.

- [ ] **Step 4: Branch `upload_tensor`**

In `upload_tensor`, replace everything from `void *wptr;` through `memcpy(sptr, scales, sfl * sizeof(float));` with:

```c
    size_t sb = sfl * sizeof(float);
    if (G.staged) {
        pthread_mutex_lock(&g_upload_mx);
        int ok = arena_suballoc(t->wbytes, &t->wbuf, NULL, 1) &&
                 arena_suballoc(sb, &t->sbuf, NULL, 1) &&
                 stage_copy(t, weights, scales, cpu_rb, stride, sb);
        pthread_mutex_unlock(&g_upload_mx);
        if (!ok) {
            if (t->wbuf) vkDestroyBuffer(G.dev, t->wbuf, NULL);
            if (t->sbuf) vkDestroyBuffer(G.dev, t->sbuf, NULL);
            free(t); return 0;
        }
    } else {
        void *wptr;
        if (!arena_suballoc(t->wbytes, &t->wbuf, &wptr, 0)) { free(t); return 0; }
        memset(wptr, 0, t->wbytes);
        for (int o = 0; o < O; o++)                        // copy row-by-row into padded layout
            memcpy((uint8_t *)wptr + (size_t)o * stride,
                   (const uint8_t *)weights + (size_t)o * cpu_rb, cpu_rb);
        void *sptr;
        if (!arena_suballoc(sb, &t->sbuf, &sptr, 0)) {
            vkDestroyBuffer(G.dev, t->wbuf, NULL); free(t); return 0;
        }
        memcpy(sptr, scales, sb);
    }
```

The `__atomic_add_fetch` accounting lines that follow stay as they are.

- [ ] **Step 5: Harness in both modes**

Run:
```bash
cd c && gcc -O3 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -DVK_TEST backend_vulkan.c -o /tmp/test_vk -lvulkan -lm 2>&1 | grep -E 'warning|error'
COLI_VK_STAGED=0 /tmp/test_vk shaders/qmatmul.spv > /tmp/vk_mapped.log 2>&1; grep -cE 'FAIL' /tmp/vk_mapped.log
COLI_VK_STAGED=1 /tmp/test_vk shaders/qmatmul.spv > /tmp/vk_staged.log 2>&1; grep -cE 'FAIL' /tmp/vk_staged.log
grep -E 'maxrel' /tmp/vk_mapped.log | head -5; grep -E 'maxrel' /tmp/vk_staged.log | head -5
```
Expected: no warnings; `0` and `0`; the `maxrel` values in the two logs are identical per case (same shader, same bytes, different memory). Keep both logs: they are PR evidence. Also compare the harness's `ms/matmul` benchmark lines: on this card the staged run should be markedly faster for the resident-tensor benches (the mapped run reads weights over the BAR spill). Record both numbers.

- [ ] **Step 6: End-to-end sanity on GLM's init path**

Run: `cd c && make colibri VK=1 2>&1 | grep -E 'warning|error'`; then confirm the engine still boots the backend without a model, exactly as CI does:
```bash
mkdir -p /tmp/vkprobe && echo '{"model_type":"glm_moe_dsa"}' > /tmp/vkprobe/config.json
COLI_NO_OMP_TUNE=1 COLI_VULKAN=1 SNAP=/tmp/vkprobe timeout 120 ./colibri 2>&1 | grep -E '\[VK\]' | head -5
```
Expected: `[VK] weights: staged device-local uploads ...` then `[VK] ready: AMD Radeon RX ...`.

- [ ] **Step 7: Commit**

```bash
git add c/backend_vulkan.c
git commit -m "vk: staged device-local weight uploads for cards without Resizable BAR

Resident weights go to a DEVICE_LOCAL-only arena through a host staging buffer
and vkCmdCopyBuffer when the host-visible slice is small (or COLI_VK_STAGED=1).
Scratches, KV mirror and readbacks keep their memory types. Harness is
bit-identical in both modes on RX 580 (gfx803).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

---

### Task 4: `coli_vk_default_spv`

**Files:**
- Modify: `c/backend_vulkan.c`, `c/backend_vulkan.h`

**Interfaces:**
- Produces: `const char *coli_vk_default_spv(char *buf, size_t n)`.

- [ ] **Step 1: Failing check in the harness**

In the harness `main`, change the first line to resolve the default when no argument is given:

```c
    char spvbuf[1024];
    const char *spv = argc > 1 ? argv[1] : coli_vk_default_spv(spvbuf, sizeof spvbuf);
    printf("shader: %s\n", spv);
```

Run: `cd c && gcc -O3 -DVK_TEST backend_vulkan.c -o /tmp/test_vk -lvulkan -lm 2>&1 | head -2`
Expected: `implicit declaration of function 'coli_vk_default_spv'`.

- [ ] **Step 2: Declare and implement**

Header, after the `coli_vk_staged` declaration:

```c
/* Resolve the main shader path the way every engine does: COLI_VK_SHADERS (the qmatmul.spv
 * file or the directory holding it), then shaders/qmatmul.spv beside the executable
 * (Linux), then the CWD-relative default. buf must hold at least 1024 bytes. */
const char *coli_vk_default_spv(char *buf, size_t n);
```

Source: add `#include <sys/stat.h>` and, under `#ifdef __linux__`, `#include <unistd.h>` to the includes. After `coli_vk_staged` add:

```c
const char *coli_vk_default_spv(char *buf, size_t n) {
    const char *env = getenv("COLI_VK_SHADERS");
    struct stat st;
    if (env && *env) {
        if (!stat(env, &st) && S_ISDIR(st.st_mode)) { snprintf(buf, n, "%s/qmatmul.spv", env); return buf; }
        return env;
    }
#ifdef __linux__
    ssize_t k = readlink("/proc/self/exe", buf, n - 1);
    if (k > 0) {
        buf[k] = 0;
        char *sl = strrchr(buf, '/');
        if (sl && (size_t)(sl + 1 - buf) + sizeof("shaders/qmatmul.spv") <= n) {
            strcpy(sl + 1, "shaders/qmatmul.spv");
            if (!stat(buf, &st)) return buf;
        }
    }
#endif
    return "shaders/qmatmul.spv";
}
```

- [ ] **Step 3: Verify all three resolutions**

Run from `c/`:
```bash
gcc -O3 -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -DVK_TEST backend_vulkan.c -o /tmp/test_vk -lvulkan -lm 2>&1 | grep -E 'warning|error'
/tmp/test_vk 2>&1 | head -1                                  # CWD default
COLI_VK_SHADERS=$PWD/shaders /tmp/test_vk 2>&1 | head -1     # directory form
COLI_VK_SHADERS=$PWD/shaders/qmatmul.spv /tmp/test_vk 2>&1 | head -1   # file form
```
Expected: `shader: shaders/qmatmul.spv`, then the absolute directory form with `/qmatmul.spv` appended, then the absolute file path. (Full harness output follows; let it run or Ctrl-C after the first line.)

- [ ] **Step 4: Commit**

```bash
git add c/backend_vulkan.c c/backend_vulkan.h
git commit -m "vk: coli_vk_default_spv shared shader-path resolver

Same lookup colibri.c and kimi_k3.c each carry privately; the qwen36 tier uses
this one. The two existing copies are left for a later cleanup.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

---

### Task 5: Tier header guard, `qt_backend_name`, and the CUDA-side shim

This task restructures the tier around a shim while keeping CUDA semantics byte-for-byte. It must compile as CUDA (syntax-only, no toolkit here) and as nothing (stubs).

**Files:**
- Modify: `c/qwen36_tier.h`
- Modify: `c/qwen36_tier.c`

**Interfaces:**
- Produces: `const char *qt_backend_name(void)`; internal shim names `QtTensor`, `QT_BACKEND`, `QT_SWAPS`, `QT_SINGLE_DEV`, `QT_BUDGET_ENV`, `be_enabled`, `be_init`, `be_device_count`, `be_available_device_count`, `be_mem_info`, `be_upload`, `be_free`, `be_issue`, `be_take`, `be_stats`, `be_shutdown`.

- [ ] **Step 1: Header**

In `c/qwen36_tier.h`:

1. Replace the top comment's last paragraph (`Enable with COLI_CUDA=1 ...` through `CPU-only with zero overhead. */`) with:
   ```c
    * Backends: CUDA (`make qwen36 CUDA=1`, COLI_CUDA=1 [COLI_GPUS=0,1]
    * [CUDA_EXPERT_GB=<G>|auto]) or Vulkan (`make qwen36 VK=1`, COLI_VULKAN=1
    * [VK_EXPERT_GB=<G>|auto]); both take [HEAT_FILE=<path>] [QT_NO_WARMSTART=1].
    * CUDA wins when both are compiled in. The Vulkan tier is single-device and
    * fills ONCE at warmstart (no runtime LFRU swaps: the Vulkan weight arena
    * never reclaims a freed slice, so a swap would leak one expert of VRAM).
    * Without either define the inline stubs below keep the engine CPU-only
    * with zero overhead. */
   ```
2. `#ifdef COLI_CUDA` becomes `#if defined(COLI_CUDA) || defined(COLI_VULKAN)`.
3. After `int  qt_ready(void);` add `const char *qt_backend_name(void);   /* "CUDA" | "Vulkan" (valid after qt_init) */`.
4. `#else /* !COLI_CUDA: inline stubs, engine stays CPU-only */` becomes `#else /* no GPU backend: inline stubs, engine stays CPU-only */`, and add `static inline const char *qt_backend_name(void){return "none";}` after the `qt_ready` stub.
5. `#endif /* COLI_CUDA */` becomes `#endif /* COLI_CUDA || COLI_VULKAN */`.

- [ ] **Step 2: Shim block in the source**

In `c/qwen36_tier.c` replace the first three lines

```c
/* qwen36_tier.c -- CUDA VRAM expert tier for the qwen36 engine. See header. */
#ifdef COLI_CUDA
#include <stdio.h>
```

with

```c
/* qwen36_tier.c -- VRAM expert tier for the qwen36 engine (CUDA or Vulkan). See header. */
#if defined(COLI_CUDA) || defined(COLI_VULKAN)
#include <stdio.h>
```

and replace `#include "backend_cuda.h"` (just below `#include "qwen36_tier.h"`) with the shim:

```c
/* ---- backend shim ------------------------------------------------------------
 * Everything below the shim is backend-agnostic placement logic; only these
 * operations differ. CUDA wins when both are compiled in (docs/qwen36-tier.md). */
#if defined(COLI_CUDA)
#include "backend_cuda.h"
#define QT_BACKEND    "CUDA"
#define QT_SWAPS      1            /* runtime LFRU swaps (backend frees and reuses VRAM) */
#define QT_SINGLE_DEV 0
#define QT_BUDGET_ENV "CUDA_EXPERT_GB"
typedef ColiCudaTensor QtTensor;
static int  be_enabled(void){ const char *e=getenv("COLI_CUDA"); return e && *e=='1'; }
static int  be_init(const int *dev,int n){ return coli_cuda_init(dev,n); }
static int  be_available_device_count(void){ return coli_cuda_available_device_count(); }
static int  be_device_count(void){ return coli_cuda_device_count(); }
static void be_mem_info(int dev,size_t *freeb,size_t *totb){ coli_cuda_mem_info(dev,freeb,totb); }
/* fmt: gs>0 -> grouped int4 (CUDA fmt 4), else per-row int4 (fmt 2); offset-binary nibbles */
static int  be_upload(QtTensor **t,const uint8_t *w,const float *sc,int I,int O,int dev,int gs){
    return gs ? coli_cuda_tensor_upload_g(t,w,sc,4,I,O,dev,gs) : coli_cuda_tensor_upload(t,w,sc,2,I,O,dev); }
static void be_free(QtTensor *t){ coli_cuda_tensor_free(t); }
static int  be_issue(QtTensor *const *g,QtTensor *const *u,QtTensor *const *d,const int *rows,int c,const float *x){
    return coli_cuda_expert_group_issue(g,u,d,rows,c,x); }
static const float *be_take(int dev,float *ybuf){ (void)ybuf; return coli_cuda_expert_group_take(dev); }
static void be_stats(int dev,size_t *tc,size_t *tb){ coli_cuda_stats(dev,tc,tb); }
static void be_shutdown(void){ coli_cuda_shutdown(); }
#elif defined(COLI_VULKAN)
#error "Vulkan shim arrives in the next commit"
#endif
```

- [ ] **Step 3: Use the shim in the placement logic**

Apply these mechanical replacements in `c/qwen36_tier.c` below the shim:

| find | replace |
|---|---|
| `ColiCudaTensor *tg, *tu, *td;` (in `QSlot`) | `QtTensor *tg, *tu, *td;` |
| `ColiCudaTensor *a=v->tg,*b=v->tu,*ct=v->td;` | `QtTensor *a=v->tg,*b=v->tu,*ct=v->td;` |
| `if(a)coli_cuda_tensor_free(a); if(b)coli_cuda_tensor_free(b); if(ct)coli_cuda_tensor_free(ct);` | `if(a)be_free(a); if(b)be_free(b); if(ct)be_free(ct);` |
| `ColiCudaTensor *tg=NULL,*tu=NULL,*td=NULL;` | `QtTensor *tg=NULL,*tu=NULL,*td=NULL;` |
| the whole `if(G.egs){ ok = coli_cuda_tensor_upload_g(...) ... } else { ok = coli_cuda_tensor_upload(...) ... }` block | see below |
| `const char *e=getenv("COLI_CUDA");\n    if(!(e && *e=='1')) return 0;` | `if(!be_enabled()) return 0;` |
| `int available=coli_cuda_available_device_count();` | `int available=be_available_device_count();` |
| `if(!coli_cuda_init(G.dev,G.ndev)){ fprintf(stderr,"[qtier] coli_cuda_init failed -> CPU path\n"); return 0; }` | `if(!be_init(G.dev,G.ndev)){ fprintf(stderr,"[qtier] %s backend init failed -> CPU path\n",QT_BACKEND); return 0; }` |
| `int have=coli_cuda_device_count();` | `int have=be_device_count();` |
| `const char *bg=getenv("CUDA_EXPERT_GB");` | `const char *bg=getenv(QT_BUDGET_ENV);` |
| `coli_cuda_mem_info(G.dev[i],&freeb,&totb);` | `be_mem_info(G.dev[i],&freeb,&totb);` |
| `fprintf(stderr,"[qtier] CUDA VRAM expert tier active: %d device(s), %.2f MB/expert\n",` | `fprintf(stderr,"[qtier] %s VRAM expert tier active: %d device(s), %.2f MB/expert\n", QT_BACKEND,` |
| `ColiCudaTensor *tg[QT_MAX_DEV][32],*tu[QT_MAX_DEV][32],*td[QT_MAX_DEV][32];` | `QtTensor *tg[QT_MAX_DEV][32],*tu[QT_MAX_DEV][32],*td[QT_MAX_DEV][32];` |
| `if(!coli_cuda_expert_group_issue(tg[di],tu[di],td[di],rows,c,xr)){` | `if(!be_issue(tg[di],tu[di],td[di],rows,c,xr)){` |
| `const float *y=coli_cuda_expert_group_take(G.dev[di]);` | `const float *y=be_take(G.dev[di],G.ybuf);` |
| `size_t tc=0,tb=0; coli_cuda_stats(G.dev[i],&tc,&tb);` | `size_t tc=0,tb=0; be_stats(G.dev[i],&tc,&tb);` |
| `coli_cuda_shutdown();` (in `qt_shutdown`) | `be_shutdown();` |

The upload block in `uploader` becomes:

```c
        int dv = G.dev[home(eid)];
        size_t mb=(size_t)G.D*G.Ih/2;
        QtTensor *tg=NULL,*tu=NULL,*td=NULL;
        /* grouped (gs64) containers carry [O,ceil(I/gs)] scales, per-row ones [O] */
        int ok = be_upload(&tg, w,      sc,          G.D,  G.Ih, dv, G.egs)
              && be_upload(&tu, w+mb,   sc+G.sc_gu,  G.D,  G.Ih, dv, G.egs)
              && be_upload(&td, w+2*mb, sc+2*G.sc_gu,G.Ih, G.D,  dv, G.egs);
```

(Check against the original: in the per-row branch the original used `sc+G.Ih` and `sc+2*G.Ih`; with `egs==0`, `G.sc_gu == Ih`, so the unified form is identical.)

Add `float *ybuf;  /* Vulkan take() target [32*D]; unused on CUDA */` to the `G` struct next to `is_x`, and in `qt_init` after `G.is_x=malloc(...)` change the check to allocate it too:

```c
    G.is_x=malloc((size_t)32*D*sizeof(float));
    G.ybuf=malloc((size_t)32*D*sizeof(float));
    if(!G.slot||!G.is_x||!G.ybuf) return 0;
```

Wrap the device-list block in `qt_init` (from `/* devices: COLI_GPUS="0,1" ...` through `if(G.ndev<1){ fprintf(stderr,"[qtier] no visible CUDA devices -> CPU path\n"); return 0; }`) as:

```c
#if QT_SINGLE_DEV
    G.ndev=1; G.dev[0]=0;
#else
    ...existing block unchanged...
#endif
```

Wrap the swap half of `qt_lfru_tick_locked`: keep the decay (`G.tick++; if(!(G.tick%1024)) ...`) unconditional and put everything from `if(G.tick%16) return;` to the end of the function inside `#if QT_SWAPS ... #endif`.

In `qt_stats`, wrap the `coli_cuda_group_stats` block (`{ uint64_t calls=0,ex=0,rows=0; ... }`) in `#ifdef COLI_CUDA ... #endif`.

Add after `int qt_ready(void){ return G.on; }`:

```c
const char *qt_backend_name(void){ return QT_BACKEND; }
```

Change the final `#endif /* COLI_CUDA */` to `#endif /* COLI_CUDA || COLI_VULKAN */`.

- [ ] **Step 4: Verify both compile shapes**

Run from `c/`:
```bash
gcc -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-unused-function -DCOLI_CUDA qwen36_tier.c && echo CUDA-OK
gcc -fsyntax-only -Wall -Wextra -Wno-unused-parameter -Wno-unused-function qwen36_tier.c && echo STUB-OK
make qwen36 2>&1 | grep -E 'warning|error'; echo "plain build exit=$?"
grep -n 'coli_cuda_' qwen36_tier.c | grep -vE '^[0-9]+:(#include|static|/\*| \*)' | grep -v 'be_' 
```
Expected: `CUDA-OK`, `STUB-OK`, no warnings (grep exit 1), and the last grep prints only the shim lines plus the `#ifdef COLI_CUDA` group-stats block (no stray direct CUDA calls elsewhere).

- [ ] **Step 5: Commit**

```bash
git add c/qwen36_tier.h c/qwen36_tier.c
git commit -m "qwen36 tier: backend shim, qt_backend_name

No behaviour change on CUDA: the placement logic now calls ten be_* operations
and a Vulkan implementation slots in next. Header guard admits COLI_VULKAN.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

---

### Task 6: Vulkan shim + the tier test

**Files:**
- Create: `c/tests/test_qwen36_tier_vk.c`
- Modify: `c/qwen36_tier.c` (the `#elif defined(COLI_VULKAN)` branch)
- Modify: `c/Makefile` (one test rule; the `TEST_RULES` scan picks it up automatically as a portable gate, because without `VK=1` the test compiles to a skip)

**Interfaces:**
- Consumes: `coli_vk_init`, `coli_vk_default_spv`, `coli_vk_available`, `coli_vk_mem_budget`, `coli_vk_mem_info`, `coli_vk_tensor_ensure`, `coli_vk_tensor_free`, `coli_vk_expert_group_issue`, `coli_vk_expert_group_take`, `coli_vk_shutdown`; tier API from `qwen36_tier.h`.
- Env: `VK_EXPERT_GB`.

- [ ] **Step 1: Write the failing test**

Create `c/tests/test_qwen36_tier_vk.c`:

```c
/* Qwen3.6 Vulkan expert tier gate: drives qwen36_tier.c through the shared Vulkan
 * backend with synthetic int4 experts (no model file) and checks the accumulated
 * routed-expert output against a CPU reference. Built into every `make check`;
 * without VK=1 it compiles to a skip, and with VK=1 but no usable device it skips
 * at runtime (exit 0) so CPU hosts and CI stay green. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#ifndef COLI_VULKAN
int main(void){ puts("skip: built without VK=1"); return 0; }
#else
#include "../qwen36_tier.h"
#include "../backend_vulkan.h"

static int fail(const char *m){ fprintf(stderr,"qwen36 tier vk test failed: %s\n",m); return 1; }
static uint32_t g_rng=0x9E3779B9u;
static uint32_t rnd(void){ g_rng=g_rng*1664525u+1013904223u; return g_rng>>8; }
static float frand(void){ return (float)(rnd()%2001)/1000.f-1.f; }   /* [-1,1] */

/* synthetic geometry (Qwen3.6-35B-A3B is D=2048, I=768; tiny fixture is 64/32) */
enum { NL=2, NE=8, D=64, IH=32, TOPK=2 };
typedef struct { uint8_t *g4,*u4,*d4; float *gs,*us,*ds; } Exp;
static Exp E[NL][NE];

/* packed two's-complement int4 (LOW nibble = even column), per-row f32 scales:
 * the container layout qwen36.c hands the tier; the tier XORs to offset-binary. */
static void make_expert(Exp *e){
    size_t mb=(size_t)D*IH/2;
    e->g4=malloc(mb); e->u4=malloc(mb); e->d4=malloc(mb);
    e->gs=malloc(IH*sizeof(float)); e->us=malloc(IH*sizeof(float)); e->ds=malloc(D*sizeof(float));
    for(size_t i=0;i<mb;i++){ e->g4[i]=(uint8_t)rnd(); e->u4[i]=(uint8_t)rnd(); e->d4[i]=(uint8_t)rnd(); }
    for(int o=0;o<IH;o++){ e->gs[o]=0.01f+0.02f*(float)(rnd()%100)/100.f; e->us[o]=0.01f+0.02f*(float)(rnd()%100)/100.f; }
    for(int o=0;o<D;o++) e->ds[o]=0.01f+0.02f*(float)(rnd()%100)/100.f;
}
static float deq(const uint8_t *row,int i){ int nib=(i&1)?(row[i>>1]>>4):(row[i>>1]&15); return (float)((nib&8)?nib-16:nib); }
/* y[O] = (x[I] . W[O,I]) * s[O] */
static void gemv(float *y,const float *x,const uint8_t *w,const float *s,int I,int O){
    for(int o=0;o<O;o++){ const uint8_t *row=w+(size_t)o*((I+1)/2); double a=0;
        for(int i=0;i<I;i++) a+=x[i]*deq(row,i); y[o]=(float)a*s[o]; }
}
static void expert_ref(float *y,const float *x,const Exp *e){
    float g[IH],u[IH];
    gemv(g,x,e->g4,e->gs,D,IH); gemv(u,x,e->u4,e->us,D,IH);
    for(int i=0;i<IH;i++){ float v=g[i]; g[i]=(v/(1.f+expf(-v)))*u[i]; }
    gemv(y,g,e->d4,e->ds,IH,D);
}
static void note_all(void){
    for(int l=0;l<NL;l++) for(int e=0;e<NE;e++)
        qt_note_block(l,e,E[l][e].g4,E[l][e].u4,E[l][e].d4,E[l][e].gs,E[l][e].us,E[l][e].ds);
    qt_fill_wait();
}
static int count_resident(int layer){ int n=0; for(int e=0;e<NE;e++) n+=qt_is_resident(layer,e); return n; }

/* One tier init for the whole run: the backend is not designed to be torn down and
 * brought up again inside one process (arenas outlive coli_vk_shutdown). The budget
 * admits exactly two experts (per-expert bytes = 3*D*IH/2 + (2*IH+D)*4 + 4096 = 7680;
 * 0.00002 GB = 21474 bytes), and the natural warmstart order fills layer 0, eids 0 and 1,
 * so those two are the resident pair and eid 2 is a guaranteed miss. */

/* resident experts, served from VRAM, reproduce the CPU int4 path */
static int resident_experts_match_cpu_path(void){
    if(!qt_is_resident(0,0)||!qt_is_resident(0,1)) return fail("warmstart should have placed layer 0 eids 0 and 1");
    float x[D]; for(int i=0;i<D;i++) x[i]=frand();
    int eids[TOPK]={0,1}; float val[TOPK]={0.7f,0.3f};
    uint32_t mask=qt_issue(0,eids,TOPK,x);
    if(mask!=3u) return fail("both resident experts should be served by the GPU");
    float out[D]={0}, ref[D]={0}, y[D];
    qt_take(mask,val,TOPK,out);
    for(int k=0;k<TOPK;k++){ expert_ref(y,x,&E[0][eids[k]]); for(int d=0;d<D;d++) ref[d]+=val[k]*y[d]; }
    double maxrel=0;
    for(int d=0;d<D;d++){ double den=fabs(ref[d])>1e-3?fabs(ref[d]):1e-3; double r=fabs(out[d]-ref[d])/den; if(r>maxrel) maxrel=r; }
    printf("resident_experts_match_cpu_path: maxrel %.3e\n",maxrel);
    if(maxrel>2e-3) return fail("GPU expert output diverges from the CPU reference");
    return 0;
}

/* a non-resident expert returns no mask bit so the engine computes it on the CPU */
static int misses_fall_back_to_cpu(void){
    if(qt_is_resident(0,2)) return fail("eid 2 should not fit the two-expert budget");
    float x[D]; for(int i=0;i<D;i++) x[i]=frand();
    int eids[1]={2}; float val[1]={1.f}; float out[D]={0};
    uint32_t mask=qt_issue(0,eids,1,x);
    qt_take(mask,val,1,out);
    if(mask!=0) return fail("non-resident expert must not be claimed by the GPU");
    for(int d=0;d<D;d++) if(out[d]!=0.f) return fail("GPU wrote output for a miss");
    puts("misses_fall_back_to_cpu: ok");
    return 0;
}

/* heat the misses hard, past the CUDA tier's 16-tick swap check and its 25%+4
 * hysteresis; on Vulkan nothing may move. */
static int residency_frozen_after_warmstart(void){
    float x[D]; for(int i=0;i<D;i++) x[i]=frand();
    float val[32]; for(int i=0;i<32;i++) val[i]=1.f;
    float out[D]={0};
    int before[NL][NE]; for(int l=0;l<NL;l++) for(int e=0;e<NE;e++) before[l][e]=qt_is_resident(l,e);
    for(int t=0;t<64;t++){
        for(int l=0;l<NL;l++){
            int hot[NE]; int n=0; for(int e=0;e<NE;e++) if(!before[l][e]) hot[n++]=e;
            for(int k=0;k<n;k++) qt_note(l,hot[k],E[l][hot[k]].g4,E[l][hot[k]].u4,E[l][hot[k]].d4,E[l][hot[k]].gs,E[l][hot[k]].us,E[l][hot[k]].ds);
            uint32_t m=qt_issue(l,hot,n,x); qt_take(m,val,n,out);
            if(m) return fail("a non-resident expert was served from VRAM");
        }
    }
    qt_fill_wait();
    for(int l=0;l<NL;l++) for(int e=0;e<NE;e++)
        if(qt_is_resident(l,e)!=before[l][e]) return fail("residency changed after warmstart (Vulkan tier must fill once)");
    puts("residency_frozen_after_warmstart: ok");
    return 0;
}

int main(void){
    for(int l=0;l<NL;l++) for(int e=0;e<NE;e++) make_expert(&E[l][e]);
    setenv("COLI_VULKAN","1",1);
    setenv("VK_EXPERT_GB","0.00002",1);
    unsetenv("HEAT_FILE"); unsetenv("QT_NO_WARMSTART");
    if(!qt_init(NL,NE,D,IH,NE,TOPK,0)){ puts("skip: no usable Vulkan device"); return 0; }
    note_all();
    int res=count_resident(0)+count_resident(1);
    if(res!=2) { fprintf(stderr,"resident=%d\n",res); return fail("budget should admit exactly two experts"); }
    int r=resident_experts_match_cpu_path();
    if(!r) r=misses_fall_back_to_cpu();
    if(!r) r=residency_frozen_after_warmstart();
    qt_shutdown();
    return r;
}
#endif
```

Note on the budget number: per-expert bytes are `3*D*IH/2 + (2*IH + D)*4 + 4096` = `3072 + 512 + 4096 = 7680`; `0.00002 GB = 21474 bytes` admits two experts (single device). If the test reports "budget should admit exactly two experts" with a different count, adjust the constant, not the assertion.

- [ ] **Step 2: Makefile rule and run it to see it fail**

In `c/Makefile`, directly after the `tests/test_qwen36_cache_index$(EXE):` rule, add:

```make
# Qwen3.6 Vulkan expert-tier gate: synthetic experts through qwen36_tier.c and the
# shared Vulkan backend, no model. Compiles to a skip without VK=1 (so it is a
# portable gate like the rest of TEST_BINS) and skips at runtime without a device.
tests/test_qwen36_tier_vk$(EXE): tests/test_qwen36_tier_vk.c qwen36_tier.c qwen36_tier.h tier.h backend_vulkan.h $(VK_OBJ) $(VK_SPV)
	$(CC) $(NOCUDA_CFLAGS) $< qwen36_tier.c $(VK_OBJ) -o $@ $(NOCUDA_LDFLAGS)
```

Run: `cd c && make tests/test_qwen36_tier_vk VK=1 2>&1 | grep -E 'error' | head -3`
Expected: the `#error "Vulkan shim arrives in the next commit"` from Task 5.

Also confirm the plain build skips: `make tests/test_qwen36_tier_vk && ./tests/test_qwen36_tier_vk` → `skip: built without VK=1`.

- [ ] **Step 3: Implement the Vulkan shim**

Replace the `#elif defined(COLI_VULKAN)` / `#error` lines in `c/qwen36_tier.c` with:

```c
#elif defined(COLI_VULKAN)
#include "backend_vulkan.h"
#define QT_BACKEND    "Vulkan"
#define QT_SWAPS      0            /* fill once: the VK arena never reclaims freed slices */
#define QT_SINGLE_DEV 1
#define QT_BUDGET_ENV "VK_EXPERT_GB"
typedef ColiVkTensor QtTensor;
static int  be_enabled(void){ const char *e=getenv("COLI_VULKAN"); return e && *e=='1'; }
static int  be_init(const int *dev,int n){ (void)dev; (void)n;
    char buf[1024]; return coli_vk_init(coli_vk_default_spv(buf,sizeof buf)); }
static int  be_available_device_count(void){ return 1; }   /* enumeration happens in coli_vk_init */
static int  be_device_count(void){ return coli_vk_available() ? 1 : 0; }
static void be_mem_info(int dev,size_t *freeb,size_t *totb){ (void)dev;
    double used=0,budget=0;
    if(coli_vk_mem_budget(&used,&budget)){
        *totb=(size_t)(budget*1e9); *freeb=budget>used?(size_t)((budget-used)*1e9):0;
    } else {
        *totb=*freeb=(size_t)4<<30;
        fprintf(stderr,"[qtier] VK_EXT_memory_budget absent: assuming 4 GB free (set VK_EXPERT_GB to override)\n");
    }
}
/* fmt: gs>0 -> grouped int4 (VK fmt 4, [O,ceil(I/gs)] scales), else per-row int4 (fmt 2).
 * The VK i4() decoder is nibble-8, the same offset-binary layout stage() produces. */
static int  be_upload(QtTensor **t,const uint8_t *w,const float *sc,int I,int O,int dev,int gs){
    (void)dev; return coli_vk_tensor_ensure(t,w,sc,gs?4:2,I,O,gs); }
static void be_free(QtTensor *t){ coli_vk_tensor_free(t); }
static int  be_issue(QtTensor *const *g,QtTensor *const *u,QtTensor *const *d,const int *rows,int c,const float *x){
    return coli_vk_expert_group_issue(g,u,d,rows,c,x); }
static const float *be_take(int dev,float *ybuf){ (void)dev; return coli_vk_expert_group_take(ybuf) ? ybuf : NULL; }
static void be_stats(int dev,size_t *tc,size_t *tb){ (void)dev; coli_vk_mem_info(tb,tc); }
static void be_shutdown(void){ coli_vk_shutdown(); }
#endif
```

Two details to check while editing: `coli_vk_mem_info(size_t *used_bytes, size_t *tensor_count)` takes bytes first, hence the swapped arguments; and `be_take` must return `NULL` when the group was never issued so `qt_take`'s existing `if(!y) continue;` skips it.

- [ ] **Step 4: Build and run the test on the RX 580**

Run:
```bash
cd c && make tests/test_qwen36_tier_vk VK=1 2>&1 | grep -E 'warning|error'
COLI_NO_OMP_TUNE=1 ./tests/test_qwen36_tier_vk
```
Expected output (order):
```
[VK] weights: staged device-local uploads (...)
[VK] ready: AMD Radeon RX ...
[qtier] dev 0: ... GB free, budget 0.0 GB (~2 experts)
[qtier] Vulkan VRAM expert tier active: 1 device(s), 0.01 MB/expert
resident_experts_match_cpu_path: maxrel <= 2e-3
misses_fall_back_to_cpu: ok
residency_frozen_after_warmstart: ok
```
exit 0. Also run it with `COLI_VK_STAGED=0` to prove the tier is independent of the upload mode. If `maxrel` exceeds `2e-3`, first suspect the scale-pointer arithmetic in `be_upload` for the down projection (`sc+2*G.sc_gu`, `G.sc_d` floats) and print the first row of `out` vs `ref`; do not loosen the tolerance.

- [ ] **Step 5: Plain build still green**

Run: `cd c && make check 2>&1 | tail -5`
Expected: all gates pass; `test_qwen36_tier_vk` prints `skip: built without VK=1`.

- [ ] **Step 6: Commit**

```bash
git add c/qwen36_tier.c c/tests/test_qwen36_tier_vk.c c/Makefile
git commit -m "qwen36 tier: Vulkan backend (single device, fill once)

Routed experts are served from VRAM through coli_vk_expert_group_issue/take.
Budget via VK_EXPERT_GB (auto = device budget minus 1 GB). No runtime LFRU
swaps: the Vulkan arena never reclaims a freed slice, so residency is decided
at warmstart (HEAT_FILE order). test_qwen36_tier_vk checks GPU-vs-CPU output
and the fill-once rule; it skips without VK=1 or a device.

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

---

### Task 7: Engine banner + notice, `make qwen36 VK=1`, test targets

**Files:**
- Modify: `c/qwen36.c` (the `qt_init` call site, ~line 2721)
- Modify: `c/Makefile` (qwen36 block ~lines 1013–1023; the five test/bench targets that compile `qwen36.c`)

**Interfaces:**
- Consumes: `qt_backend_name()` from Task 5.

- [ ] **Step 1: Engine**

In `c/qwen36.c` replace

```c
    /* Optional CUDA VRAM expert tier (COLI_CUDA=1): hot experts live in
     * DEVICE_LOCAL memory across the configured GPUs, misses fall back to the
     * CPU int8 path. See qwen36_tier.h. */
    if (qt_init(m.c.n_layers, m.c.n_experts, m.c.hidden, m.c.inter, cap, m.c.topk, m.c.expert_gs)) {
        fprintf(stderr, "[gpu] MoE experts -> CUDA VRAM tier\n");
```

with

```c
    /* Optional VRAM expert tier (COLI_CUDA=1 or COLI_VULKAN=1): hot experts live in
     * DEVICE_LOCAL memory, misses fall back to the CPU int8 path. See qwen36_tier.h. */
#ifndef COLI_VULKAN
    if (getenv("COLI_VULKAN"))
        fprintf(stderr, "[qwen36] COLI_VULKAN is set but this binary was built without VK=1 "
                        "(no Vulkan tier); running on CPU\n");
#endif
    if (qt_init(m.c.n_layers, m.c.n_experts, m.c.hidden, m.c.inter, cap, m.c.topk, m.c.expert_gs)) {
        fprintf(stderr, "[gpu] MoE experts -> %s VRAM tier\n", qt_backend_name());
```

- [ ] **Step 2: Makefile qwen36 block**

Replace

```make
ifeq ($(CUDA),1)
QWEN36_TIER_SRC = qwen36_tier.c
QWEN36_CFLAGS   = $(CFLAGS)
QWEN36_LDFLAGS  = $(LDFLAGS)
else
QWEN36_TIER_SRC =
QWEN36_CFLAGS   = $(NOCUDA_CFLAGS)
QWEN36_LDFLAGS  = $(NOCUDA_LDFLAGS)
endif
qwen36$(EXE): qwen36.c cli_args.h qwen36_tier.h st.h json.h compat.h $(QWEN36_TIER_SRC) $(CUDA_OBJ)
	$(CC) $(QWEN36_CFLAGS) qwen36.c $(QWEN36_TIER_SRC) $(CUDA_OBJ) -o qwen36$(EXE) $(QWEN36_LDFLAGS)
```

with

```make
# VK=1 (without CUDA) compiles the same tier against the shared Vulkan backend;
# NOCUDA_* already carry -DCOLI_VULKAN and -lvulkan when VK=1. CUDA wins if both.
ifeq ($(CUDA),1)
QWEN36_TIER_SRC = qwen36_tier.c
QWEN36_CFLAGS   = $(CFLAGS)
QWEN36_LDFLAGS  = $(LDFLAGS)
else ifeq ($(VK),1)
QWEN36_TIER_SRC = qwen36_tier.c
QWEN36_CFLAGS   = $(NOCUDA_CFLAGS)
QWEN36_LDFLAGS  = $(NOCUDA_LDFLAGS)
else
QWEN36_TIER_SRC =
QWEN36_CFLAGS   = $(NOCUDA_CFLAGS)
QWEN36_LDFLAGS  = $(NOCUDA_LDFLAGS)
endif
qwen36$(EXE): qwen36.c cli_args.h qwen36_tier.h st.h json.h compat.h $(QWEN36_TIER_SRC) $(CUDA_OBJ) $(VK_OBJ) $(VK_SPV)
	$(CC) $(QWEN36_CFLAGS) qwen36.c $(QWEN36_TIER_SRC) $(CUDA_OBJ) $(VK_OBJ) -o qwen36$(EXE) $(QWEN36_LDFLAGS)
```

Also update the block comment above it (`# MoE). With CUDA=1 the optional VRAM expert tier (qwen36_tier.c) is compiled`) to read `# MoE). With CUDA=1 or VK=1 the optional VRAM expert tier (qwen36_tier.c) is compiled`.

- [ ] **Step 3: Test targets that compile qwen36.c**

For each of `tests/test_qwen36_ctx`, `tests/test_qwen36_dense_batch`, `tests/test_qwen36_json_escape`, `tests/bench_qwen36_dense_batch`, `tests/test_qwen36_cache_index`: append ` $(VK_OBJ)` to the prerequisite list after `$(CUDA_OBJ)`, and insert ` $(VK_OBJ)` after `$(CUDA_OBJ)` in the recipe's compile line. Example for the first:

```make
tests/test_qwen36_ctx$(EXE): tests/test_qwen36_ctx.c qwen36.c qwen36_tier.h st.h json.h compat.h $(QWEN36_TIER_SRC) $(CUDA_OBJ) $(VK_OBJ)
	$(CC) $(CFLAGS) $< $(QWEN36_TIER_SRC) $(CUDA_OBJ) $(VK_OBJ) -o $@ $(LDFLAGS)
```

- [ ] **Step 4: Build matrix**

Run from `c/`:
```bash
make qwen36 2>&1 | grep -E 'warning|error'; echo plain=$?
make qwen36 VK=1 2>&1 | grep -E 'warning|error'; echo vk=$?
ldd qwen36 | grep -c libvulkan
python3 tests/test_makefile_cuda_scope.py 2>&1 | tail -2
make qwen36-tiny-check 2>&1 | tail -3
make qwen36-tiny-check VK=1 2>&1 | tail -3
make qwen36 >/dev/null 2>&1 && COLI_VULKAN=1 SNAP=/nonexistent ./qwen36 2>&1 | grep -m1 'COLI_VULKAN is set'
```
Expected: both grep exits are 1 (no warnings), `1` libvulkan link, the scope test passes, both tiny checks pass, and the final line prints `[qwen36] COLI_VULKAN is set but this binary was built without VK=1 (no Vulkan tier); running on CPU`. (The notice sits after model load in the engine; if `SNAP=/nonexistent` exits before reaching it, point `SNAP` at the tiny fixture `qwen36-tiny-generate` produces instead.)

- [ ] **Step 5: Commit**

```bash
git add c/qwen36.c c/Makefile
git commit -m "qwen36: make qwen36 VK=1 builds the Vulkan tier; name the backend

The banner names CUDA or Vulkan, and a build without the tier says so once
when COLI_VULKAN is set instead of ignoring it (refs #894).

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

---

### Task 8: Docs, environment table, changelog, CI

**Files:**
- Modify: `docs/vulkan.md`
- Rename: `docs/qwen36-cuda-tier.md` → `docs/qwen36-tier.md`, then modify
- Modify: `docs/qwen36.md` (lines 66–69)
- Modify: `docs/ENVIRONMENT.md` (Vulkan table; Qwen3.6 section)
- Modify: `CHANGELOG.md` (`## [Unreleased]`)
- Modify: `.github/workflows/ci.yml` (the `vulkan` job)

- [ ] **Step 1: `docs/vulkan.md`**

Replace the paragraph starting `**Discrete cards need Resizable BAR.**` (through `Unified-memory APUs are unaffected.`) with:

```markdown
**Resizable BAR is faster; staged uploads make it optional.** The weight tiers
prefer HOST_VISIBLE|DEVICE_LOCAL memory. With ReBAR disabled that combination
only exists in a ~256 MB BAR window, and the driver silently places everything
beyond it in system RAM — the tier then *reports* resident experts while every
access crosses PCIe, slower than the CPU path (measured 0.11 vs 0.24 tok/s
either side of the BIOS toggle on an RX 9070 XT). When the backend sees a small
host-visible slice it now switches to **staged uploads**: resident weights go
to plain DEVICE_LOCAL memory through a host staging buffer and
`vkCmdCopyBuffer` (`[VK] weights: staged device-local uploads` in the banner).
`COLI_VK_STAGED=1` forces the staged path on any card, `=0` forces the mapped
path. Enable Resizable BAR / Smart Access Memory in the BIOS when you can; it
removes the copy at warmstart. Unified-memory APUs have no device-local-only
memory and are unaffected either way.
```

In "Limits and future work", change `Polaris/gfx803 validation on real hardware` to reflect what Task 9 measures (leave the bullet in place until then; Task 9 edits it with numbers).

- [ ] **Step 2: Tier doc**

```bash
git mv docs/qwen36-cuda-tier.md docs/qwen36-tier.md
```

Edit `docs/qwen36-tier.md`:

- Title: `# qwen36: VRAM expert tier (CUDA or Vulkan)`.
- First paragraph: change `computed there through the existing shared CUDA backend (`backend_cuda.cu` expert-group API — no new backend).` to `computed there through the existing shared CUDA or Vulkan backend (`backend_cuda.cu` / `backend_vulkan.c` expert-group API — no new backend).`
- After the `## Usage` block (before `## Measured ...`) add:

```markdown
## Vulkan tier (`make -C c qwen36 VK=1`)

Any Vulkan 1.2 device — AMD via Mesa/RADV (including Polaris cards ROCm
dropped), Intel ANV, NVIDIA. Needs `libvulkan` and `glslc` at build time, like
the GLM Vulkan backend (see [vulkan.md](vulkan.md)).

```bash
make -C c qwen36 VK=1
COLI_VULKAN=1 HEAT_FILE=heat.bin VK_EXPERT_GB=auto \
OMP_NUM_THREADS=<physical cores> COLI_NO_OMP_TUNE=1 \
SNAP=<container> N_NEW=200 ./c/qwen36 256 4 prompt.txt
```

Differences from the CUDA tier:

- **Single device.** `COLI_GPUS` is not read; the backend picks the most
  capable Vulkan device (discrete > integrated).
- **Fill once.** Residency is decided at warmstart — `HEAT_FILE` order when the
  file exists, natural order otherwise — up to `VK_EXPERT_GB` (`auto` = the
  driver's device-local budget minus 1 GB). There are no runtime LFRU swaps:
  the Vulkan weight arena never reclaims a freed slice, so each swap would leak
  one expert of VRAM. Heat still accumulates and saves at exit, so the second
  run starts hot. `QT_NO_WARMSTART=1` switches to filling on first use, which
  is still fill-once.
- **No Resizable BAR needed.** Discrete cards without ReBAR get real VRAM
  residency through the backend's staged uploads (`COLI_VK_STAGED`, see
  [vulkan.md](vulkan.md)).
- **CUDA wins** when a binary is built with both `CUDA=1` and `VK=1`.
- Numerics: the same offset-binary int4 layout as the CUDA upload, so
  `test_qwen36_tier_vk` (built into `make check`) holds the GPU output to
  within 2e-3 relative of the CPU int4 path.
```

- [ ] **Step 3: `docs/qwen36.md`**

Replace lines 66–69:

```markdown
Requirements: ~30 GB RAM for comfortable expert caching and NVMe storage for
the container. The default build is CPU-only; `make -C c qwen36 CUDA=1` or
`make -C c qwen36 VK=1` adds the optional VRAM expert tier documented in
[`qwen36-tier.md`](qwen36-tier.md).
```

- [ ] **Step 4: `docs/ENVIRONMENT.md`**

In the Vulkan table, after the `COLI_VK_SPIN_US` row add:

```markdown
| `COLI_VK_STAGED` | auto | `1` forces staged device-local weight uploads (host staging buffer + copy), `0` forces mapped host-visible uploads. Auto: staged when the host-visible slice is under a quarter of VRAM (Resizable BAR off). The `[VK] weights:` banner reports the mode. |
```

In the Qwen3.6 section, change the intro sentence to `Read **only** by `c/qwen36.c` and its expert tier `c/qwen36_tier.c`.` and add rows:

```markdown
| `COLI_VULKAN` | off | With a `make qwen36 VK=1` build, `=1` enables the Vulkan VRAM expert tier (single device, fill-once). See [qwen36-tier.md](qwen36-tier.md). |
| `VK_EXPERT_GB` | `auto` | Vulkan tier VRAM budget in GB; `auto` = device-local budget minus 1 GB (falls back to 4 GB with a warning when `VK_EXT_memory_budget` is absent). Mirrors `CUDA_EXPERT_GB`. |
```

- [ ] **Step 5: `CHANGELOG.md`**

Under `## [Unreleased]`, before the first `###` heading, add:

```markdown
### Qwen3.6 Vulkan expert tier, and Vulkan on cards without Resizable BAR

- `make qwen36 VK=1` builds the qwen36 VRAM expert tier against the shared
  Vulkan backend (AMD via RADV including Polaris, Intel, NVIDIA): single
  device, fill-once at warmstart, `VK_EXPERT_GB` budget. The engine now says
  so when `COLI_VULKAN` is set on a build without the tier (refs #894).
- The Vulkan backend uploads resident weights through a host staging buffer
  into plain device-local memory when the host-visible slice is small, so
  discrete cards without Resizable BAR keep real VRAM residency instead of
  silently spilling to system RAM (`COLI_VK_STAGED` forces either mode).
  Queue submits and arena allocation are now mutex-protected.
- First validation of the Vulkan backend on Polaris/gfx803 (RX 580).
```

- [ ] **Step 6: CI**

In `.github/workflows/ci.yml`, in the `vulkan` job's `Build with VK=1` step, after the `ldd colibri` line add:

```yaml
          make qwen36 VK=1
          ldd qwen36 | grep -q libvulkan || { echo "FAIL: qwen36 built without linking libvulkan"; exit 1; }
```

and add a new step after `Initialise the backend against Lavapipe`:

```yaml
      - name: Qwen3.6 Vulkan tier gate on Lavapipe
        run: |
          cd c
          make tests/test_qwen36_tier_vk VK=1
          # Lavapipe is all host-visible memory, so the mapped path runs here; the staged
          # path needs a discrete card and is covered by the hardware validation in the PR.
          VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json COLI_NO_OMP_TUNE=1 \
            ./tests/test_qwen36_tier_vk 2>&1 | tee tier.log
          grep -q "residency_frozen_after_warmstart: ok" tier.log || {
            echo "FAIL: the Vulkan tier gate did not run to completion"; exit 1; }
```

- [ ] **Step 7: Check links and the doc-vars drift the maintainer's procedure describes**

Run:
```bash
grep -rn 'qwen36-cuda-tier' --include='*.md' --include='*.mdx' --include='*.json' . | grep -v superpowers
cd c && for v in COLI_VK_STAGED VK_EXPERT_GB; do grep -c "\`$v\`" ../docs/ENVIRONMENT.md; grep -l "getenv(\"$v\")" *.c; done
```
Expected: no stale links; each variable appears in the doc and in exactly one source file (`backend_vulkan.c`, `qwen36_tier.c`).

- [ ] **Step 8: Commit**

```bash
git add docs/vulkan.md docs/qwen36-tier.md docs/qwen36.md docs/ENVIRONMENT.md CHANGELOG.md .github/workflows/ci.yml
git commit -m "docs, ci: Qwen3.6 Vulkan tier and staged uploads

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

---

### Task 9: Hardware validation on the RX 580 and PR

**Files:**
- Modify: `docs/vulkan.md` (Polaris numbers), `docs/qwen36-tier.md` (a Polaris row)

- [ ] **Step 1: Ask before the download**

The pre-converted container is ~20 GB from Hugging Face. Ask the user: "OK to run `hf download Kreuzzelg/qwen36-35b-a3b-colibri-i4-gs64 --local-dir ~/Models/qwen36_i4_gs64`?" (needs `pip install huggingface_hub` if `hf` is missing). Stop until answered.

- [ ] **Step 2: CPU baseline with a frozen heat file**

From `c/` with the plain build (`make qwen36`):
```bash
echo "Write a haiku about mountains, then explain the syllable structure." > /tmp/p.txt
SNAP=~/Models/qwen36_i4_gs64 N_NEW=64 OMP_NUM_THREADS=8 COLI_NO_OMP_TUNE=1 \
  ./qwen36 256 4 /tmp/p.txt --temp 0 > /tmp/cpu.out 2> /tmp/cpu.err
tail -3 /tmp/cpu.err
```
Record tok/s and TTFT from stderr.

- [ ] **Step 3: Vulkan tier, cold then warm heat**

```bash
make qwen36 VK=1
rm -f /tmp/heat.bin
COLI_VULKAN=1 HEAT_FILE=/tmp/heat.bin VK_EXPERT_GB=auto SNAP=~/Models/qwen36_i4_gs64 N_NEW=64 \
  OMP_NUM_THREADS=8 COLI_NO_OMP_TUNE=1 ./qwen36 256 4 /tmp/p.txt --temp 0 > /tmp/vk1.out 2> /tmp/vk1.err
cp /tmp/heat.bin /tmp/heat.frozen
cp /tmp/heat.frozen /tmp/heat.bin
COLI_VULKAN=1 HEAT_FILE=/tmp/heat.bin VK_EXPERT_GB=auto SNAP=~/Models/qwen36_i4_gs64 N_NEW=64 \
  OMP_NUM_THREADS=8 COLI_NO_OMP_TUNE=1 ./qwen36 256 4 /tmp/p.txt --temp 0 > /tmp/vk2.out 2> /tmp/vk2.err
cp /tmp/heat.frozen /tmp/heat.bin
COLI_VULKAN=1 HEAT_FILE=/tmp/heat.bin VK_EXPERT_GB=auto SNAP=~/Models/qwen36_i4_gs64 N_NEW=64 \
  OMP_NUM_THREADS=8 COLI_NO_OMP_TUNE=1 ./qwen36 256 4 /tmp/p.txt --temp 0 > /tmp/vk3.out 2> /tmp/vk3.err
grep -E '\[VK\] weights|\[qtier\]' /tmp/vk2.err
diff <(cat /tmp/vk2.out) <(cat /tmp/vk3.out) && echo "GPU run-to-run: token-identical"
diff <(cat /tmp/cpu.out) <(cat /tmp/vk2.out) && echo "CPU vs GPU: token-identical" || echo "CPU vs GPU: differs (report the position; issue 510 explains near-tie flips)"
```
Record for the PR: `[VK] weights` banner, `[qtier]` resident count and hit rate, tok/s for CPU / VK cold / VK warm, and the two diffs. Runs 2 and 3 must be token-identical (frozen placement); if they are not, that is a bug to fix before the PR.

Also run the mapped-path arm once with `COLI_VK_STAGED=0` (same frozen heat file) to show the spill cost on this card; that single number is what justifies the staged path in the PR.

- [ ] **Step 4: Record the numbers**

In `docs/vulkan.md`, replace the `Polaris/gfx803 validation on real hardware` bullet with a one-line measured entry (hardware, Mesa version, staged vs mapped tok/s, harness result) and add the same to `docs/qwen36-tier.md` under a `## Measured (RX 580 8 GB, i7-7700K, Qwen3.6-35B-A3B int4-gs64, 64-token decode)` heading with rows for CPU-only, Vulkan cold heat, Vulkan warm heat, and Vulkan mapped path (`COLI_VK_STAGED=0`). Commit:

```bash
git add docs/vulkan.md docs/qwen36-tier.md
git commit -m "docs: Polaris (RX 580) measurements for the Vulkan tier and staged uploads

Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>
Claude-Session: https://claude.ai/code/session_019BacNGNxAJ1M57UdYE2M3N"
```

- [ ] **Step 5: Final gates, then hand off**

```bash
cd c && make clean >/dev/null && make check 2>&1 | tail -3 && make qwen36 VK=1 2>&1 | grep -cE 'warning' 
```
Expected: check green, `0` warnings. Then invoke `superpowers:finishing-a-development-branch`. The branch's first commit (the spec) and the plan file must be dropped from the PR: `git rebase -i` is not available here, so use `git rebase --onto upstream/dev <spec-commit-sha> qwen36-vulkan-tier` after moving the two `docs/superpowers` files aside, or keep them in a separate local branch. Push to `origin` and open the PR against `JustVugg/colibri:dev` with the template: Summary (problem + smallest change), Validation (harness both modes, tier test, tiny-check both builds, the RX 580 table, the frozen-heat diffs), Compatibility (default build unchanged; CUDA tier unchanged; `VK=1` is opt-in). End the PR body with the required attribution lines.
