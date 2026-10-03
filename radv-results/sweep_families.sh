#!/bin/bash
set -u
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
cd ~/colibri-radv/c || exit 1
: > ~/radv-results/sweep.txt
echo "shader exit=0 (run separately)" >> ~/radv-results/sweep.txt
for m in qwen inkling-olmoe mimo-qwenimage kimi deepseek glm; do
  s=$(date +%s)
  bash tests/vulkan_engines.sh "$m" > ~/radv-results/sweep.$m.log 2>&1
  rc=$?
  echo "$m exit=$rc ($(( $(date +%s) - s ))s)" >> ~/radv-results/sweep.txt
done
echo "SWEEP COMPLETE" >> ~/radv-results/sweep.txt
