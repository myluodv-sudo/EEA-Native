#!/usr/bin/env python3
"""通过 GitHub Git Data API 一次性提交多个文件。

用途：当 `github.com` 主域的 git 通道不可用（例如代理只放行 api.github.com）
时，改用 REST API 提交，效果等同于一次普通 commit。

用法：
    cd native
    GH_TOKEN=xxxx python tools/gh_api_commit.py <<'MSG'
    提交信息第一行

    正文...
    MSG

注意：token 从环境变量读取，不写入任何文件。
"""
from __future__ import annotations

import base64
import json
import os
import subprocess
import sys

TOKEN = os.environ["GH_TOKEN"]
REPO = "myluodv-sudo/EEA-Native"
API = "https://api.github.com"

# 需要提交的文件（相对当前目录）
FILES = [
    "CMakePresets.json",
    ".github/workflows/build.yml",
]


def curl(method: str, path: str, payload=None):
    """用 curl 发请求 —— 复用系统代理设置，比 Python urllib 更稳。"""
    cmd = [
        "curl", "-s", "--max-time", "60",
        "-H", f"Authorization: token {TOKEN}",
        "-H", "User-Agent: EEA-CI",
        "-H", "Accept: application/vnd.github+json",
        "-X", method,
    ]
    if payload is not None:
        cmd += ["-H", "Content-Type: application/json", "--data-binary", "@-"]
    cmd.append(API + path)

    body = json.dumps(payload).encode("utf-8") if payload is not None else None
    proc = subprocess.run(cmd, input=body, capture_output=True)
    text = proc.stdout.decode("utf-8", "replace")
    try:
        return json.loads(text)
    except Exception:
        print("  !! 无法解析响应:", text[:600])
        sys.exit(1)


def main() -> None:
    message = sys.stdin.read().strip()
    if not message:
        print("  !! 提交信息为空")
        sys.exit(1)

    print("1) 读取 main 分支当前提交")
    ref = curl("GET", f"/repos/{REPO}/git/ref/heads/main")
    base_commit = ref["object"]["sha"]
    print("   base commit :", base_commit[:12])

    print("2) 读取 base tree")
    commit = curl("GET", f"/repos/{REPO}/git/commits/{base_commit}")
    base_tree = commit["tree"]["sha"]
    print("   base tree   :", base_tree[:12])

    print("3) 逐个创建 blob")
    items = []
    for name in FILES:
        data = base64.b64encode(open(name, "rb").read()).decode()
        blob = curl("POST", f"/repos/{REPO}/git/blobs",
                    {"content": data, "encoding": "base64"})
        print(f"   {name:<36} -> {blob['sha'][:12]}")
        items.append({"path": name, "mode": "100644", "type": "blob", "sha": blob["sha"]})

    print("4) 创建新 tree")
    tree = curl("POST", f"/repos/{REPO}/git/trees",
                {"base_tree": base_tree, "tree": items})
    print("   new tree    :", tree["sha"][:12])

    print("5) 创建新 commit")
    new_commit = curl("POST", f"/repos/{REPO}/git/commits",
                      {"message": message, "tree": tree["sha"], "parents": [base_commit]})
    print("   new commit  :", new_commit["sha"][:12])

    print("6) 更新 main 引用")
    updated = curl("PATCH", f"/repos/{REPO}/git/refs/heads/main", {"sha": new_commit["sha"]})
    print("   main ->", updated.get("object", {}).get("sha", "?")[:12])
    print("\n完成。远程 main 已指向", new_commit["sha"])


if __name__ == "__main__":
    main()
