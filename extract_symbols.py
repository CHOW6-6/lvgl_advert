import re

s = open("/home/user/code/lv_port_pc_vscode-release-v8/advert/boot_screen.c", encoding="utf-8").read()

# 只提取字符串字面量里的中文字符和符号
chars = set()
for m in re.finditer(r'"([^"]*)"', s):
    for c in m.group(1):
        if '\u4e00' <= c <= '\u9fff' or c in '·…，':
            chars.add(c)

zh = sorted([c for c in chars if '\u4e00' <= c <= '\u9fff'])
sym = sorted([c for c in chars if c in '·…，'])
print("文案实际用到的汉字（去重 %d 个）:" % len(zh))
print("".join(zh))
print("\n符号:", "".join(sym))

# 生成 lv_font_conv 的 --symbols 参数
symbols = "".join(zh) + "".join(sym)
open("/home/user/code/lv_port_pc_vscode-release-v8/boot_symbols.txt", "w").write(symbols)
print("\n--symbols 参数已写入 boot_symbols.txt")
