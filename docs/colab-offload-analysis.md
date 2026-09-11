# Attaching a Google Colab instance to colibrì: seam analysis

Question asked: is there an avenue to attach a Colab (CLI-driven) instance to
speed up on-device inference?

Short answer: **the seam exists, and decode cannot use it.** The existing
distributed path is a LAN protocol whose cost model inverts over a WAN. There
are three things Colab *is* worth attaching to, none of them on the decode path.
This document shows the arithmetic, then names the avenues that survive it.

## 1. The seam that already exists

colibrì already ships a distributed expert-execution path. It is not a stub:

| piece | location |
|---|---|
| wire protocol (`COLIEX01`, v1) | `c/colibri.c:3073-3145` |
| coordinator side (per-layer batch send/recv) | `cluster_moe_batch`, `c/colibri.c:3146-3192` |
| worker side (accept loop, disk-backed FFN) | `cluster_worker_run`, `c/colibri.c:3216-3283` |
| dispatch into the MoE loop | `c/colibri.c:5560-5563` |
| worker entry (`EXPERT_WORKER=1`) | `c/colibri.c:10886-10890` |
| coordinator attach (`CLUSTER_WORKERS`) | `c/colibri.c:11093-11096` |
| discovery / registry control plane | `c/cluster.py`, `coli cluster {coordinator,worker}` |

The shape is sound for its stated target: the coordinator keeps tokens, routing
and KV local, and ships a layer's **routed batch-union** as one request so a
token does not pay one round trip per expert (`README.md:170-200`).

The attach point for any remote accelerator is therefore
`CLUSTER_WORKERS=host:port` — a single environment variable, no code change.
That is the avenue. What follows is why it does not pay.

## 2. The arithmetic that kills decode offload

Use DeepSeek V4 Flash, whose geometry is documented in-tree
(`docs/deepseek-v4.md:59-60`): **43 layers, hidden 4096, 256 routed experts,
top-6**.

### Payload, per token, at decode (S=1)

Per layer the coordinator sends one f32 activation row per unique routed expert
and receives one f32 output row back (`cluster_moe_batch`, activations are raw
little-endian f32 — no quantization on the wire, no compression):

```
up   per layer = 6 experts x (8 B hdr + 4096 x 4 B) =  98,352 B  ~ 96 KiB
down per layer = same                                            ~ 96 KiB
per token      = 43 layers x 2 x 96 KiB                          ~ 8.1 MiB
```

To sustain even **5 tok/s** that is ~20 MiB/s **each way** — about 169 Mbit/s
symmetric, sustained, for one user. Consumer uplink is the binding constraint
and it is typically 10-40 Mbit/s. The upload alone puts the ceiling near
0.5 tok/s before any compute happens.

GLM-5.2 is worse in every term (744B, more layers, wider hidden); Kimi K3 is
93 layers (`docs/kimi_k3.md:4`).

### Round trips, per token

`cluster_moe_batch` is **fully synchronous**: for each worker it writes the
whole request, then blocks reading the whole response, before touching the next
worker. So per token:

```
round trips = n_layers x n_workers = 43 x N   (N workers multiply RTT, they do not hide it)
```

At a realistic 40 ms RTT to a Colab region through a tunnel relay, one worker
gives `43 x 40 ms = 1.72 s/token` — a **0.58 tok/s ceiling from latency alone**,
with zero compute and zero bandwidth charged. The README's measured CPU-only
baseline is ~1.8 tok/s warm on a 128 GB desktop (`README.md:324`). Offloading to
Colab would be roughly **3x slower than doing nothing**, and adding workers
makes it worse, not better.

This is the whole answer for decode. No tunnel, region choice, or protocol
tuning closes a 3-10x gap that is structural.

### Prefill does not rescue it

The instinct is "prefill is batched, so ship that instead". It inverts the wrong
term. At S=2048 the per-layer payload scales with S: ~768 MiB up per layer for
DSV4. Prefill is more bandwidth-bound, not less. This avenue is closed.

## 3. Two defects in the existing cluster path (independent of Colab)

Found while reading the seam; these bite on LAN too and are worth filing
regardless of whether anyone attaches Colab.

