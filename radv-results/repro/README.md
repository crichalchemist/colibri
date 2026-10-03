# MoltenVK repros (Radeon Pro 580)

Both programs read their `.spv` from the current directory. Run from this directory.

## Build
macOS (MacPorts headers/loader):
    glslc --target-env=vulkan1.2 sgprobe.comp -o sgprobe.spv
    glslc --target-env=vulkan1.2 dynoff.comp  -o dynoff.spv
    cc -O2 -Wall -Wextra -I/opt/local/include sgprobe.c -o sgprobe -L/opt/local/lib -lvulkan
    cc -O2 -Wall -Wextra -I/opt/local/include dynoff.c  -o dynoff  -L/opt/local/lib -lvulkan
Linux (RADV) -- not yet run (Linux visit 1):
    glslc --target-env=vulkan1.2 sgprobe.comp -o sgprobe.spv   # same for dynoff
    cc -O2 -Wall -Wextra sgprobe.c -o sgprobe -lvulkan
    cc -O2 -Wall -Wextra dynoff.c  -o dynoff  -lvulkan
macOS builds: 0 warnings (sgprobe.c uses designated initializers for the three properties structs; see task-8-report.md).

## Run (macOS, four cells: sdk142|macports141 x argument buffers 1|0)
    ../run_matrix.sh repro.sgprobe ./sgprobe
    for args in "4 160 1 0" "4 160 0 0" "4 160 1 1" "1 160 1 0"; do ../run_matrix.sh "repro.dynoff.${args// /_}" ./dynoff ${=args}; done   # zsh
dynoff args: K N private(0|1) static(0|1). Ignore run_matrix's fails= column; read the program's own lines.
Linux run lines -- not yet run (Linux visit 1): run `./sgprobe` and `./dynoff K N priv stat` directly.
Layer run: add VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation VK_LAYER_REPORT_FLAGS=error,warn,perf VK_LAYER_DUPLICATE_MESSAGE_LIMIT=0 VK_LOADER_DEBUG=layer VK_LAYER_PATH=<dir with the layer json>.

## Observed on macOS (2026-10-02)
sgprobe, sdk142 (MoltenVK 1.4.2), both modes:
    device AMD Radeon Pro 580 | driver MoltenVK 1.4.2 | subgroupSize 32 ...
    gl_SubgroupSize 32 | gl_SubgroupInvocationID max=63 | gl_SubgroupID max=3 | gl_NumSubgroups 4 | subgroupAdd(1) 64
    sgprobe: reported 32, counted 64 -> INCONSISTENT   (rc=1)
sgprobe, macports141 (1.4.1), both modes: reported 64, counted 64 -> CONSISTENT (rc=0).
dynoff, all four cells, all four configs: every window "ok", PASS. It did NOT reproduce defect A here.
Pass output per window: `window k: y0=<got> want <want> ok`; if offsets were ignored the first
window would show `<- x window W, tag T` and later windows `untouched`.

## mvk_shared_module/ — defect A, minimal (one module, a static and a dynamic pipeline)
Build (macOS): `cd mvk_shared_module && glslc --target-env=vulkan1.2 copy.comp -o copy.spv && cc -O2 -Wall -Wextra -I/opt/local/include mvk_shared_module.c -o mvk_shared_module -L/opt/local/lib -lvulkan`
Build (Linux, not yet run — Linux visit 1): `cd mvk_shared_module && glslc --target-env=vulkan1.2 copy.comp -o copy.spv && cc -O2 -Wall -Wextra mvk_shared_module.c -o mvk_shared_module -lvulkan`
Run: `./mvk_shared_module <own 0|1> <dynamic-first 0|1>`. On macOS: through `../../run_matrix.sh mvkmod.<a>_<b> ./mvk_shared_module a b`.
Expected on MoltenVK 1.4.1 and 1.4.2 (2026-10-02, logs/mvkmod.*): `0 0` FAILs with argument buffers on (window 0 = 17, window 1 untouched) and PASSes with them off; `0 1`, `1 0` and `1 1` PASS everywhere.
Expected on RADV: all four PASS. That is the non-MoltenVK control for A.

## spvc_alias/ — defect C, offline (no GPU, macOS only: needs xcrun metal)
`glslc --target-env=vulkan1.2 alias.comp -o alias.spv`
`~/VulkanSDK/1.4.357.1/macOS/bin/spirv-cross alias.spv --msl --msl-version 30000 --msl-argument-buffers --msl-dynamic-buffer 0 0 --output alias.dyn.metal`
`xcrun -sdk macosx metal -std=metal3.0 -c alias.dyn.metal -o /dev/null` gives `use of undeclared identifier '_27'`. Without `--msl-dynamic-buffer 0 0` it compiles.

Update (addendum 3): dynoff's sixth argument `twin` does reproduce A. `./dynoff 4 160 0 0 <gap> 1` FAILs with argument buffers on, on 1.4.1 and 1.4.2, and PASSes with them off; `... 2` (own module) PASSes everywhere. The "did NOT reproduce" line above refers to twin 0 only.
