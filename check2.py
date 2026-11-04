import re

# 从 boot_screen.c 提取所有中文字符串字面量
s = open("/home/user/code/lv_port_pc_vscode-release-v8/advert/boot_screen.c", encoding="utf-8").read()
# 找所有中文字符
all_zh = set(re.findall(r"[\u4e00-\u9fff]", s))
# 还有 · 和 …
all_sym = set(re.findall(r"[·…，]", s))
print("boot_screen.c 里用到的所有汉字（去重）:")
print("".join(sorted(all_zh)))
print("共", len(all_zh), "个")
print("符号:", "".join(sorted(all_sym)))

# 从 boot_zh_font.c 提取已生成的字
f = open("/home/user/code/lv_port_pc_vscode-release-v8/advert/boot_zh_font.c", encoding="utf-8").read()
# 找 unicode_list 或 glyph 注释里的 U+XXXX
font_cps = set()
for m in re.finditer(r"U\+([0-9A-Fa-f]{4})", f):
    font_cps.add(int(m.group(1), 16))

missing = [c for c in all_zh if ord(c) not in font_cps]
print("\n字库缺失的汉字:", "".join(missing) if missing else "无")
for c in all_sym:
    print(f"符号 {c!r} U+{ord(c):04X}:", "有" if ord(c) in font_cps else "缺")
