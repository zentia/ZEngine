"""把沉浸式宣发页打包成可直接推到 username.github.io 仓库根目录的静态站点。

用法：python integration/chessboard/tools/build_pages.py
产物：integration/chessboard/pages/（每次运行先清空重建）

设计取舍：
- 保留 chessboard 原有目录层级（web/ + ../build/ + ../assets/），这样 starry.js
  里的 BUILD / fetchJSON 路径一个都不用改，本地 tools/serve.py 与线上完全同构。
- GitHub Pages 对 .mjs 的 Content-Type 并不稳定，而 ES module 有严格 MIME 检查；
  构建时把所有本地 .mjs 复制成 .js 并重写 import 后缀，彻底规避。
- .nojekyll 阻止 Jekyll 处理静态文件；根 index.html 把 / 转到指定画境。
- 只带 starry 页真正 fetch 的文件，不发布验证页 / Unity / Blender / 探针产物。
"""
import io
import re
import shutil
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / "pages"

if OUT.exists():
    shutil.rmtree(OUT)
OUT.mkdir()

written = []


def write(rel, data, binary=False):
    target = OUT / rel
    target.parent.mkdir(parents=True, exist_ok=True)
    if binary:
        target.write_bytes(data)
    else:
        io.open(target, "w", encoding="utf-8", newline="").write(data)
    written.append((rel, target.stat().st_size))


def text(rel):
    return io.open(ROOT / rel, encoding="utf-8", newline="").read()


def binary(rel):
    return (ROOT / rel).read_bytes()


def copy(rel, target=None):
    write(target or rel, binary(rel), binary=True)


# --- 契约与几何：starry.js 里 "../" 与 BUILD="../build/" 指向这些文件 ---------
for rel in ["visual-pack.json", "board-spec.json", "ceremony.json"]:
    copy(rel)
for rel in ["build/board.zbrd", "build/board.manifest.json",
            "build/external_mesh.manifest.json"]:
    copy(rel)

# --- 贴图与概念图 -----------------------------------------------------------
for rel in ["assets/sky_panorama.jpg", "assets/concept.jpg"]:
    copy(rel)
for item in sorted((ROOT / "assets/worlds").glob("*")):
    if item.is_file() and item.suffix in {".png", ".json"}:
        copy("assets/worlds/" + item.name)

# --- web/：.mjs -> .js 并重写静态 import -------------------------------------
LOCAL_MODULE = re.compile(r'(["\'])\./([\w-]+)\.mjs\1')
MODULES = ["starry.js", "zbrd.mjs", "external_mesh.mjs",
           "world-pack.mjs", "world-sky.mjs", "world-scenery.mjs",
           "board-ceremony.mjs"]
for name in MODULES:
    source = text("web/" + name)
    rewritten, count = LOCAL_MODULE.subn(r'\1./\2.js\1', source)
    assert not LOCAL_MODULE.search(rewritten), name + " 仍残留 .mjs import"
    write("web/" + name.replace(".mjs", ".js"), rewritten)
    if name == "starry.js":
        assert count == 6, "starry.js 应有 6 个本地 .mjs import，实际 %d" % count

worlds = text("web/worlds.json").replace("world-scenery.mjs", "world-scenery.js")
write("web/worlds.json", worlds)
copy("web/starry.css")

html = text("web/starry.html")
old_link = '<a href="./index.html">返回数据验证页</a>'
assert old_link in html, "starry.html 的验证页链接变了，请同步构建脚本"
html = html.replace(old_link, '<a href="?world=starry-night">星月夜 · 默认画境</a>')
write("web/starry.html", html)

# --- 根入口 -----------------------------------------------------------------
write("index.html", """<!doctype html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ZEngine · 十二画境</title>
<script>
  // 默认落在「马尔利港的洪水」；/?world=<id> 与 /?event=<id> 可透传。
  const params = new URLSearchParams(location.search);
  const requested = params.get("world");
  const event = params.get("event");
  location.replace("web/starry.html?world="
    + encodeURIComponent(requested || "port-marly-flood")
    + (event ? "&event=" + encodeURIComponent(event) : ""));
</script>
</head>
<body style="font:14px system-ui;background:#0b1622;color:#dbe7f2;padding:24px">
<a style="color:#d8b476" href="web/starry.html?world=port-marly-flood">进入沉浸式画境 →</a>
</body>
</html>
""")
write(".nojekyll", "")

# --- 自检 -------------------------------------------------------------------
required = [
    "index.html", ".nojekyll", "web/starry.html", "web/starry.js",
    "web/zbrd.js", "web/external_mesh.js", "web/world-pack.js",
    "web/world-sky.js", "web/world-scenery.js", "web/board-ceremony.js",
    "web/worlds.json", "web/starry.css", "visual-pack.json", "board-spec.json",
    "ceremony.json",
    "build/board.zbrd", "build/board.manifest.json",
    "build/external_mesh.manifest.json", "assets/sky_panorama.jpg",
    "assets/concept.jpg", "assets/worlds/manifest.json",
]
for rel in required:
    assert (OUT / rel).is_file(), "missing " + rel
assert not list(OUT.rglob("*.mjs")), "Pages 站点不应含 .mjs"
concepts = list((OUT / "assets/worlds").glob("*.png"))
assert len(concepts) == 11, len(concepts)

total = sum(size for _, size in written)
print("pages files:", len(written), "bytes:", total,
      "(%.1f MB)" % (total / 1048576))
print("entry: / -> web/starry.html?world=port-marly-flood")
print("all local modules are .js; .nojekyll present; 11 concept PNGs")
