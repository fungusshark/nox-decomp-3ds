#!/usr/bin/env python3
"""
Upload cataloged Nox playtest save games to the orphan `_playtest_saves` branch
(pure git plumbing like upload-gh-screenshots.py: no checkout, no `main` bloat)
and regenerate the branch README index table.

Catalog layout (one directory per save, made by the playtester):
  <catalog>/<class>/<map>/<name>/
      SAVE000N/Player.plr, SAVE000N/<Map>/<Map>.map   (campaign slot)  or  Q00.plr (Solo Quest)
      meta.json        {name, class, map, quest_stage, how_reached, slot_dir, game_version, commit, timestamp, restore}
      screenshot.png   (optional)

Usage:
  ./scripts/upload-playtest-saves.py ~/github-giraffepus/playtest-captures/saves/warrior/War01a/war01a_start [more dirs...]
  ./scripts/upload-playtest-saves.py --root ~/github-giraffepus/playtest-captures/saves --all

Each save lands at `<class>/<map>/<name>/` on the branch (replacing an older
upload of the same path). Prints the tree URL of each uploaded save, for
linking from issues. Restore: copy the SAVE000N/ dir (or Q00.plr) into
sdmc:/3ds/nox/save/ and load it from Solo -> Load Game (or Solo Quest).
"""

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

MAX_FILE = 4 * 1024 * 1024  # saves are ~0.4 MB; refuse anything odd


def git(*args, env=None, inp=None, text=True):
    return subprocess.check_output(["git", *args], env=env, input=inp, text=text)


def repo_info():
    out = subprocess.check_output(["gh", "repo", "view", "--json", "nameWithOwner,isPrivate"], text=True)
    d = json.loads(out)
    return d["nameWithOwner"], bool(d["isPrivate"])


def save_dirs(ns):
    if ns.all:
        return sorted(p.parent for p in Path(ns.root).expanduser().glob("*/*/*/meta.json"))
    return [Path(p).expanduser() for p in ns.dirs]


def readme(entries, repo, branch):
    rows = ["# Nox 3DS playtest saves", "",
            "Save games captured by the emulator playtester so maps can be resumed and retested after fixes.",
            "Restore: copy `SAVE000N/` (campaign) or `Q00.plr` (Solo Quest) into `sdmc:/3ds/nox/save/`, then",
            "Solo -> Load Game -> pick the row -> Load (or Solo Quest -> Load Character).",
            "Emulator-made (Azahar); hardware results may differ.", "",
            "| Class | Map | Name | Stage / position | Game | Path |", "|---|---|---|---|---|---|"]
    for path, m in sorted(entries.items()):
        cell = lambda s: str(s or "").replace("|", "/").replace("\n", " ")
        rows.append(f"| {cell(m.get('class'))} | {cell(m.get('map'))} | {cell(m.get('name'))} | "
                    f"{cell(m.get('quest_stage'))} | {cell(m.get('game_version'))} | [{path}]({path}/) |")
    return "\n".join(rows) + "\n"


def main():
    ap = argparse.ArgumentParser(description="Upload playtest saves to the orphan _playtest_saves branch")
    ap.add_argument("dirs", nargs="*", help="catalog save dirs (each has meta.json)")
    ap.add_argument("--root", default="~/github-giraffepus/playtest-captures/saves")
    ap.add_argument("--all", action="store_true", help="upload every save under --root")
    ap.add_argument("--branch", default="_playtest_saves")
    ap.add_argument("--dry-run", action="store_true")
    ns = ap.parse_args()
    dirs = save_dirs(ns)
    if not dirs:
        ap.error("no save dirs given")

    repo, private = repo_info()
    br = ns.branch
    subprocess.run(["git", "fetch", "-q", "origin", f"{br}:refs/remotes/origin/{br}"], stderr=subprocess.DEVNULL)
    r = subprocess.run(["git", "rev-parse", "--verify", f"refs/remotes/origin/{br}"], capture_output=True, text=True)
    parent = r.stdout.strip() if r.returncode == 0 else None

    idx = Path(f"/tmp/gh_playtest_saves_index_{os.getpid()}")
    idx.unlink(missing_ok=True)
    env = dict(os.environ, GIT_INDEX_FILE=str(idx))
    uploaded = []
    try:
        if parent:
            git("read-tree", parent, env=env)
        for d in dirs:
            meta = json.loads((d / "meta.json").read_text())
            rel = f"{meta['class']}/{meta['map'].split(' ')[0] if meta.get('slot_dir') else 'quest'}/{meta['name']}"
            # drop an older upload of the same save
            old = git("ls-files", "--", rel + "/", env=env).split()
            if old:
                git("update-index", "--force-remove", "--", *old, env=env)
            for f in sorted(p for p in d.rglob("*") if p.is_file()):
                if f.stat().st_size > MAX_FILE:
                    sys.exit(f"refusing {f}: {f.stat().st_size} bytes")
                blob = git("hash-object", "-w", str(f), env=env).strip()
                git("update-index", "--add", "--cacheinfo", f"100644,{blob},{rel}/{f.relative_to(d).as_posix()}", env=env)
            uploaded.append(rel)
        entries = {}
        for path in git("ls-files", env=env).split():
            if path.endswith("/meta.json"):
                entries[path[: -len("/meta.json")]] = json.loads(git("cat-file", "blob", f":{path}", env=env))
        blob = git("hash-object", "-w", "--stdin", env=env, inp=readme(entries, repo, br)).strip()
        git("update-index", "--add", "--cacheinfo", f"100644,{blob},README.md", env=env)
        tree = git("write-tree", env=env).strip()
        if ns.dry_run:
            print(readme(entries, repo, br))
            return 0
        cmd = ["commit-tree", tree, "-m", f"chore(saves): upload {len(uploaded)} playtest save(s)"]
        if parent:
            cmd += ["-p", parent]
        commit = git(*cmd, env=env).strip()
        subprocess.check_call(["git", "push", "-q", "origin", f"{commit}:refs/heads/{br}"])
    finally:
        idx.unlink(missing_ok=True)
    print(f"# Uploaded {len(uploaded)} save(s) to {repo}@{br} ({commit[:7]})")
    for rel in uploaded:
        print(f"https://github.com/{repo}/tree/{br}/{rel}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
