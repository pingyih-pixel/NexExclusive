#!/usr/bin/env bash
# 用 Zig 构建；OUT_DIR 必须是绝对目录，默认本脚本所在的 mod。
#
# Zig 的位置按这个顺序找：
#   1. 环境变量 ZIG（指向 zig 安装目录，或直接指向 zig 可执行文件）
#   2. PATH 里的 zig
#   3. mod/zig.path —— 本机路径写在这里（已 gitignore，不会进仓库）
# 这样仓库里不出现任何开发机的绝对路径，本机也不用每次都传 ZIG=。
set -euo pipefail
MOD_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"

# 本机覆盖文件：内容形如 ZIG=/d/path/to/zig-x86_64-windows-0.17.1
if [[ -f "$MOD_DIR/zig.path" ]]; then
    # shellcheck disable=SC1091
    . "$MOD_DIR/zig.path"
fi

ZIG=${ZIG:-}
if [[ -z "$ZIG" ]] && command -v zig >/dev/null 2>&1; then
    ZIG=$(command -v zig)
fi
if [[ -z "$ZIG" ]]; then
    printf '找不到 zig。三选一：\n' >&2
    printf '  · 把 zig 放进 PATH\n' >&2
    printf '  · 传环境变量：ZIG=/c/zig/zig-x86_64-windows-0.17.1 %s all\n' "$0" >&2
    printf '  · 写进 %s（内容：ZIG=/c/zig/zig-x86_64-windows-0.17.1）\n' "$MOD_DIR/zig.path" >&2
    exit 2
fi

# ZIG 可能指目录（内含 zig.exe + lib/），也可能直接指可执行文件
if [[ -d "$ZIG" ]]; then
    CC="$ZIG/zig.exe"
    export ZIG_LIB_DIR="$ZIG/lib"
else
    CC="$ZIG"
fi

if [[ ! -x "$CC" ]]; then
    printf 'zig 不可执行：%s\n' "$CC" >&2
    printf '请确认 ZIG 指向 zig 安装目录（内含 zig.exe 与 lib/）或 zig 可执行文件本身。\n' >&2
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
    # 一键宿主：内置 HTTP 配置服务 + CDP 注入 + 进程看护 + 托盘常驻。
    # 它按 exe 所在目录找资源，所以 OUT_DIR 就是发布包本身（扁平结构）。
    #   --subsystem,windows: 不弹控制台窗口，日常运行靠托盘
    #   advapi32: 客户端路径探测要读注册表、安装要写卸载项
    #   shell32/user32: 托盘图标与菜单
    #   version: 读客户端 exe 的版本资源，判断是否为已验证版本
    #   comdlg32: 找不到客户端时弹文件选择框

    # 版本资源 + 清单。版本号从 nexhost.c 的 APP_VERSION 取，只在这里定义一次。
    local ver v1 v2 v3 rc res
    ver=$(sed -n 's/^#define APP_VERSION L"\([^"]*\)".*/\1/p' "$MOD_DIR/tools/nexhost.c" | head -1)
    if [[ ! "$ver" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
        printf '读不出 APP_VERSION（得到 [%s]）\n' "$ver" >&2
        exit 2
    fi
    IFS=. read -r v1 v2 v3 <<< "$ver"
    rc="$OUT_DIR/.nexhost.rc"
    res="$OUT_DIR/.nexhost.res"
    # manifest 必须和 .rc 同目录、且文件名与 .rc 里写的一致。
    # 版本占位符在 manifest 里也有（assemblyIdentity 的 version），两边都要替换——
    # 只换 .rc 会得到一个 version="@V1@.@V2@.@V3@.0" 的清单，Windows 直接拒绝加载，
    # 报「应用程序的并行配置不正确」，事件日志里才看得到真实原因。
    cp -- "$MOD_DIR/tools/nexhost.manifest" "$OUT_DIR/nexhost.manifest"
    sed -i -e "s/@V1@/$v1/g" -e "s/@V2@/$v2/g" -e "s/@V3@/$v3/g" "$OUT_DIR/nexhost.manifest"
    sed -e "s/@V1@/$v1/g" -e "s/@V2@/$v2/g" -e "s/@V3@/$v3/g" \
        "$MOD_DIR/tools/nexhost.rc.in" > "$rc"
    "$CC" rc "$rc" "$res"

    "$CC" cc "${FLAGS[@]}" -Wl,--subsystem,windows \
        -o "$OUT_DIR/NexExclusive.exe" "$MOD_DIR/tools/nexhost.c" "$res" \
        -lws2_32 -lole32 -loleaut32 -ladvapi32 -lshell32 -luser32 -lgdi32 \
        -lversion -lcomdlg32
    rm -f -- "$rc" "$res" "$OUT_DIR/nexhost.manifest"
    # 配置只在缺失时复制——用户可能改过；界面脚本是代码，每次都覆盖。
    local f
    for f in hook.cfg ui.json; do
        [[ -f "$OUT_DIR/$f" ]] || cp -- "$MOD_DIR/$f" "$OUT_DIR/$f"
    done
    if [[ -d "$MOD_DIR/ui" ]]; then
        mkdir -p "$OUT_DIR/ui"
        cp -- "$MOD_DIR/ui/panel.js" "$MOD_DIR/ui/simplify.js" "$OUT_DIR/ui/"
    fi
    printf '已构建到 %s：NexExclusive.exe（含 hook.cfg / ui.json / ui/）\n' "$OUT_DIR"
}

build_tools() {
    # 诊断与验证工具：端点枚举、默认端点切换、会话音量 vs 端点音量、
    # 独占格式矩阵（共享 vs 独占对照）、端点自己声明的格式白名单
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/list_eps.exe"   "$MOD_DIR/tools/list_eps.c"   "${LIBS[@]}"
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/set_ep.exe"     "$MOD_DIR/tools/set_ep.c"     "${LIBS[@]}" -lshell32
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/sess_vol.exe"   "$MOD_DIR/tools/sess_vol.c"   "${LIBS[@]}" -lshell32
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/probe_excl.exe" "$MOD_DIR/tools/probe_excl.c" "${LIBS[@]}"
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/ep_formats.exe" "$MOD_DIR/tools/ep_formats.c" "${LIBS[@]}" -lpropsys
    # 开发辅助：带 DevTools 端口启动客户端（参数或 NEX_CLIENT 给路径）。
    # 之前一直没进构建脚本，改完没人编译就烂在那儿了。
    "$CC" cc "${FLAGS[@]}" -o "$OUT_DIR/nexlaunch.exe" "$MOD_DIR/tools/launch.c"
    printf '已构建到 %s：list_eps.exe set_ep.exe sess_vol.exe probe_excl.exe ep_formats.exe nexlaunch.exe\n' "$OUT_DIR"
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
