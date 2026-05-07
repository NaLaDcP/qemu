#!/bin/sh
set -e

if [ -z "$IMAGE_FILE" ]; then
  echo "ERROR: IMAGE_FILE n'est pas défini (ex: IMAGE_FILE=/images/Image)"
  exit 1
fi

exec qemu-system-aarch64 \
  -machine virt,virtualization=on \
  -cpu cortex-a53 \
  -device "loader,file=${IMAGE_FILE},addr=0x70000000,cpu-num=0" \
  -serial mon:stdio \
  -d irq,guest_errors \
  -D /logs/irq.log \
  -m size=2G \
  -nographic \
  "$@"
