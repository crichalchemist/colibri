#!/bin/bash
# Step 2 of HANDOFF.md, one tree. usage: run_tree.sh <name> <sha>
set -u
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json
name=$1; sha=$2; R=$HOME/radv-results
cd ~/colibri-radv || exit 1
git switch --detach "$sha" >/dev/null 2>&1 || { echo "$name: SWITCH FAILED"; exit 1; }
echo "$name: HEAD=$(git rev-parse --short=8 HEAD)"
cd c || exit 1
make clean >/dev/null 2>&1
echo -n "$name: tier-build warnings="
make tests/test_vk_tier VK=1 > "$R/$name.build.log" 2>&1
grep -ci warning "$R/$name.build.log"
[ -x tests/test_vk_tier ] || { echo "$name: TIER BINARY MISSING"; tail -15 "$R/$name.build.log"; exit 1; }
echo -n "$name: harness-build warnings="
cc -O2 -Wall -Wextra -pthread -DVK_TEST backend_vulkan.c -o vk_test -lvulkan -lm > "$R/$name.ccbuild.log" 2>&1
grep -ci warning "$R/$name.ccbuild.log"
[ -x vk_test ] || { echo "$name: VK_TEST BINARY MISSING"; tail -15 "$R/$name.ccbuild.log"; exit 1; }
./tests/test_vk_tier shaders/qmatmul.spv > "$R/$name.tier.log" 2>&1
echo "$name: tier last-line: $(tail -1 "$R/$name.tier.log")"
COLI_VK_TEST_MATMUL_ONLY=1 ./vk_test shaders/qmatmul.spv > "$R/$name.harness.log" 2>&1
echo "$name: harness last-line: $(tail -1 "$R/$name.harness.log")"
echo "$name: --- ready/warning/xbatch ---"
grep -E '^\[VK\] (ready|warning)|^xbatch fmt' "$R/$name.harness.log" | sort -u
echo "$name: xbatch lines NOT '(GEMM from 16)': $(grep -c '^xbatch fmt' "$R/$name.harness.log" 2>/dev/null) total, $(grep '^xbatch fmt' "$R/$name.harness.log" 2>/dev/null | grep -vc "GEMM from 16") deviating"
