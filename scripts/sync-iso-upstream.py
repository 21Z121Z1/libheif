#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (c) 2026 libheif contributors
#
# This file is part of libheif.
#
# libheif is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# libheif is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with libheif. If not, see <https://www.gnu.org/licenses/>.
"""Prepare and publish a tested, fast-forward-only upstream/ISO integration."""

import argparse
import json
import os
from pathlib import Path
import re
import subprocess


BASE = "master"
ISO = "iso21496-gain-map-metadata-core"
BASE_REF = "refs/heads/codex/iso-sync-base"
ISO_REF = "refs/heads/codex/iso-sync-candidate"


def git(repo, *args):
    result = subprocess.run(["git", "-C", str(repo), *args], check=True,
                            text=True, stdout=subprocess.PIPE)
    return result.stdout.strip()


def ancestor(repo, older, newer):
    git(repo, "merge-base", "--is-ancestor", older, newer)


def require_clean(repo):
    if git(repo, "status", "--porcelain", "--untracked-files=no"):
        raise RuntimeError("Refusing to change a checkout with tracked edits")


def prepare(repo, upstream, output, validate_current=False):
    require_clean(repo)
    output.mkdir(parents=True, exist_ok=True)
    git(repo, "fetch", "--no-tags", "origin",
        f"refs/heads/{BASE}:refs/remotes/origin/{BASE}",
        f"refs/heads/{ISO}:refs/remotes/origin/{ISO}")
    git(repo, "fetch", "--no-tags", upstream, "refs/heads/master")
    upstream_sha = git(repo, "rev-parse", "FETCH_HEAD")
    base_before = git(repo, "rev-parse", f"refs/remotes/origin/{BASE}")
    iso_before = git(repo, "rev-parse", f"refs/remotes/origin/{ISO}")
    git(repo, "config", "user.name", "github-actions[bot]")
    git(repo, "config", "user.email", "41898282+github-actions[bot]@users.noreply.github.com")
    # This command runs only in a disposable Actions checkout or a test fixture.
    # Existing local branches are never reset or reused.
    try:
        git(repo, "checkout", "-b", BASE_REF.removeprefix("refs/heads/"), base_before)
        git(repo, "merge", "--no-edit", upstream_sha)
        base_after = git(repo, "rev-parse", "HEAD")
        git(repo, "checkout", "-b", ISO_REF.removeprefix("refs/heads/"), iso_before)
        git(repo, "merge", "--no-edit", base_after)
        iso_after = git(repo, "rev-parse", "HEAD")
    except subprocess.CalledProcessError:
        conflicts = git(repo, "diff", "--name-only", "--diff-filter=U")
        (output / "conflicts.txt").write_text(conflicts + "\n", encoding="utf-8")
        raise
    result = dict(base_before=base_before, iso_before=iso_before,
                  base_after=base_after, iso_after=iso_after, upstream=upstream_sha,
                  changed=base_before != base_after or iso_before != iso_after)
    (output / "manifest.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    if result["changed"] or validate_current:
        git(repo, "bundle", "create", str(output / "source.bundle"), BASE_REF, ISO_REF)
    if os.environ.get("GITHUB_OUTPUT"):
        with open(os.environ["GITHUB_OUTPUT"], "a", encoding="utf-8") as stream:
            for key, value in result.items():
                stream.write(f"{key}={str(value).lower() if isinstance(value, bool) else value}\n")
    return result


def publish(repo, output):
    require_clean(repo)
    result = json.loads((output / "manifest.json").read_text(encoding="utf-8"))
    for key in ("base_before", "iso_before", "base_after", "iso_after", "upstream"):
        if not re.fullmatch(r"[0-9a-f]{40}", result[key]):
            raise RuntimeError(f"Invalid commit in manifest: {key}")
    git(repo, "bundle", "verify", str(output / "source.bundle"))
    git(repo, "fetch", str(output / "source.bundle"),
        f"{BASE_REF}:{BASE_REF}", f"{ISO_REF}:{ISO_REF}")
    if git(repo, "rev-parse", BASE_REF) != result["base_after"] or \
            git(repo, "rev-parse", ISO_REF) != result["iso_after"]:
        raise RuntimeError("Bundle refs do not match the tested manifest")
    ancestor(repo, result["base_before"], result["base_after"])
    ancestor(repo, result["iso_before"], result["iso_after"])
    ancestor(repo, result["upstream"], result["base_after"])
    ancestor(repo, result["base_after"], result["iso_after"])
    remote = dict(line.split()[::-1] for line in git(
        repo, "ls-remote", "origin", f"refs/heads/{BASE}", f"refs/heads/{ISO}").splitlines())
    if remote.get(f"refs/heads/{BASE}") != result["base_before"] or \
            remote.get(f"refs/heads/{ISO}") != result["iso_before"]:
        raise RuntimeError("A maintained branch changed during validation; retry from its new head")
    # --atomic prevents updating only one branch. No force or history rewrite.
    git(repo, "push", "--atomic", "origin",
        f"{result['base_after']}:refs/heads/{BASE}",
        f"{result['iso_after']}:refs/heads/{ISO}")
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "publish"))
    parser.add_argument("--repo", type=Path, default=Path.cwd())
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--upstream", default="https://github.com/strukturag/libheif.git")
    parser.add_argument("--validate-current", action="store_true")
    args = parser.parse_args()
    output = args.output.resolve()
    result = (prepare(args.repo, args.upstream, output, args.validate_current) if args.mode == "prepare"
              else publish(args.repo, output))
    print(json.dumps(result, indent=2))
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"], "a", encoding="utf-8") as stream:
            stream.write(f"ISO upstream sync: {args.mode}\n\n```json\n")
            stream.write(json.dumps(result, indent=2) + "\n```\n")


if __name__ == "__main__":
    main()
