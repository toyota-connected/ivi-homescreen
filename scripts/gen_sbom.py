#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ivi-homescreen contributors
#
# Emits a CycloneDX 1.6 SBOM for this repository's vendored dependencies.
#
# The C++ dependencies are git submodules, which carry no package metadata, so a
# scanner walking the source tree finds nothing to report. This reads the pinned
# submodule commits out of git and names them as components with `pkg:github`
# PURLs, which is the form advisory databases match on.
#
# Every submodule must appear in LICENSES below. A submodule that does not is an
# error rather than an omission: a dependency missing from an SBOM is worse than
# no SBOM, because it reads as "scanned, nothing found".
#
#   scripts/gen_sbom.py --output sbom.cdx.json
#   scripts/gen_sbom.py --engine-version <sha> --output sbom.cdx.json

import argparse
import configparser
import json
import re
import subprocess
import sys
import uuid
from datetime import datetime, timezone
from pathlib import Path

# SPDX identifier per submodule, read from the license file each one ships at the
# commit this repository pins. The file is named so the claim can be rechecked.
LICENSES = {
    "third_party/asio": ("BSL-1.0", "asio/LICENSE_1_0.txt"),
    "third_party/sanitizers-cmake": ("MIT", "LICENSE"),
    "third_party/cxxopts": ("MIT", "LICENSE"),
    "third_party/rapidjson": ("MIT", "license.txt"),
    "third_party/tomlplusplus": ("MIT", "LICENSE"),
    "third_party/Vulkan-Headers": ("Apache-2.0", "LICENSE.md"),
    "third_party/drm-cxx": ("MIT", "LICENSE"),
    "third_party/wayland-cxx-scanner": ("MIT", "COPYING"),
    "third_party/fmt": ("MIT", "LICENSE"),
    "third_party/googletest": ("BSD-3-Clause", "LICENSE"),
}


def run(args, cwd):
    return subprocess.run(
        args, cwd=cwd, check=True, capture_output=True, text=True
    ).stdout.strip()


def github_slug(url):
    """owner/repo for a GitHub remote, or None when it is hosted elsewhere."""
    match = re.match(r"^(?:https://|git@)github\.com[/:]([^/]+)/(.+?)(?:\.git)?$", url)
    return f"{match.group(1)}/{match.group(2)}" if match else None


def submodules(root):
    """Each submodule as (path, url), in .gitmodules order."""
    config = configparser.ConfigParser()
    config.read(root / ".gitmodules")
    found = []
    for section in config.sections():
        if not section.startswith("submodule "):
            continue
        found.append((config[section]["path"], config[section]["url"]))
    return found


def pinned_commit(root, path):
    """The commit this repository pins, without needing the submodule checked out."""
    line = run(["git", "ls-tree", "HEAD", path], cwd=root)
    if not line:
        raise SystemExit(f"gen_sbom: {path} is in .gitmodules but not in the tree")
    return line.split()[2]


def component(name, version, purl, license_id, url=None):
    entry = {
        "type": "library",
        "name": name,
        "version": version,
        "purl": purl,
        "licenses": [{"license": {"id": license_id}}],
    }
    if url:
        entry["externalReferences"] = [{"type": "vcs", "url": url}]
    return entry


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", default="-", help="output path, or - for stdout")
    parser.add_argument(
        "--engine-version",
        help="Flutter engine commit this build links, when known. The engine is "
        "fetched at build time rather than pinned in-tree, so it can only be "
        "named by whoever ran the build.",
    )
    parser.add_argument(
        "--repo-root", default=None, help="defaults to this script's repository"
    )
    args = parser.parse_args()

    root = Path(args.repo_root) if args.repo_root else Path(__file__).resolve().parent.parent

    unknown = [p for p, _ in submodules(root) if p not in LICENSES]
    if unknown:
        raise SystemExit(
            "gen_sbom: no license recorded for "
            + ", ".join(unknown)
            + "\nAdd it to LICENSES in this script, reading the license file the "
            "submodule ships. A dependency missing from an SBOM reads as "
            '"scanned, nothing found", which is worse than no SBOM.'
        )

    components = []
    for path, url in submodules(root):
        slug = github_slug(url)
        commit = pinned_commit(root, path)
        license_id, _ = LICENSES[path]
        components.append(
            component(
                name=Path(path).name,
                version=commit,
                purl=f"pkg:github/{slug}@{commit}" if slug else f"pkg:generic/{Path(path).name}@{commit}",
                license_id=license_id,
                url=url,
            )
        )

    if args.engine_version:
        components.append(
            component(
                name="flutter-engine",
                version=args.engine_version,
                purl=f"pkg:github/flutter/flutter@{args.engine_version}",
                license_id="BSD-3-Clause",
                url="https://github.com/flutter/flutter",
            )
        )

    try:
        described = run(["git", "describe", "--tags", "--always", "--dirty"], cwd=root)
    except subprocess.CalledProcessError:
        described = run(["git", "rev-parse", "HEAD"], cwd=root)

    document = {
        "bomFormat": "CycloneDX",
        "specVersion": "1.6",
        "serialNumber": f"urn:uuid:{uuid.uuid4()}",
        "version": 1,
        "metadata": {
            "timestamp": datetime.now(timezone.utc).isoformat(timespec="seconds"),
            "tools": {"components": [{"type": "application", "name": "gen_sbom.py"}]},
            "component": {
                "type": "application",
                "name": "ivi-homescreen",
                "version": described,
                "purl": f"pkg:github/toyota-connected/ivi-homescreen@{described}",
                "externalReferences": [
                    {
                        "type": "vcs",
                        "url": "https://github.com/toyota-connected/ivi-homescreen",
                    }
                ],
            },
        },
        "components": components,
    }

    text = json.dumps(document, indent=2) + "\n"
    if args.output == "-":
        sys.stdout.write(text)
    else:
        Path(args.output).write_text(text)
        print(f"gen_sbom: {len(components)} components -> {args.output}")


if __name__ == "__main__":
    main()