**(a) The worker's expert cache is 1-deep per layer, so a multi-expert batch
guarantees a miss on every item after the first.** `cluster_worker_run` allocates
`ESlot *cache = calloc(nr_layers, sizeof(ESlot))` — one slot per layer
(`c/colibri.c:3219-3221`) — then loops the request's `n` items, all of which
carry the *same* layer, against that single slot:

```c
ESlot *slot = &cache[layer];
for (uint32_t j = 0; j < n; j++) {
    if (slot->eid != items[j].eid || !slot->slab) { ... expert_load(...); }
```

Every item with a different `eid` evicts the previous one and re-reads from
disk. The batch-union optimisation that makes the protocol worth having on the
wire is therefore undone on the worker: a 6-expert layer batch costs 6 cold disk
loads, every layer, every token. The fix is a per-layer slot set sized to the
batch cap (or at least to top-k), not a single `ESlot`.

**(b) A worker failure kills the coordinator process outright.** The `fail:`
label in `cluster_moe_batch` prints one line and calls `exit(1)`
(`c/colibri.c:3186-3190`) — there is no fallback to local expert compute and no
worker eviction. On a LAN that is a defensible fail-fast. Pointed at a
preemptible cloud VM it means **the remote machine's scheduler can terminate
your local inference process mid-token**, losing the session. Any
remote-attach story needs this to degrade to local compute instead.

## 4. Colab-specific blockers, beyond the arithmetic

Even if the numbers worked:

- **The worker needs the whole model on its own disk.** `cluster_worker_run`
  calls `st_init_multi(&m.S, snap, ...)` and streams experts from local storage
  — it is a *disk-backed* worker, not a weight-receiving one. Colab gives roughly
  110 GB (free) / ~220 GB (Pro) of ephemeral disk. GLM-5.2 is ~372 GB and
  GLM-5.3-Flash ~195 GB converted (`README.md:408-414`) — neither fits. Only
  DeepSeek V4 Flash REAP-150B (~85 GB) and Qwen3.6-35B-A3B (~20 GB) fit at all,
  and re-staging them on every session start costs more than the session saves.
- **No inbound ports.** `cluster_worker_run` binds `INADDR_ANY` and waits for
  the coordinator to connect *in*. Colab cannot accept inbound connections, so
  this requires a third-party tunnel relay — which adds the relay's RTT to all
  43xN round trips, worsening the dominant term.
- **Preemption and session caps** (idle timeout, 12h ceiling) collide directly
  with defect (b) above.
- **Terms of service.** Colab's terms prohibit using it as a remote backend for
  external services, remote-shell/tunnel access, and non-interactive background
  compute. A persistent expert worker driven by your desktop is squarely the
  prohibited pattern, not an edge case. Verify current terms before building on
  this; an approach that depends on a ToS violation is not an engineering plan.

## 5. Security posture — do not expose this protocol

If anyone does tunnel a worker, understand what is being exposed. The protocol
has **no authentication and no encryption**. `cluster_worker_run` accepts any
connection to `0.0.0.0:<port>` and validates only an 8-byte magic and a version
word. Anyone who finds the tunnel URL can drive arbitrary expert compute on the
worker, and malformed input reaches paths that `exit(1)` — a one-packet denial
of service. The registry in `c/cluster.py` has a Host-header rebinding guard but
no API key, and attaching a cross-host worker means widening `--allowed-host`.

This protocol was designed for "other Macs" on a trusted LAN (`README.md:171`)
and it is correct for that. It is not an internet-facing protocol and should not
be made into one without authentication and transport security.

## 6. What Colab is actually worth attaching to

The shape that wins is **high compute, tiny result, latency-insensitive**. Decode
is the opposite (low compute per round trip, large result, latency-critical).
Three things in this repo have the right shape:

### A. CUDA build and test farm — highest value, zero engine change

The repo carries a real CUDA/HIP backend (`c/backend_cuda.cu`, one source for
both vendors via `c/backend_gpu_compat.h`) and CUDA-only tests and benchmarks
that a contributor without an NVIDIA GPU simply cannot run:

