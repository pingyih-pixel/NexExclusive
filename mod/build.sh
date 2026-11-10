#!/usr/bin/env bash
# 使用现有 Zig 工具链构建；OUT_DIR 必须是绝对目录，默认本脚本所在的 mod。
# Zig 的位置默认取下面的开发机路径，换机器时用 ZIG=/path/to/zig 覆盖。
set -euo pipefail
MOD_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
ZIG=${ZIG:-/d/Installed_Program/zig/zig-x86_64-windows-0.17.1}
export ZIG_LIB_DIR="$ZIG/lib"
CC="$ZIG/zig.exe"

# 工具链缺失时给出一句能照着做的提示，而不是让 zig.exe 的 ENOENT 冒上来。
if [[ ! -x "$CC" ]]; then
    printf '找不到 zig 工具链：%s\n' "$CC" >&2
    printf '请设置 ZIG 指向 zig 安装目录（内含 zig.exe 与 lib/），例如：\n' >&2
    printf '  ZIG=/c/zig/zig-x86_64-windows-0.17.1 %s all\n' "$0" >&2
    exit 2
fi

FLAGS=(-target x86_64-windows-gnu -O2 -Wall)
LIBS=(-lole32 -loleaut32 -luuid)
TARGET=${1:-all}
if (( $# > 1 )); then
    printf '用法：%s [probe|sink|host|tools|all|test]\n' "$0" >&2
    exit 2
fi
case "$TARGET" in
    probe|sink|host|tools|all|test) ;;
    *) printf '用法：%s [probe|sink|host|tools|all|test]\n' "$0" >&2; exit 2 ;;
esac

OUT_DIR=${OUT_DIR-$MOD_DIR}
if [[ "$OUT_DIR" != /* && ! "$OUT_DIR" =~ ^[[:alpha:]]:[/\\] && "$OUT_DIR" != \\\\* ]]; then
    printf 'OUT_DIR 必须是绝对目录：%s\n' "$OUT_DIR" >&2
    exit 2
fi
OUT_DIR="$(cygpath -au -- "$OUT_DIR")"
if [[ ! -d "$OUT_DIR" ]]; then
    PARENT_DIR="$(dirname -- "$OUT_DIR")"
    if [[ ! -d "$PARENT_DIR" ]]; then
        printf '构建目录的父目录不存在：%s\n' "$PARENT_DIR" >&2
        exit 2
    fi
    mkdir -- "$OUT_DIR"
fi

build_probe() {
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/host.exe" "$MOD_DIR/src/host.c" "${LIBS[@]}"
    "$CC" cc "${FLAGS[@]}" -shared -o "$OUT_DIR/hooklog.dll" "$MOD_DIR/src/hooklog.c" "${LIBS[@]}"
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/inject.exe" "$MOD_DIR/src/inject.c"
    printf '已构建到 %s：host.exe hooklog.dll inject.exe\n' "$OUT_DIR"
}

build_sink() {
    "$CC" cc "${FLAGS[@]}" -shared -o "$OUT_DIR/nexsink.dll" "$MOD_DIR/src/nexsink.c" "${LIBS[@]}"
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/player.exe" "$MOD_DIR/src/player.c" "${LIBS[@]}"
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/inject2.exe" "$MOD_DIR/src/inject2.c" -ladvapi32
    printf '已构建到 %s：nexsink.dll player.exe inject2.exe\n' "$OUT_DIR"
}

build_host() {
    # 一键宿主：内置 HTTP 配置服务 + CDP 注入 + 进程看护
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/NexExclusive.exe" "$MOD_DIR/tools/nexhost.c" -lws2_32 -lole32 -loleaut32
    printf '已构建到 %s：NexExclusive.exe\n' "$OUT_DIR"
}

build_tools() {
    # 诊断与验证工具：端点枚举、默认端点切换、会话音量 vs 端点音量、
    # 独占格式矩阵（共享 vs 独占对照）、端点自己声明的格式白名单
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/list_eps.exe"   "$MOD_DIR/tools/list_eps.c"   "${LIBS[@]}"
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/set_ep.exe"     "$MOD_DIR/tools/set_ep.c"     "${LIBS[@]}" -lshell32
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/sess_vol.exe"   "$MOD_DIR/tools/sess_vol.c"   "${LIBS[@]}" -lshell32
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/probe_excl.exe" "$MOD_DIR/tools/probe_excl.c" "${LIBS[@]}"
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/ep_formats.exe" "$MOD_DIR/tools/ep_formats.c" "${LIBS[@]}" -lpropsys
    printf '已构建到 %s：list_eps.exe set_ep.exe sess_vol.exe probe_excl.exe ep_formats.exe\n' "$OUT_DIR"
}

build_test() {
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/regression.exe" "$MOD_DIR/test/regression.c" "${LIBS[@]}"
    "$OUT_DIR/regression.exe"
}

case "$TARGET" in
    probe) build_probe ;;
    sink)  build_sink ;;
    host)  build_host ;;
    tools) build_tools ;;
    all)   build_probe; build_sink; build_host; build_tools ;;
    test)  build_test ;;
esac
