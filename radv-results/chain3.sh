#!/bin/bash
# venv with diffusers, then rerun mimo-qwenimage once chain2 has released the GPU.
set -u
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
R=$HOME/radv-results; S=$R/chain3.txt; : > $S
say() { echo "$*" >> $S; }
V=$HOME/venv-colibri
python3 -m venv --system-site-packages "$V" > $R/venv.log 2>&1 || { say "venv create FAILED"; exit 1; }
"$V/bin/pip" install diffusers > $R/pip-diffusers.log 2>&1 || { say "pip diffusers FAILED rc=$?"; tail -3 $R/pip-diffusers.log >> $S; exit 1; }
say "venv ready: $("$V/bin/python" -c 'import diffusers,torch,transformers,safetensors;print("diffusers",diffusers.__version__,"torch",torch.__version__)' 2>&1 | tail -1)"
until grep -q 'CHAIN2 COMPLETE' $R/chain2.txt 2>/dev/null; do sleep 5; done
say "gpu free; rerunning mimo-qwenimage on comb with PY=$V/bin/python"
cd ~/colibri-radv && git switch --detach 0baf0c63 >/dev/null 2>&1 && cd c || { say "switch FAILED"; exit 1; }
t=$(date +%s); PY="$V/bin/python" bash tests/vulkan_engines.sh mimo-qwenimage > $R/sweep.mimo-qwenimage.rerun.log 2>&1; rc=$?
say "mimo-qwenimage rerun exit=$rc ($(( $(date +%s)-t ))s) last='$(tail -1 $R/sweep.mimo-qwenimage.rerun.log)'"
say "CHAIN3 COMPLETE"
