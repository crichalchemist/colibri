#!/bin/bash
# Steps 5, 3 and glm attribution — sequential, one GPU job at a time.
set -u
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
R=$HOME/radv-results; S=$R/chain2.txt; : > $S
say() { echo "$*" >> $S; }
cd ~/colibri-radv || exit 1

# --- dev: step 5 full harness, then glm attribution ---
git switch --detach 77647eeb >/dev/null 2>&1; cd c; make clean >/dev/null 2>&1
cc -O2 -Wall -Wextra -pthread -DVK_TEST backend_vulkan.c -o vk_test -lvulkan -lm > $R/dev.harness-full.build.log 2>&1
say "step5 dev build warnings=$(grep -ci warning $R/dev.harness-full.build.log)"
t=$(date +%s); ./vk_test shaders/qmatmul.spv > $R/dev.harness-full.log 2>&1; rc=$?
say "step5 dev harness-full rc=$rc last='$(tail -1 $R/dev.harness-full.log)' ($(( $(date +%s)-t ))s)"
t=$(date +%s); bash tests/vulkan_engines.sh glm > $R/sweep.glm.dev.log 2>&1; rc=$?
say "glm-on-dev exit=$rc ($(( $(date +%s)-t ))s) last='$(tail -1 $R/sweep.glm.dev.log)'"

# --- comb: step 3 layers, then glm rerun ---
cd ~/colibri-radv; git switch --detach 0baf0c63 >/dev/null 2>&1; cd c; make clean >/dev/null 2>&1
make tests/test_vk_tier VK=1 > $R/comb.layers.build.log 2>&1
cc -O2 -Wall -Wextra -pthread -DVK_TEST backend_vulkan.c -o vk_test -lvulkan -lm >> $R/comb.layers.build.log 2>&1
say "step3 comb build warnings=$(grep -ci warning $R/comb.layers.build.log)"
t=$(date +%s)
VK_LOADER_DEBUG=layer VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation COLI_VK_TEST_MATMUL_ONLY=1 ./vk_test shaders/qmatmul.spv > $R/comb.layers.harness.log 2>&1; rc=$?
say "step3 layers harness rc=$rc last='$(tail -1 $R/comb.layers.harness.log)' ($(( $(date +%s)-t ))s)"
t=$(date +%s)
VK_LOADER_DEBUG=layer VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation ./tests/test_vk_tier shaders/qmatmul.spv > $R/comb.layers.tier.log 2>&1; rc=$?
say "step3 layers tier rc=$rc last='$(tail -1 $R/comb.layers.tier.log)' ($(( $(date +%s)-t ))s)"
t=$(date +%s); bash tests/vulkan_engines.sh glm > $R/sweep.glm.comb-rerun.log 2>&1; rc=$?
say "glm-on-comb-rerun exit=$rc ($(( $(date +%s)-t ))s) last='$(tail -1 $R/sweep.glm.comb-rerun.log)'"
say "CHAIN2 COMPLETE"