- `c/tests/test_backend_cuda_dsv4.cu`
- `c/tests/bench_tensor_core.cu`
- `c/tests/bench_dsv4_deepgemm.cu`
- `make -C c glm CUDA=1 CUDA_ARCH=...` / the `qwen36` VRAM expert tier
  (`docs/qwen36-cuda-tier.md`), which measured **1.44 -> 10.05 tok/s** on two
  8 GB cards (`README.md:414`)

A Colab notebook that clones the repo, builds with `CUDA=1`, and runs those
tests turns "I cannot test the GPU path" into "I can". This speeds up
*development of* the thing that makes inference fast — it does not speed up a
user's inference. That distinction is the honest framing, and this is still the
best return of the three.

### B. Routing-heat map generation — the one with the right ratio

`.coli_usage` is the persistent expert-usage history (`c/route_trace.h`,
`c/colibri.c:11240-11245`). It drives `PIN=auto`, the usage-ranked partial
mirror (`coli mirror plan|stage|verify --usage`), and GPU tier selection. Its
format is one `uint32` count per expert per layer — for DSV4 that is
`43 x 256 x 4 B ~ 44 KB`.

So: generating it needs a full corpus run through the model (hours of compute);
consuming it locally needs a ~44 KB file. That is a compute-to-bandwidth ratio
around 10^6:1 — exactly the shape that survives a WAN, and the inverse of the
decode path.

Caveats that decide whether this is real for you:
- The map is only valid for the same model, and it is only *useful* if the
  Colab corpus resembles your actual workload. A generic corpus produces a
  generic pin set, which is worth less than the map your own machine accumulates
  for free across sessions.
- The model must still fit Colab's disk, so in practice this applies to
  Qwen3.6-35B-A3B and DSV4 REAP-150B, not GLM-5.2 or Kimi K3.
- It is a cold-start accelerator. It buys you a good pin set on day one instead
  of day three; it does not raise your steady-state ceiling.

### C. Model conversion — real but disk-capped

Converting a BF16 checkpoint to colibrì's int4-gs64 container is one-time,
latency-insensitive, and bandwidth-asymmetric: Colab pulls from Hugging Face
over Google's backbone, and you download only the *converted* result. For
GLM-5.3-Flash that is downloading ~195 GB instead of ~640 GB of BF16 source.

The blocker is disk: you cannot hold source and destination simultaneously for
any of the large models. This only works as a shard-at-a-time stream
(fetch shard -> quantize -> push to a bucket or HF repo -> delete), which is a
pipeline someone has to build and babysit across session preemptions. Worth it
if you are converting repeatedly; not worth it once.

## 7. The honest alternative, if the real goal is "use Colab's GPU"

If the underlying want is "run this faster than my CPU can, using a GPU I do not
own", the supported path already exists and needs no new code: run `coli serve`
*on* Colab for a model that fits its VRAM (Qwen3.6-35B-A3B at ~20 GB), and point
a client at it. `c/openai_server.py` already speaks the OpenAI protocol.

That is remote inference, not on-device speedup — the local machine contributes
nothing and the model is capped by Colab's VRAM, which is precisely the
constraint colibrì exists to escape. It is the opposite of this project's thesis
(`README.md:140-158`: parameters are data to be staged, not resident state to be
held). Stated here so the trade is explicit rather than discovered later.

## Conclusion

- **Decode offload to Colab: closed.** 43xN serial round trips and ~8 MiB/token
  each way put the ceiling ~3x *below* the local CPU baseline. This is
  structural, not tunable.
- **Prefill offload: closed.** Scales the wrong term.
- **The attach point is `CLUSTER_WORKERS`** and needs no code change — which
  makes it easy to try and easy to be misled by on a LAN benchmark that does not
  transfer.
- **Worth doing:** Colab as a CUDA build/test farm (A), and possibly as a
  routing-heat generator (B) for the two models that fit its disk.
- **Worth fixing regardless:** the 1-deep worker expert cache and the
  `exit(1)`-on-worker-failure coordinator, both of which cost real performance
  and robustness on the LAN configuration the feature actually targets.
