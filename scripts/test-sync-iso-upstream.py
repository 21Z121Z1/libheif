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
"""Exercise upstream integration against temporary real Git repositories."""

import importlib.util
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


SPEC = importlib.util.spec_from_file_location(
    "sync_iso", Path(__file__).with_name("sync-iso-upstream.py"))
SYNC = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(SYNC)


def run(*args):
    return subprocess.run(["git", *map(str, args)], check=True, text=True,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE).stdout.strip()


class SyncTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="libheif-sync-test-")
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.origin = self.root / "origin.git"
        self.author = self.root / "author"
        self.work = self.root / "prepare"
        self.publisher = self.root / "publisher"
        self.output = self.root / "artifact"
        run("init", "--bare", "--initial-branch=master", self.origin)
        run("clone", self.origin, self.author)
        run("-C", self.author, "config", "user.name", "Test")
        run("-C", self.author, "config", "user.email", "test@example.invalid")
        self.commit("common.txt", "initial\n")
        run("-C", self.author, "push", "origin", "master")
        self.upstream = self.root / "upstream.git"
        run("clone", "--bare", self.author, self.upstream)
        run("-C", self.author, "checkout", "-b", SYNC.ISO)
        self.commit("iso.txt", "ISO support\n")
        run("-C", self.author, "push", "origin", SYNC.ISO)
        run("clone", self.origin, self.work)
        run("clone", self.origin, self.publisher)

    def commit(self, name, contents):
        (self.author / name).write_text(contents)
        run("-C", self.author, "add", name)
        run("-C", self.author, "commit", "-m", name)

    def advance_upstream(self, name="upstream.txt", contents="new upstream\n"):
        run("-C", self.author, "checkout", "master")
        self.commit(name, contents)
        run("-C", self.author, "push", self.upstream, "master")
        run("-C", self.author, "checkout", SYNC.ISO)

    def refs(self):
        return run("ls-remote", self.origin, "refs/heads/master", f"refs/heads/{SYNC.ISO}")

    def prepare(self):
        return SYNC.prepare(self.work, str(self.upstream), self.output)

    def test_no_change(self):
        before = self.refs()
        self.assertFalse(self.prepare()["changed"])
        self.assertFalse((self.output / "source.bundle").exists())
        self.assertEqual(before, self.refs())

    def test_manual_validation_can_bundle_unchanged_source(self):
        before = self.refs()
        result = SYNC.prepare(self.work, str(self.upstream), self.output, validate_current=True)
        self.assertFalse(result["changed"])
        SYNC.publish(self.publisher, self.output)
        self.assertEqual(before, self.refs())

    def test_merge_retains_iso_and_publishes_both_branches(self):
        self.advance_upstream()
        before = self.refs()
        result = self.prepare()
        self.assertTrue(result["changed"])
        self.assertEqual(before, self.refs(), "Preparation must not publish")
        self.assertEqual("ISO support", run("-C", self.work, "show", "HEAD:iso.txt"))
        self.assertEqual("new upstream", run("-C", self.work, "show", "HEAD:upstream.txt"))
        SYNC.publish(self.publisher, self.output)
        self.assertEqual(result["base_after"], run("-C", self.origin, "rev-parse", "master"))
        self.assertEqual(result["iso_after"], run("-C", self.origin, "rev-parse", SYNC.ISO))
        self.assertNotIn("iso.txt", run("-C", self.origin, "ls-tree", "--name-only", "master"))

    def test_conflict_does_not_publish(self):
        self.commit("common.txt", "ISO modification\n")
        run("-C", self.author, "push", "origin", SYNC.ISO)
        self.advance_upstream("common.txt", "upstream modification\n")
        before = self.refs()
        with self.assertRaises(subprocess.CalledProcessError):
            self.prepare()
        self.assertEqual(before, self.refs())
        self.assertEqual("common.txt\n", (self.output / "conflicts.txt").read_text())

    def test_concurrent_update_does_not_overwrite_either_branch(self):
        self.advance_upstream()
        self.prepare()
        self.commit("concurrent.txt", "new maintained work\n")
        run("-C", self.author, "push", "origin", SYNC.ISO)
        before = self.refs()
        with self.assertRaisesRegex(RuntimeError, "changed during validation"):
            SYNC.publish(self.publisher, self.output)
        self.assertEqual(before, self.refs())

    def test_atomic_rejection_preserves_both_branches(self):
        self.advance_upstream()
        self.prepare()
        hook = self.origin / "hooks" / "update"
        hook.write_text(f'#!/bin/sh\n[ "$1" != "refs/heads/{SYNC.ISO}" ]\n')
        hook.chmod(0o755)
        before = self.refs()
        with self.assertRaises(subprocess.CalledProcessError):
            SYNC.publish(self.publisher, self.output)
        self.assertEqual(before, self.refs())

    def test_manifest_must_match_bundle(self):
        self.advance_upstream()
        result = self.prepare()
        result["iso_after"] = result["base_after"]
        (self.output / "manifest.json").write_text(json.dumps(result))
        before = self.refs()
        with self.assertRaisesRegex(RuntimeError, "do not match"):
            SYNC.publish(self.publisher, self.output)
        self.assertEqual(before, self.refs())

    def test_non_fast_forward_candidate_is_rejected(self):
        # The manifest and bundle agree, but the ISO candidate is built on the
        # baseline line instead of descending from the maintained ISO head.
        # Publication must refuse a non-fast-forward even when refs match.
        self.advance_upstream()
        result = self.prepare()
        run("-C", self.work, "checkout", "--detach", result["base_before"])
        (self.work / "divergent.txt").write_text("not descended from ISO\n")
        run("-C", self.work, "add", "divergent.txt")
        run("-C", self.work, "commit", "-m", "divergent candidate")
        divergent = run("-C", self.work, "rev-parse", "HEAD")
        run("-C", self.work, "update-ref", SYNC.ISO_REF, divergent)
        run("-C", self.work, "bundle", "create", self.output / "source.bundle",
            SYNC.BASE_REF, SYNC.ISO_REF)
        result["iso_after"] = divergent
        (self.output / "manifest.json").write_text(json.dumps(result))
        before = self.refs()
        with self.assertRaises(subprocess.CalledProcessError):
            SYNC.publish(self.publisher, self.output)
        self.assertEqual(before, self.refs())

    def test_publish_requires_all_validation_gates(self):
        # Regression guard against weakening the gate: the publish job must
        # depend on every validation job and must not run via always()/failure
        # bypass, and no gate may be marked continue-on-error.
        import yaml
        workflow = yaml.safe_load(
            (Path(__file__).resolve().parent.parent /
             ".github" / "workflows" / "sync-iso-upstream.yml").read_text())
        jobs = workflow["jobs"]
        publish = jobs["publish"]
        self.assertEqual(set(publish["needs"]),
                         {"prepare", "gain-map", "core-tests", "source-checks"})
        self.assertNotIn("always()", publish.get("if", ""))
        self.assertNotIn("failure()", publish.get("if", ""))
        for name in ("gain-map", "core-tests", "source-checks"):
            self.assertNotIn("continue-on-error", jobs[name])
            for step in jobs[name].get("steps", []):
                self.assertNotIn("continue-on-error", step)


if __name__ == "__main__":
    unittest.main()
