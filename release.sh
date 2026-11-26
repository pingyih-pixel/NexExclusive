#!/usr/bin/env bash
# 打发布包：编译 → 只挑出运行时要的 8 个文件 → 生成包内清单 → 加安装说明 → 压 zip →（有 Inno Setup 时）
# 打安装程序 → 出校验和与 Release 说明。产物全在 dist/ 下，不碰构建目录，也不会把 .pdb / 测试工具塞进去。
#
#   ./release.sh              用 nexhost.c 里的 APP_VERSION
#   ./release.sh 1.0.1        临时指定版本号（不改源码）
#
# 依赖：zig（同 build.sh，用 ZIG= 覆盖）、C:\Windows\System32\tar.exe（bsdtar，
# 系统自带，-a 能直接写 zip）、sha256sum。没有 zip/7z 也不影响。
set -euo pipefail
ROOT="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
MOD_DIR="$ROOT/mod"
DIST="$ROOT/dist"
BSD_TAR=${BSD_TAR:-/c/Windows/System32/tar.exe}

if [[ ! -x "$BSD_TAR" ]]; then
    printf '找不到 bsdtar：%s\n' "$BSD_TAR" >&2
    printf '它是 Windows 自带的（System32\\tar.exe），可以用 BSD_TAR=/path/to/tar 指定。\n' >&2
    exit 2
fi

# 版本号：源码里的 APP_VERSION 是唯一真相，避免脚本和程序说的不是一回事
VER=${1-}
if [[ -z "$VER" ]]; then
    VER=$(sed -n 's/^#define APP_VERSION L"\([^"]*\)".*/\1/p' "$MOD_DIR/tools/nexhost.c" | head -1)
fi
if [[ ! "$VER" =~ ^[0-9]+\.[0-9]+\.[0-9]+$ ]]; then
    printf '版本号不合法：[%s]（期望 1.2.3 这种形式）\n' "$VER" >&2
    exit 2
fi

# 我们验过的客户端版本，写进说明里，让用户能自己对一下。只截取 kVerified[]
# 数组本身：以前取整个文件里第一个 3.x 字符串，数组一加第二项就漏掉了。
CLIENT_VERS=$(sed -n '/kVerified\[\] = {/,/};/p' "$MOD_DIR/tools/nexhost.c" |
              sed -n 's/.*L"\([0-9][0-9.]*\)".*/\1/p')
if [[ -z "$CLIENT_VERS" ]]; then
    printf '读不出 nexhost.c 里 kVerified[] 的客户端版本列表\n' >&2
    exit 2
fi
CLIENT_VER_LIST=$(printf '%s\n' "$CLIENT_VERS" | paste -sd'/' - | sed 's#/#、#g')
CLIENT_VER_BULLETS=$(printf '%s\n' "$CLIENT_VERS" | sed 's/^/  - /')

NAME="NexExclusive-$VER"
STAGE="$DIST/$NAME"
BUILD="$DIST/.build"

printf '==> 编译 %s\n' "$VER"
rm -rf -- "$STAGE" "$BUILD"
mkdir -p -- "$STAGE" "$BUILD"
OUT_DIR="$BUILD" bash "$MOD_DIR/build.sh" all

# 发布包里只要这 8 个。player.exe / inject*.exe / *_eps.exe 是开发和诊断用的，
# 放进去只会让用户困惑，也会让杀软多几个可疑样本。
FILES=(NexExclusive.exe NexAudioHost.exe nexsink.dll nexui.dll hook.cfg ui.json ui/panel.js ui/simplify.js)
printf '==> 收集文件\n'
for f in "${FILES[@]}"; do
    if [[ ! -f "$BUILD/$f" ]]; then
        # 最常见的原因就是被杀软当场删了，所以把话说清楚，别让人去查编译参数
        printf '缺文件：%s\n' "$BUILD/$f" >&2
        printf '如果刚才编译成功、文件却不见了，基本是杀软把 NexExclusive.exe 隔离了。\n' >&2
        printf '查一下：Get-MpThreatDetection | Sort-Object InitialDetectionTime -Descending | Select -First 3\n' >&2
        exit 1
    fi
    mkdir -p -- "$(dirname -- "$STAGE/$f")"
    cp -- "$BUILD/$f" "$STAGE/$f"
done

printf '==> 写包内清单\n'
( cd "$STAGE" && sha256sum "${FILES[@]}" > manifest.txt )

printf '==> 写安装说明\n'
cat > "$STAGE/README.txt" <<EOF
NexExclusive $VER —— 给网易云音乐客户端加上 WASAPI 独占输出

【怎么装】
  双击 NexExclusive.exe。
  第一次运行会问你要不要安装，选「是」：
    · 装到 %LOCALAPPDATA%\NexExclusive
    · 开机自动启动，之后开网易云就自动生效
    · 在「设置 → 应用」里可以卸载
  选「否」也能直接用，只是不会自启、也没有卸载入口。

  想在命令行里装：
    NexExclusive.exe --install
    NexExclusive.exe --install --client "D:\...\cloudmusic.exe"   （客户端装在非常规位置时）
    NexExclusive.exe --verify
    NexExclusive.exe --uninstall
    NexExclusive.exe --help

