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

    # 3. 编译（build.bat 在部分终端环境下即使编译成功也返回 1，
    #    所以这里不看退出码，以 bilimouse.bin 是否刷新为准）
    before = BIN.stat().st_mtime if BIN.exists() else 0
    subprocess.run(["cmd", "/c", str(ROOT / "tools" / "build.bat"), str(ROOT), "build"])
    if not BIN.exists() or BIN.stat().st_mtime <= before:
        sys.exit("error: build failed (bilimouse.bin not refreshed)")
    print(f"bin refreshed: {BIN.stat().st_size} bytes")

    # 4. 创建 release（tag 自动打在远端 master HEAD）
    rel = api("POST", f"https://api.github.com/repos/{REPO}/releases", token, {
        "tag_name": f"v{ver}",
        "target_commitish": "master",
        "name": f"v{ver}",
        "body": notes,
    })
    rid = rel["id"]
    print(f"release created: {rel['html_url']}")

    # 5. 上传 manifest.json + bilimouse.bin
    manifest = json.dumps({
        "version": ver,
        "url": f"{LATEST}/bilimouse.bin",
        "notes": notes[:90],
        "size": BIN.stat().st_size,   # 固件端检查更新时先显示包大小
    }).encode()
    api("POST",
        f"https://uploads.github.com/repos/{REPO}/releases/{rid}/assets?name=manifest.json",
        token, raw=manifest, ctype="application/json")
    api("POST",
        f"https://uploads.github.com/repos/{REPO}/releases/{rid}/assets?name=bilimouse.bin",
        token, raw=BIN.read_bytes(), ctype="application/octet-stream")
    print("assets uploaded: manifest.json, bilimouse.bin")

    print(f"\ndone. 设备 OTA 地址(固定): {LATEST}/manifest.json")


if __name__ == "__main__":
    main()
