#!/bin/bash
#
# make_initial_ads.sh —— 把三组「图片 + 视频」打成板上能直接播的初始广告
#
# 产出 advert/data/ ，就是开发板上 ADVERT_ROOT 的完整布局：
#
#   advert/data/
#   ├── meta/
#   │   ├── 01_watch.conf        一行：V|标题|副标题|帧率|停留秒
#   │   ├── 02_girl.conf
#   │   └── 03_lion.conf
#   └── media/
#       ├── 01_watch/{cover.jpg,thumb.jpg,f_000.jpg…}
#       ├── 02_girl/…
#       └── 03_lion/…
#
# `make deploy` 会把这个目录整个 tar 到 /root/lvgl-weather/advert；
# 也可以用 `adclient.py push-tree --dir advert/data` 增量推一遍。
#
# 素材来源（一张图 + 一段视频 = 一条广告，图当封面，视频当正片）：
#   01_watch  表.mp4              + 屏幕截图 2026-10-29 093858.png
#   02_girl   1.mp4               + 屏幕截图 2026-10-29 093957.png
#   03_lion   广告视频生成.mp4     + 微信图片_20261027112040.jpg

set -e

HERE="$(cd "$(dirname "$0")" && pwd)"
PROJ="$(cd "$HERE/../.." && pwd)"
OUT="$PROJ/advert/data"
SRC="${ADS_SRC:-/mnt/c/Users/a1901/Desktop}"
CLIENT="$PROJ/advert/tcp/adclient.py"

[ -d "$SRC" ] || { echo "素材目录不存在：$SRC"; exit 1; }

rm -rf "$OUT"
mkdir -p "$OUT/media" "$OUT/meta"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

# pack <id> <标题> <副标题> <视频> <封面>
pack() {
    local id="$1"
    "$CLIENT" pack --out "$TMP" --id "$id" --title "$2" --sub "$3" \
                   --video "$4" --cover "$5" >/dev/null

    mv "$TMP/$id" "$OUT/media/$id"
    # 包里的 meta.conf 在板上要提到 meta/<id>.conf
    mv "$OUT/media/$id/meta.conf" "$OUT/meta/$id.conf"
    echo "  $id  ->  $(cat "$OUT/meta/$id.conf")"
}

echo "生成初始广告："
pack 01_watch "臻蓝系列 · 机械腕表" "BLUE SERIES 8500" \
     "$SRC/表.mp4" "$SRC/屏幕截图 2026-10-29 093858.png"

pack 02_girl "雨中舞狮 · 少女手办" "LION DANCE IN RAIN" \
     "$SRC/1.mp4" "$SRC/屏幕截图 2026-10-29 093957.png"

pack 03_lion "狮头小娘子 · 限定款" "LION HAT CHIBI" \
     "$SRC/广告视频生成.mp4" "$SRC/微信图片_20261027112040.jpg"

echo
echo "总大小：$(du -sh "$OUT" | cut -f1)"
du -sh "$OUT"/media/* | sed 's/^/  /'
