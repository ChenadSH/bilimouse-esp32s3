#!/usr/bin/env python3
"""BiliMouse 一键发版脚本（GitHub Releases OTA 方案）

用法:
    python tools/release.py <版本号> [更新说明]

示例:
    set GITHUB_TOKEN=ghp_xxx
    python tools/release.py 2.4.0 "修复点击拖拽、新增定时休眠"

流程:
    1. 改 CMakeLists.txt 的 PROJECT_VER
    2. git commit
    3. tools\\build.bat 编译
    4. 创建 GitHub release v<版本>
    5. 上传附件 bilimouse.bin + manifest.json（url 用 latest/download 永久链接）

令牌来源: 环境变量 GITHUB_TOKEN，其次交互输入（仅仓库权限的 PAT 即可）。
依赖: Python 3.8+ 标准库 + git + tools/build.bat（需已装 ESP-IDF）。
"""
import json
import os
import re
import subprocess
import sys
import urllib.request
from pathlib import Path

REPO = "ChenadSH/bilimouse-esp32s3"
OWNER = "ChenadSH"
ROOT = Path(__file__).resolve().parent.parent
BIN = ROOT / "build" / "bilimouse.bin"
CMAKELISTS = ROOT / "CMakeLists.txt"
LATEST = f"https://github.com/{REPO}/releases/latest/download"
RAW_MASTER = f"https://raw.githubusercontent.com/{REPO}/master"
OTA_DIR = ROOT / "ota"


def run(cmd, **kw):
    print("+", " ".join(str(c) for c in cmd))
    r = subprocess.run(cmd, cwd=ROOT, **kw)
    if r.returncode != 0:
        sys.exit(f"error: command failed ({r.returncode})")
    return r


def api(method, url, token, data=None, raw=None, ctype="application/json"):
    body = raw if raw is not None else (json.dumps(data).encode() if data else None)
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Authorization", f"token {token}")
    req.add_header("Accept", "application/vnd.github+json")
    if body:
        req.add_header("Content-Type", ctype)
    try:
        with urllib.request.urlopen(req) as resp:
            payload = resp.read()
            return json.loads(payload) if payload else {}
    except urllib.error.HTTPError as e:
        sys.exit(f"error: {method} {url} -> {e.code}\n{e.read().decode(errors='replace')}")


def main():
    if len(sys.argv) < 2:
        sys.exit(__doc__)
    ver = sys.argv[1]
    if not re.fullmatch(r"\d+\.\d+\.\d+", ver):
        sys.exit("error: 版本号必须是三段数字，如 2.4.0")
    notes = sys.argv[2] if len(sys.argv) > 2 else f"BiliMouse v{ver}"

    token = os.environ.get("GITHUB_TOKEN") or input("GitHub PAT: ").strip()
    if not token:
        sys.exit("error: 缺少 GITHUB_TOKEN")

    # 1. 版本号写进 CMakeLists.txt（已是目标版本则跳过，不算错误）
    text = CMAKELISTS.read_text(encoding="utf-8")
    new_text = re.sub(r'(set\(PROJECT_VER\s*")[^"]+(")', rf"\g<1>{ver}\g<2>", text)
    if new_text == text and f'set(PROJECT_VER "{ver}")' not in text:
        sys.exit("error: CMakeLists.txt 里找不到 PROJECT_VER")
    CMAKELISTS.write_text(new_text, encoding="utf-8")
    print(f"PROJECT_VER -> {ver}")

    # 2. 提交并推送（tag 要打在远端最新提交上，必须先 push）
    subprocess.run(["git", "add", "CMakeLists.txt"], cwd=ROOT)
    subprocess.run(["git", "commit", "-m", f"release: v{ver}"], cwd=ROOT)
    run(["git", "push", "origin", "master"])

    # 3. 编译：解析 tools/env.bat 构造环境后直接跑 idf.py。
    #    不经过 cmd 一行链（%IDF_PATH% 会在 env.bat 生效前被展开），
    #    也不用 build.bat（部分终端下成功也返回 1）。
    env = os.environ.copy()
    for m in re.finditer(r'^set\s+"?(\w+)=(.*?)"?\s*$',
                         (ROOT / "tools" / "env.bat").read_text(encoding="utf-8"),
                         re.M):
        env[m.group(1)] = m.group(2).replace("%PATH%", env.get("PATH", ""))
    py = Path(env["IDF_PYTHON_ENV_PATH"]) / "Scripts" / "python.exe"
    idf_py = Path(env["IDF_PATH"]) / "tools" / "idf.py"
    r = subprocess.run([str(py), str(idf_py), "build"], cwd=ROOT, env=env)
    if r.returncode != 0:
        sys.exit(f"error: idf.py build failed ({r.returncode})")
    if not BIN.exists():
        sys.exit("error: build/bilimouse.bin 不存在")
    print(f"bin ok: {BIN.stat().st_size} bytes")

    # 3.5 资产双通道：
    #   GitHub Releases 的 CDN（Azure Front Door）对 ESP-IDF 的 TLS 指纹有
    #   bot 防护，会间歇/持续返回 618 + XHTML 拒绝页（PC curl 却是 200），
    #   设备根本拉不到。而 raw.githubusercontent.com（radio 台列表同源）
    #   实测对设备永远畅通。所以 manifest + bin 同时提交进仓库 ota/ 目录，
    #   manifest.url 指向 tag 化的 raw bin 路径（内容不可变，无缓存歧义）。
    OTA_DIR.mkdir(exist_ok=True)
    manifest = {
        "version": ver,
        "url": f"https://raw.githubusercontent.com/{REPO}/v{ver}/ota/bilimouse.bin",
        "notes": notes[:90],
        "size": BIN.stat().st_size,
    }
    (OTA_DIR / "manifest.json").write_bytes(json.dumps(manifest).encode())
    (OTA_DIR / "bilimouse.bin").write_bytes(BIN.read_bytes())
    run(["git", "add", "ota/manifest.json"])
    run(["git", "add", "-f", "ota/bilimouse.bin"])   # 仓库 .gitignore 掉了 *.bin
    subprocess.run(["git", "commit", "-m", f"ota assets: v{ver}"], cwd=ROOT)
    run(["git", "push", "origin", "master"])

    # 4. 创建 release（tag 自动打在远端 master HEAD，已含 ota/ 资产）
    rel = api("POST", f"https://api.github.com/repos/{REPO}/releases", token, {
        "tag_name": f"v{ver}",
        "target_commitish": "master",
        "name": f"v{ver}",
        "body": notes,
    })
    rid = rel["id"]
    print(f"release created: {rel['html_url']}")

    # 5. 上传 manifest.json + bilimouse.bin（Releases 通道，人下载用；
    #    设备 OTA 走 raw 通道，见 3.5）
    rel_manifest = dict(manifest)
    rel_manifest["url"] = f"{LATEST}/bilimouse.bin"
    api("POST",
        f"https://uploads.github.com/repos/{REPO}/releases/{rid}/assets?name=manifest.json",
        token, raw=json.dumps(rel_manifest).encode(), ctype="application/json")
    api("POST",
        f"https://uploads.github.com/repos/{REPO}/releases/{rid}/assets?name=bilimouse.bin",
        token, raw=BIN.read_bytes(), ctype="application/octet-stream")
    print("assets uploaded: manifest.json, bilimouse.bin")

    print(f"\ndone. 设备 OTA 地址(固定/raw 通道): {RAW_MASTER}/ota/manifest.json")
    print(f"      bin 资产地址: {manifest['url']}")


if __name__ == "__main__":
    main()
