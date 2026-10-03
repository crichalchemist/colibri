#!/bin/bash
# pinned diffusers (the SHA make_qwenimage_tiny.py documents), then rerun mimo-qwenimage.
set -u
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
R=$HOME/radv-results; S=$R/chain4.txt; : > $S
say() { echo "$*" >> $S; }
V=$HOME/venv-colibri
"$V/bin/pip" install "git+https://github.com/huggingface/diffusers@80c7ed262aeffbeb43ef13ae04baeb9b84515a69" > $R/pip-diffusers-pinned.log 2>&1 \
  || { say "pip pinned diffusers FAILED rc=$?"; tail -3 $R/pip-diffusers-pinned.log >> $S; exit 1; }
say "pinned ready: $("$V/bin/python" -c 'import diffusers; from diffusers import AutoencoderKLQwenImage21; print("diffusers", diffusers.__version__, "QwenImage21 classes importable")' 2>&1 | tail -1)"
cd ~/colibri-radv && git switch --detach 0baf0c63 >/dev/null 2>&1 && cd c || { say "switch FAILED"; exit 1; }
t=$(date +%s); PY="$V/bin/python" bash tests/vulkan_engines.sh mimo-qwenimage > $R/sweep.mimo-qwenimage.rerun2.log 2>&1; rc=$?
say "mimo-qwenimage rerun2 exit=$rc ($(( $(date +%s)-t ))s) last='$(tail -1 $R/sweep.mimo-qwenimage.rerun2.log)'"
say "CHAIN4 COMPLETE"
