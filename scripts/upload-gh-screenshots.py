#!/usr/bin/env python3
"""
Upload PNG screenshots to an isolated orphan `_screenshots` branch on GitHub
using pure git plumbing (zero working-tree checkout, zero `main` repo bloat)
and print full-width inline Markdown (`<img width="760" ... />`) that renders
large inline on GitHub Issues/PRs and opens in GitHub's lightbox without
triggering `Content-Disposition: attachment` file downloads.

Usage:
  ./scripts/upload-gh-screenshots.py <subdir> <img1.png> [img2.png ...]
Example:
  ./scripts/upload-gh-screenshots.py issue-34 /tmp/ui34_servopts_game.png
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def run(cmd: list[str], input_bytes: bytes | None = None) -> bytes:
    return subprocess.check_output(cmd, input=input_bytes)


def get_repo_info() -> tuple[str, bool]:
    out = subprocess.check_output(
        ["gh", "repo", "view", "--json", "nameWithOwner,isPrivate"],
        text=True,
    )
    info = json.loads(out)
    return info["nameWithOwner"], bool(info["isPrivate"])


def main() -> int:
    ap = argparse.ArgumentParser(description="Upload screenshots to orphan _screenshots branch for large inline GitHub embeds")
    ap.add_argument("subdir", help="Subdirectory on _screenshots branch (e.g. 'issue-34')")
    ap.add_argument("files", nargs="+", help="PNG/JPG screenshot paths to upload")
    ap.add_argument("--branch", default="_screenshots", help="Orphan branch name (default: _screenshots)")
    ap.add_argument("--width", type=int, default=720, help="Inline display width in px (default: 720)")
    args = ap.parse_args()

    repo_slug, is_private = get_repo_info()
    branch = args.branch
    subdir = args.subdir.strip("/")

    # Fetch existing _screenshots branch if it exists on origin
    parent_sha = None
    subprocess.run(["git", "fetch", "-q", "origin", f"{branch}:refs/remotes/origin/{branch}"], stderr=subprocess.DEVNULL)
    res = subprocess.run(["git", "rev-parse", "--verify", f"refs/remotes/origin/{branch}"], capture_output=True, text=True)
    if res.returncode == 0:
        parent_sha = res.stdout.strip()

    # Use a temporary git index file so the working tree & real index are never touched
    tmp_index = Path(f"/tmp/gh_screenshots_index_{os.getpid()}")
    tmp_index.unlink(missing_ok=True)
    env = os.environ.copy()
    env["GIT_INDEX_FILE"] = str(tmp_index)

    try:
        if parent_sha:
            subprocess.check_call(["git", "read-tree", parent_sha], env=env)

        uploaded = []
        for fpath in args.files:
            p = Path(fpath)
            if not p.is_file():
                print(f"WARN: skipping missing file {p}", file=sys.stderr)
                continue
            blob_sha = subprocess.check_output(["git", "hash-object", "-w", str(p)], env=env, text=True).strip()
            rel_path = f"{subdir}/{p.name}" if subdir else p.name
            subprocess.check_call(
                ["git", "update-index", "--add", "--cacheinfo", f"100644,{blob_sha},{rel_path}"],
                env=env,
            )
            uploaded.append((p.name, rel_path))

        if not uploaded:
            print("ERROR: no valid files to upload", file=sys.stderr)
            return 1

        tree_sha = subprocess.check_output(["git", "write-tree"], env=env, text=True).strip()
        commit_cmd = ["git", "commit-tree", tree_sha, "-m", f"chore(screenshots): upload {len(uploaded)} image(s) to {subdir}"]
        if parent_sha:
            commit_cmd.extend(["-p", parent_sha])
        commit_sha = subprocess.check_output(commit_cmd, env=env, text=True).strip()

        subprocess.check_call(["git", "push", "-q", "origin", f"{commit_sha}:refs/heads/{branch}"])
    finally:
        tmp_index.unlink(missing_ok=True)

    print(f"# Uploaded {len(uploaded)} screenshot(s) to {repo_slug}@{branch}/{subdir} ({commit_sha[:7]}):\n")
    for name, rel_path in uploaded:
        if is_private:
            url = f"https://github.com/{repo_slug}/blob/{branch}/{rel_path}?raw=true"
        else:
            url = f"https://raw.githubusercontent.com/{repo_slug}/{branch}/{rel_path}"
        print(f'<p><img src="{url}" width="{args.width}" alt="{name}" /></p>')
    return 0


if __name__ == "__main__":
    sys.exit(main())