【装完在哪】
  右下角托盘有个图标，右键可以：打开客户端 / 看日志 / 打开程序目录 / 退出。
  日志在同目录的 host.log。

【要求】
  · Windows 10/11 64 位
  · 网易云音乐客户端 $CLIENT_VER_LIST（只验证过这几个版本；其他版本能跑，
    但客户端大版本更新有可能改动内部结构，日志里会标「未经验证」）
  · 独占输出要求声卡/耳机支持你要的采样率。客户端设置里选 44.1k / 48k 最稳，
    88.2k 及以上要看设备白名单——不是所有标称 96k 的设备都收 88.2k。

【杀软报毒】
  这个程序会往网易云进程里加载自己的 DLL，行为特征和注入类木马相似，
  Windows Defender 有可能报 Behavior:Win32/DefenseEvasion.A!ml 并直接隔离。
  这是误报。要用的话，把这三个文件加进排除项（需要管理员权限）：

    Add-MpPreference -ExclusionPath "\$env:LOCALAPPDATA\NexExclusive\NexExclusive.exe"
    Add-MpPreference -ExclusionPath "\$env:LOCALAPPDATA\NexExclusive\nexsink.dll"
    Add-MpPreference -ExclusionPath "\$env:LOCALAPPDATA\NexExclusive\nexui.dll"

  从解压目录直接运行的话，把那个目录下的同名三个文件也加上。
  不要排除整个目录——目录对当前用户可写，整体排除等于让放进去的任何文件都不再被扫描。

  不想加排除项就别用——别为了这个关掉实时保护。

【卸载】
  设置 → 应用 → NexExclusive → 卸载；或者运行 NexExclusive.exe --uninstall。
  卸载会删掉程序文件、开机自启项和卸载项，不动你的音乐和客户端。

  注意：从安装目录里运行卸载时，主程序文件正被自己占用，Windows 不允许
  删除正在运行的 exe。如果当前不是管理员（无法登记「重启后删除」），目录里
  会剩下一个 NexExclusive.exe——关掉程序后手动删掉那个文件夹即可。
EOF

printf '==> 打包\n'
ZIP="$DIST/$NAME.zip"
rm -f -- "$ZIP"
# -a 让 bsdtar 按扩展名选 zip 格式；-C 到 dist 保证压缩包里带一层目录名，
# 用户解压出来是一个文件夹而不是散落一地。
( cd "$DIST" && "$BSD_TAR" -a -c -f "$ZIP" "$NAME" )

# 安装程序：Inno Setup 把上面同一个便携包目录（含 manifest.txt）打进 Setup.exe，
# 安装时调用包内 NexExclusive.exe --install，安装逻辑仍只有一份。
# 本机没装 Inno Setup 时跳过；CI 设 REQUIRE_INSTALLER=1，缺了就失败。
SETUP_NAME="$NAME-setup.exe"
SETUP="$DIST/$SETUP_NAME"
rm -f -- "$SETUP"
ISCC=${ISCC:-}
if [[ -z "$ISCC" ]]; then
    for c in "$(command -v iscc 2>/dev/null || true)" \
             "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
             "/c/Program Files/Inno Setup 6/ISCC.exe"; do
        if [[ -n "$c" && -x "$c" ]]; then ISCC=$c; break; fi
    done
fi
if [[ -n "$ISCC" ]]; then
    printf '==> 打安装程序（%s）\n' "$ISCC"
    # /D 开头的参数会被 MSYS 当成路径改写，关掉转换并传 Windows 路径
    MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' "$ISCC" /Qp \
        "/DAppVersion=$VER" \
        "/DPackageDir=$(cygpath -w "$STAGE")" \
        "/DOutputDir=$(cygpath -w "$DIST")" \
        "$(cygpath -w "$ROOT/installer/NexExclusive.iss")"
    if [[ ! -f "$SETUP" ]]; then
        printf '安装程序没有生成：%s\n' "$SETUP" >&2
        exit 1
    fi
elif [[ "${REQUIRE_INSTALLER:-0}" == 1 ]]; then
    printf '找不到 Inno Setup 的 ISCC.exe，而 REQUIRE_INSTALLER=1\n' >&2
    exit 1
else
    printf '==> 跳过安装程序：没找到 Inno Setup 6（ISCC.exe），可用 ISCC=/path/to/ISCC.exe 指定\n'
fi

printf '==> 校验和\n'
ARTIFACTS=("$NAME.zip")
[[ -f "$SETUP" ]] && ARTIFACTS+=("$SETUP_NAME")
( cd "$DIST" && sha256sum "${ARTIFACTS[@]}" > SHA256SUMS.txt && cat SHA256SUMS.txt )

