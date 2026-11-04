#!/bin/bash
#
# gen_fonts.sh —— 从 TTF 生成 LVGL 点阵中文字库
#
# 背景：
#   GEC6818 的 rootfs 里没有 libfreetype，LVGL 的 FreeType 运行时字体方案用不了，
#   只能在编译期把用到的字烤成点阵。lv_font_conv 是 LVGL 官方的转换器（Node 实现）。
#
# 本机为什么这么绕：
#   WSL 里没有 node，npx 走的是 Windows 版 node.exe（/mnt/c/...）。
#   Windows 进程读不了 \\wsl.localhost 路径（CMD 不支持 UNC 当前目录），
#   所以脚本把 TTF 和字符集先拷到 Windows 临时目录，转完再拷回来。
#
# 用法：
#   ./advert/tools/gen_fonts.sh
#   FONT_TTF=/path/to/other.ttf ./advert/tools/gen_fonts.sh

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
OUT_DIR="$PROJ/advert/fonts"

# Windows 侧临时目录（Windows 版 node 只能读写 Windows 路径）
# 这台机器上 Windows 用户是 a1901；换机器时用 FONT_TMP 覆盖即可
WT="${FONT_TMP:-/mnt/c/Users/a1901/AppData/Local/Temp/lvfont}"
WT_WIN="$(wslpath -w "$WT" | tr '\\' '/')"

FONT_TTF="${FONT_TTF:-$WT/DroidSansFallback.ttf}"
[ -f "$FONT_TTF" ] || { echo "缺少字体文件：$FONT_TTF"; exit 1; }

mkdir -p "$OUT_DIR" "$WT"
cp -f "$FONT_TTF" "$WT/font.ttf"

echo "[1/3] 提取界面用到的汉字 …"
python3 - "$PROJ/advert/advert_ui.c" "$WT/ui_chars.txt" <<'PY'
import sys

src, dst = sys.argv[1], sys.argv[2]
text = open(src, encoding="utf-8").read()

chars = set()
for ch in text:
    if 0x4E00 <= ord(ch) <= 0x9FFF or ch in "—·×":
        chars.add(ch)

# 界面上可能出现的固定标点/符号
chars.update("—·×“”‘’《》、，。！？：；（）")

out = "".join(sorted(chars))
open(dst, "w", encoding="utf-8").write(out)
print("      共 %d 个汉字" % len(chars))
PY

echo "[2/3] 生成 GB2312 全集字符表 …"
python3 - "$WT/gb2312.txt" <<'PY'
import sys

# 0xA1~0xA9 是 GB2312 的符号区（·、—、《》、℃ 等），0xB0 之后才是汉字。
# 符号区必须一起带上：广告标题里写「臻蓝系列 · 机械腕表」，那个间隔号就属于符号区，
# 少了它就显示成一个空方块。
cs = []
for hi in range(0xA1, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            cs.append(bytes([hi, lo]).decode("gb2312"))
        except UnicodeDecodeError:
            pass

# 坑：GB2312 里的「间隔号」是 U+30FB(・)，而输入法打出来的是 U+00B7(·)。
# 两个码点长得像，但字形表里是两条记录，不补就会显示成空方块。
# 破折号同理（GB2312 是 U+2015 ―，日常用的是 U+2014 —）。
cs.append("·—–")

open(sys.argv[1], "w", encoding="utf-8").write("".join(cs))
print("      %d 个字符（含符号区）" % len(cs))
PY

conv() {   # conv <字号> <字符来源文件|GB2312> <变量名>
    local size="$1" source="$2" name="$3"
    local win_font="$WT_WIN/font.ttf"
    local win_out="$WT_WIN/$name.c"

    echo "      生成 $name (${size}px) …"
    (
        cd "$WT"
        npx --yes --registry=https://registry.npmmirror.com lv_font_conv \
            --size "$size" --bpp 4 --format lvgl \
            --font "$win_font" \
            --range 0x20-0x7E \
            --symbols "$(cat "$source")" \
            --lv-font-name "$name" \
            -o "$win_out"
    )
    cp -f "$WT/$name.c" "$OUT_DIR/$name.c"
    printf "      %-12s %s\n" "$name.c" "$(du -h "$OUT_DIR/$name.c" | cut -f1)"
}

echo "[3/3] 转换 …"
# 20px / 16px / 14px 都会显示"客户输入的文字"（标题、备注），必须覆盖 GB2312 全集：
#   · 20px 播放页大标题
#   · 16px 广告库标题、滚动公告
#   · 14px 广告库备注、信息页说明
# 只有 24px 是纯固定文案（品牌字标、页面大标题），可以按实际用字裁剪。
conv 20 "$WT/gb2312.txt"     ad_font_20
conv 16 "$WT/gb2312.txt"     ad_font_16
conv 14 "$WT/gb2312.txt"     ad_font_14
conv 24 "$WT/ui_chars.txt"   ad_font_24

echo "完成 → $OUT_DIR"
