#!/bin/bash
set -e

bash build_kernel.sh psyche
bash build_kernel.sh thyme
bash build_kernel.sh umi
bash build_kernel.sh munch
bash build_kernel.sh lmi
bash build_kernel.sh cmi
bash build_kernel.sh cas
bash build_kernel.sh apollo
bash build_kernel.sh alioth
bash build_kernel.sh elish
bash build_kernel.sh enuma
bash build_kernel.sh dagu
bash build_kernel.sh pipa

bash build_kernel.sh psyche ksu
bash build_kernel.sh thyme ksu
bash build_kernel.sh umi ksu
bash build_kernel.sh munch ksu
bash build_kernel.sh lmi ksu
bash build_kernel.sh cmi ksu
bash build_kernel.sh cas ksu
bash build_kernel.sh apollo ksu
bash build_kernel.sh alioth ksu
bash build_kernel.sh elish ksu
bash build_kernel.sh enuma ksu
bash build_kernel.sh dagu ksu
bash build_kernel.sh pipa ksu