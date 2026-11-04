import re

def all_codepoints(path):
    s = open(path, encoding="utf-8").read()
    cps = set()
    for m in re.finditer(r"static const uint16_t (unicode_list_\d+)\[\] = \{(.*?)\};", s, re.S):
        body = m.group(2)
        for n in re.findall(r"0x([0-9a-fA-F]+)", body):
            cps.add(int(n, 16))
    return cps

need = "需要访问许可身份资料确认接收求开始读取完毕核验通过等待用户授权已授予权限建立会话通道索引工作区与深求索"

for name, path in [("chinese_ziku", "test/chinese_ziku.c"),
                   ("han_bold_20", "test/lv_font_source_han_sans_bold_20.c")]:
    cps = all_codepoints(path)
    have = [c for c in need if ord(c) in cps]
    miss = [c for c in need if ord(c) not in cps]
    print(name, "sparse codepoints:", len(cps), "covered:", len(have), "/", len(need))
    print("  have:", "".join(have))
    print("  miss:", "".join(miss))
    print()