printf '==> 写 Release 说明\n'
NOTES="$DIST/RELEASE-NOTES.md"
SIZE=$(du -k "$ZIP" | cut -f1)
# sha256sum 在 MSYS 下可能输出「哈希 *文件名」（二进制模式标记），两种写法都认
sum_of() { awk -v f="$1" '{n=$2; sub(/^\*/, "", n); if (n == f) print $1}' "$DIST/SHA256SUMS.txt"; }
SHA=$(sum_of "$NAME.zip")
if [[ -f "$SETUP" ]]; then
    SETUP_SIZE=$(du -k "$SETUP" | cut -f1)
    SETUP_SHA=$(sum_of "$SETUP_NAME")
    INSTALL_SECTION="两种下载，二选一：

- **安装版** \`$SETUP_NAME\`：双击安装到 \`%LOCALAPPDATA%\\NexExclusive\`，登记开机自启、
  开始菜单和「设置 → 应用」里的卸载项。不需要管理员权限。
- **便携版** \`$NAME.zip\`：解压后双击 \`NexExclusive.exe\` 直接用。第一次会问要不要安装，
  选「否」就保持便携（不自启、不写卸载项）；选「是」和安装版效果相同。"
    FILE_LINES="$SETUP_NAME    $SETUP_SIZE KB
sha256       $SETUP_SHA

$NAME.zip    $SIZE KB
sha256       $SHA"
else
    INSTALL_SECTION="下载 \`$NAME.zip\`，解压，双击 \`NexExclusive.exe\`。第一次会问要不要安装，选「是」即可。"
    FILE_LINES="$NAME.zip    $SIZE KB
sha256       $SHA"
fi
cat > "$NOTES" <<EOF
# NexExclusive $VER

给网易云音乐客户端加上 WASAPI 独占输出——让音频绕开系统混音器，直接交给声卡。

## 安装

$INSTALL_SECTION

装完驻留托盘，之后开网易云自动生效。两种方式都没有代码签名，SmartScreen 可能提示
「Windows 已保护你的电脑」，点「更多信息 → 仍要运行」。

命令行方式：\`NexExclusive.exe --install\` / \`--uninstall\` / \`--help\`。

## 这个版本做了什么

- 一个 exe 干完所有事：启动客户端、注入、起配置服务、注入界面脚本、看护重连
- 托盘常驻 + 登录自启，客户端重启或页面刷新会自动补回去
- 首次双击引导安装，装完直接拉起，不用再找快捷方式
- 单实例：自启的那份和手动双击的那份不会打架
- 客户端路径七级探测（环境变量 → host.cfg → 运行中的进程 → 注册表 →
  桌面/开始菜单快捷方式 → 常见位置 → 文件选择框），装在非常规目录也能找到
- 客户端版本检查：只对 $CLIENT_VER_LIST 做过验证，其他版本会在日志里标出来

## 已知问题

**Windows Defender 会误报。** 程序要往网易云进程里加载 DLL，行为特征和注入类
木马相似，Defender 的机器学习模型会判成 \`Behavior:Win32/DefenseEvasion.A!ml\`
并直接隔离 exe（连带删掉快捷方式和自启项）。这是误报，但确实会挡住安装。

要用的话需要给这三个文件加排除项（管理员权限）：

\`\`\`powershell
Add-MpPreference -ExclusionPath "\$env:LOCALAPPDATA\NexExclusive\NexExclusive.exe"
Add-MpPreference -ExclusionPath "\$env:LOCALAPPDATA\NexExclusive\nexsink.dll"
Add-MpPreference -ExclusionPath "\$env:LOCALAPPDATA\NexExclusive\nexui.dll"
\`\`\`

不要排除整个目录——目录对当前用户可写，整体排除会让放进去的任何文件都不再被扫描。
不想加排除项就别用，不要为了这个关掉实时保护。

## 文件

\`\`\`
$FILE_LINES
\`\`\`

zip 包内（安装版装的也是这些）：\`NexExclusive.exe\`、\`NexAudioHost.exe\`、\`nexsink.dll\`、\`nexui.dll\`、\`hook.cfg\`、\`ui.json\`、\`ui/\`、\`manifest.txt\`、\`README.txt\`。

## 验证环境

- Windows 11 家庭版 build 26200（25H2）x64
- 网易云音乐客户端：
$CLIENT_VER_BULLETS
- 已实测端点：HyperX Cloud III（44.1k/48k/96k 独占）、AudioQuest DragonFly MM3A（额外支持 88.2k）
EOF

rm -rf -- "$BUILD"
printf '\n完成：\n'
printf '  %s\n' "$ZIP"
if [[ -f "$SETUP" ]]; then printf '  %s\n' "$SETUP"; fi
printf '  %s\n' "$DIST/SHA256SUMS.txt"
printf '  %s\n' "$NOTES"
