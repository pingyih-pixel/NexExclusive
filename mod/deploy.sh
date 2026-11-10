#!/usr/bin/env bash
# 仅向用户明确给定的 PID 注入已构建 DLL，不构建、不选择或启动进程。
set -euo pipefail

usage() {
    printf '用法：%s <pid> [dll]\n' "$0" >&2
    printf '请通过外部授权流程启动或选择目标进程，再明确提供 PID；本脚本不会启动、停止或探测进程。\n' >&2
}
if (( $# < 1 || $# > 2 )); then
    usage
    exit 2
fi
PID=$1
if [[ ! "$PID" =~ ^[0-9]+$ ]] || (( ${#PID} > 10 )); then
    printf 'PID 必须是 1 到 4294967295 的十进制整数。\n' >&2
    exit 2
fi
if (( 10#$PID < 1 || 10#$PID > 4294967295 )); then
    printf 'PID 必须是 1 到 4294967295 的十进制整数。\n' >&2
    exit 2
fi

MOD_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
OUT_DIR=${OUT_DIR-$MOD_DIR}
if [[ "$OUT_DIR" != /* && ! "$OUT_DIR" =~ ^[[:alpha:]]:[/\\] && "$OUT_DIR" != \\\\* ]]; then
    printf 'OUT_DIR 必须是绝对目录：%s\n' "$OUT_DIR" >&2
    exit 2
fi
OUT_DIR="$(cygpath -au -- "$OUT_DIR")"
DLL=${2-$OUT_DIR/nexsink.dll}
if [[ -z "$DLL" ]]; then
    printf 'DLL 路径不能为空。\n' >&2
    exit 2
fi
DLL="$(cygpath -au -- "$DLL")"
if [[ ! -f "$DLL" ]]; then
    printf '已构建 DLL 不存在或不是普通文件：%s\n' "$DLL" >&2
    exit 2
fi
if [[ ! -f "$OUT_DIR/inject2.exe" || ! -x "$OUT_DIR/inject2.exe" ]]; then
    printf '请先构建注入器：%s/inject2.exe\n' "$OUT_DIR" >&2
    exit 2
fi
DLL_WINDOWS="$(cygpath -aw -- "$DLL")"
# 不使用输出管道或吞掉错误，原样返回注入器的退出码。
exec "$OUT_DIR/inject2.exe" "$PID" "$DLL_WINDOWS"
