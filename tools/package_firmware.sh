#!/usr/bin/env bash
set -euo pipefail

build_dir="${1:-build}"
output_dir="${2:-dist}"

files=(
  "$build_dir/bootloader/bootloader.bin"
  "$build_dir/partition_table/partition-table.bin"
  "$build_dir/flipforge_bridge.bin"
  "$build_dir/flipforge_bridge.elf"
  "$build_dir/flash_args"
  "$build_dir/flasher_args.json"
)

for file in "${files[@]}"; do
  if [[ ! -f "$file" ]]; then
    echo "Missing build artifact: $file" >&2
    exit 1
  fi
done

mkdir -p "$output_dir/bootloader" "$output_dir/partition_table"
cp "$build_dir/bootloader/bootloader.bin" "$output_dir/bootloader/bootloader.bin"
cp "$build_dir/partition_table/partition-table.bin" "$output_dir/partition_table/partition-table.bin"
cp "$build_dir/flipforge_bridge.bin" "$output_dir/flipforge_bridge.bin"
cp "$build_dir/flipforge_bridge.elf" "$output_dir/flipforge_bridge.elf"
cp "$build_dir/flash_args" "$output_dir/flash_args"
cp "$build_dir/flasher_args.json" "$output_dir/flasher_args.json"

if command -v sha256sum >/dev/null 2>&1; then
  (cd "$output_dir" && sha256sum bootloader/bootloader.bin partition_table/partition-table.bin flipforge_bridge.bin flipforge_bridge.elf flash_args flasher_args.json > SHA256SUMS)
else
  (cd "$output_dir" && shasum -a 256 bootloader/bootloader.bin partition_table/partition-table.bin flipforge_bridge.bin flipforge_bridge.elf flash_args flasher_args.json > SHA256SUMS)
fi
