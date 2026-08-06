#!/usr/bin/env bash
# 将 applications/*/README.md 同步到 docs/applications/ 作为站点文档源
# 单一数据源原则: README 改动后, CI 与本地预览均通过本脚本同步
# 仅同步已有长篇 README; 手写文档(data-collect/laser-ctrl/spi-demo/canopen-demo)不受影响
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
APPS_DIR="$ROOT_DIR/applications"
DOCS_APPS_DIR="$ROOT_DIR/docs/applications"

mkdir -p "$DOCS_APPS_DIR"

sync_one() {
  local src="$1" dst="$2"
  if [[ -f "$src" ]]; then
    cp "$src" "$dst"
    echo "[sync] $src -> $dst"
  else
    echo "[warn] 源文件不存在, 跳过: $src" >&2
  fi
}

# 已有长篇 README → 站点文档 (保持同步, 避免重复维护)
sync_one "$APPS_DIR/mod_handler/README.md" "$DOCS_APPS_DIR/mod-handler.md"
sync_one "$APPS_DIR/dapLink/README.md"     "$DOCS_APPS_DIR/zephyrlink.md"

echo "[sync] 完成"
