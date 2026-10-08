#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
# Copyright (c) 2026 ivi-homescreen contributors
#
# Emits a CycloneDX 1.6 SBOM for this repository's vendored dependencies.
#
# The C++ dependencies are git submodules, which carry no package metadata, so a
# scanner walking the source tree finds nothing to report. This reads the pinned
# submodule commits out of git and names them as components.
#
# A `pkg:github` PURL names a component but does not make it scannable: advisory
# databases key on CPEs, and grype matches no `pkg:github` PURL at all -- not
# even one whose version has known CVEs. An SBOM carrying only PURLs therefore
# scans clean whatever is pinned in it, which is the same failure as omitting a
# component: a wrong answer that reads as an authoritative one. So each component
# that NVD lists also carries a CPE, and the releases those CPEs name are
# reconciled against the pin below.
#
# Every submodule must appear in DEPENDENCIES. One that does not is an error
# rather than an omission, for the same reason.
#
# The VEX entries are emitted twice, in two formats, because no scanner reads
# both. CycloneDX carries them inside the SBOM, where they belong and where a
# CycloneDX-aware tool finds them; grype ignores that and reads OpenVEX passed
# on the command line. Verified against grype 0.120.1: embedded CycloneDX VEX
# does not suppress a finding, `--vex` with the OpenVEX file does. Both come off
# the one VEX table below, so they cannot disagree.
#
#   scripts/gen_sbom.py --output sbom.cdx.json
#   scripts/gen_sbom.py --output sbom.cdx.json --vex-output vex.openvex.json
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

# Per-submodule metadata.
#
# license is the SPDX identifier read from the license file each submodule ships
# at the commit this repository pins. The file is named so the claim can be
# rechecked.
#
# cpe is the NVD dictionary entry a scanner matches on, as vendor:product, or
# None where NVD has no entry for the project. These were queried against the
# NVD CPE API rather than guessed: a CPE naming the wrong product silently
# imports some other project's advisories. Only rapidjson and fmt are listed --
# asio, cxxopts, tomlplusplus, googletest, Vulkan-Headers and sanitizers-cmake
# return nothing, and drm-cxx and wayland-cxx-scanner are ours. For fmt the
# vendor is `fmt`, not `fmt_project`: both are in the dictionary, but only the
# former is referenced by an advisory.
#
# scope is CycloneDX's. "required" is compiled into the delivered binary;
# "excluded" is not shipped, which is what makes a finding against it noise for
# anyone scanning an image. googletest builds only under BUILD_UNIT_TESTS and
# sanitizers-cmake contributes CMake modules; everything else ships, including
# the header-only parts of wayland-cxx-scanner and Vulkan-Headers.
DEPENDENCIES = {
    "third_party/asio": {
        "license": ("BSL-1.0", "asio/LICENSE_1_0.txt"),
        "cpe": None,
        "scope": "required",
    },
    "third_party/sanitizers-cmake": {
        "license": ("MIT", "LICENSE"),
        "cpe": None,
        "scope": "excluded",
    },
    "third_party/cxxopts": {
        "license": ("MIT", "LICENSE"),
        "cpe": None,
        "scope": "required",
    },
    "third_party/rapidjson": {
        "license": ("MIT", "license.txt"),
        "cpe": "tencent:rapidjson",
        "scope": "required",
    },
    "third_party/tomlplusplus": {
        "license": ("MIT", "LICENSE"),
        "cpe": None,
        "scope": "required",
    },
    "third_party/Vulkan-Headers": {
        "license": ("Apache-2.0", "LICENSE.md"),
        "cpe": None,
        "scope": "required",
    },
    "third_party/drm-cxx": {
        "license": ("MIT", "LICENSE"),
        "cpe": None,
        "scope": "required",
    },
    "third_party/wayland-cxx-scanner": {
        "license": ("MIT", "COPYING"),
        "cpe": None,
        "scope": "required",
    },
    "third_party/fmt": {
        "license": ("MIT", "LICENSE"),
        "cpe": "fmt:fmt",
        "scope": "required",
    },
    "third_party/googletest": {
        "license": ("BSD-3-Clause", "LICENSE"),
        "cpe": None,
        "scope": "excluded",
    },
}

# What we assert about advisories a CPE above will match but that do not apply to
# the commit pinned.
#
# Only rapidjson needs entries, because it is the only component whose CPE names
# a release older than its pin: there has been no rapidjson release since 1.1.0,
# so the CPE says 1.1.0 while the pin is hundreds of commits past it. Without
# these, the SBOM reports 1.1.0's CVEs against code that does not contain them.
#
# `fixed_by` is verified against the submodule every time this runs, so the claim
# cannot outlive the pin that justified it -- move the pin back before the fix
# and generating the SBOM fails rather than keeps asserting not_affected. An
# entry without `fixed_by` asserts nothing and is emitted as-is.
VEX = {
    "third_party/rapidjson": [
        {
            "id": "CVE-2024-38517",
            "state": "not_affected",
            "justification": "code_not_present",
            "fixed_by": "8269bc2bc289e9d343bae51cdf6d23ef0950e001",
            "detail": (
                "Integer underflow in GenericReader::ParseNumber(). Fixed "
                "upstream by 8269bc2b 'Prevent int underflow when parsing "
                "exponents', which replaced the 1.1.0 exponent guard "
                "(`exp >= 214748364`) with a bound derived from expFrac. The "
                "pinned commit descends from that fix and carries the replacement "
                "guard, so the vulnerable arithmetic is not present."
            ),
        },
        {
            "id": "CVE-2024-39684",
            "state": "in_triage",
            "detail": (
                "Integer overflow in GenericReader::ParseNumber(), the sibling "
                "of CVE-2024-38517. Advisories give the affected range as "
                "<= 1.1.0 and note that distributions shipped patches, but none "
                "publishes an upstream fixing commit, so there is nothing to "
                "verify the pin against. The pin is well past 1.1.0 and the "
                "adjacent exponent arithmetic was rewritten by 8269bc2b, which "
                "makes it likely fixed -- not enough to assert not_affected."
            ),
        },
    ]
}


# This script reads two things out of git that a shallow checkout does not have,
# and actions/checkout takes submodules at depth 1 with no tags.
#
# Tags give every component its release version, and are wanted for all of them:
# without them a component falls back to its commit, which is accurate but tells
# a reader less, and makes the document's contents depend on how it was cloned.
# Enough history to resolve an ancestor is only needed where a VEX entry claims
# one, which is --deep-submodules, and is the expensive half.
#
# Missing either is fatal rather than degraded. An SBOM with no CPEs is the bug
# this script exists to fix, and it would otherwise publish looking fine.
SHALLOW_HINT = (
    "The submodule is shallow or missing its tags. Fetch tags for all of them, "
    "and full history for the ones asserting an ancestor:\n"
    "  git submodule foreach --quiet 'git fetch --tags origin || true'\n"
    "  for m in $(scripts/gen_sbom.py --deep-submodules); do \\\n"
    "    git -C \"$m\" fetch --unshallow origin || true; done"
)

# CycloneDX and OpenVEX spell the same two assertions differently.
OPENVEX_STATUS = {"not_affected": "not_affected", "in_triage": "under_investigation"}
OPENVEX_JUSTIFICATION = {"code_not_present": "vulnerable_code_not_present"}


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


def normalize_version(tag):
    """An upstream tag as a version a CPE can carry.

    The current pins produce three shapes: `11.2.0`, `v1.4.364` and asio's
    `asio-1-38-2`. The `release-` prefix is handled too but is not currently
    reached -- it is googletest's older tag form, and a pin could land on one.
    Anything else is passed through rather than guessed at, since a wrong version
    is worse than an unusual-looking one.
    """
    text = tag
    for prefix in ("release-", "v"):
        if text.startswith(prefix):
            text = text[len(prefix) :]
            break
    match = re.match(r"^[A-Za-z][A-Za-z0-9_]*-(\d+(?:-\d+)+)$", text)
    return match.group(1).replace("-", ".") if match else text


def worktree(root, path):
    """The submodule's checkout, or None when it is not populated."""
    work = root / path
    return work if (work / ".git").exists() else None


def release_of(work):
    """(version, exact) from the submodule's own tags, or (None, False).

    `exact` is False when the pin sits some commits after the tag. That case is
    what the VEX entries exist for: the CPE then names a release the pin is past,
    so advisories closed between the two would otherwise be reported.
    """
    try:
        exact = run(["git", "describe", "--tags", "--exact-match"], cwd=work)
        return normalize_version(exact), True
    except subprocess.CalledProcessError:
        pass
    try:
        tag = run(["git", "describe", "--tags", "--abbrev=0"], cwd=work)
    except subprocess.CalledProcessError:
        return None, False  # no tags reachable at all
    return normalize_version(tag), False


def descends_from(work, commit):
    """"verified", "refuted" or "unavailable" for `commit` being an ancestor.

    The three are kept apart deliberately. `git merge-base --is-ancestor` exits
    non-zero both when the claim is false and when the commit is simply not in
    the object store, and a shallow submodule checkout -- which is what
    actions/checkout produces -- gives the second. Treating that as a refutation
    would report a moved pin that had not moved; treating it as a pass would
    assert not_affected having checked nothing.
    """
    try:
        subprocess.run(
            ["git", "cat-file", "-e", f"{commit}^{{commit}}"],
            cwd=work,
            check=True,
            capture_output=True,
        )
    except subprocess.CalledProcessError:
        return "unavailable"
    try:
        subprocess.run(
            ["git", "merge-base", "--is-ancestor", commit, "HEAD"],
            cwd=work,
            check=True,
            capture_output=True,
        )
        return "verified"
    except subprocess.CalledProcessError:
        return "refuted"


def cpe_for(vendor_product, version):
    vendor, product = vendor_product.split(":")
    return f"cpe:2.3:a:{vendor}:{product}:{version}:*:*:*:*:*:*:*"


def component(name, version, purl, license_id, scope, ref, cpe=None, url=None):
    entry = {
        "bom-ref": ref,
        "type": "library",
        "name": name,
        "version": version,
        "purl": purl,
        "scope": scope,
        "licenses": [{"license": {"id": license_id}}],
    }
    if cpe:
        entry["cpe"] = cpe
    if url:
        entry["externalReferences"] = [{"type": "vcs", "url": url}]
    return entry


def vulnerabilities(path, ref, purl, work, openvex_out):
    """VEX entries for one component, verifying every claim that makes one.

    Appends the OpenVEX form to `openvex_out` as it goes, so the two formats are
    built from one pass over one table.
    """
    out = []
    for entry in VEX.get(path, []):
        fix = entry.get("fixed_by")
        if fix:
            state = descends_from(work, fix)
            if state == "refuted":
                raise SystemExit(
                    f"gen_sbom: {path} asserts {entry['id']} is "
                    f"{entry['state']} because it descends from {fix[:12]}, but "
                    "the pinned commit does not contain it.\nThe pin moved. "
                    "Recheck the advisory against the new pin and update VEX in "
                    "this script -- an unverified not_affected is worse than no "
                    "SBOM, because a scanner treats it as an answer."
                )
            if state == "unavailable":
                raise SystemExit(
                    f"gen_sbom: {path} has no {fix[:12]} in its object store, so "
                    f"the {entry['id']} assertion cannot be checked.\n"
                    + SHALLOW_HINT
                )
        analysis = {"state": entry["state"], "detail": entry["detail"]}
        if "justification" in entry:
            analysis["justification"] = entry["justification"]
        out.append(
            {
                "id": entry["id"],
                "source": {
                    "name": "NVD",
                    "url": f"https://nvd.nist.gov/vuln/detail/{entry['id']}",
                },
                "analysis": analysis,
                "affects": [{"ref": ref}],
            }
        )

        # grype matches a VEX statement's product against the PURL it recorded
        # for the component, not the bom-ref.
        statement = {
            "vulnerability": {"name": entry["id"]},
            "products": [{"@id": purl}],
            "status": OPENVEX_STATUS[entry["state"]],
        }
        if "justification" in entry:
            statement["justification"] = OPENVEX_JUSTIFICATION[entry["justification"]]
        # OpenVEX requires a statement of impact for not_affected, and it is the
        # evidence either way.
        statement["impact_statement"] = entry["detail"]
        openvex_out.append(statement)
    return out


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
        "--deep-submodules",
        action="store_true",
        help="print the submodule paths whose full history this script needs, "
        "one per line, and exit -- the ones whose VEX entries assert an "
        "ancestor. The caller unshallows these after a shallow checkout; "
        "keeping the list here stops it drifting from the assertions that "
        "actually need it. Tags are wanted for every submodule, which needs no "
        "list.",
    )
    parser.add_argument(
        "--vex-output",
        help="also write the VEX assertions as an OpenVEX document, which is the "
        "form grype reads (grype ignores the copy embedded in the SBOM)",
    )
    parser.add_argument(
        "--repo-root", default=None, help="defaults to this script's repository"
    )
    args = parser.parse_args()

    root = Path(args.repo_root) if args.repo_root else Path(__file__).resolve().parent.parent

    if args.deep_submodules:
        for path, _ in submodules(root):
            if any("fixed_by" in entry for entry in VEX.get(path, [])):
                print(path)
        return

    unknown = [p for p, _ in submodules(root) if p not in DEPENDENCIES]
    if unknown:
        raise SystemExit(
            "gen_sbom: nothing recorded for "
            + ", ".join(unknown)
            + "\nAdd it to DEPENDENCIES in this script, reading the license file "
            "the submodule ships and checking whether NVD lists it. A dependency "
            'missing from an SBOM reads as "scanned, nothing found", which is '
            "worse than no SBOM."
        )

    components = []
    found_vulns = []
    openvex = []
    unpinned = []
    for path, url in submodules(root):
        meta = DEPENDENCIES[path]
        slug = github_slug(url)
        commit = pinned_commit(root, path)
        license_id, _ = meta["license"]
        name = Path(path).name
        ref = f"{name}@{commit[:12]}"

        work = worktree(root, path)
        release, exact = release_of(work) if work else (None, False)

        # A CPE is only as good as the version on it, and the version comes from
        # the submodule's own tags. No tags, no version, no CPE -- which is the
        # unscannable SBOM this script exists to avoid, so it is collected and
        # raised below rather than quietly published.
        cpe = cpe_for(meta["cpe"], release) if meta["cpe"] and release else None
        if meta["cpe"] and not release:
            unpinned.append(path)

        purl = f"pkg:github/{slug}@{commit}" if slug else f"pkg:generic/{name}@{commit}"
        components.append(
            component(
                name=name,
                version=release if exact else commit,
                purl=purl,
                license_id=license_id,
                scope=meta["scope"],
                ref=ref,
                cpe=cpe,
                url=url,
            )
        )
        if cpe:
            found_vulns.extend(vulnerabilities(path, ref, purl, work, openvex))

    if unpinned:
        raise SystemExit(
            "gen_sbom: no release could be read for "
            + ", ".join(unpinned)
            + ", so the component would carry no CPE and no scanner would match "
            "it.\n" + SHALLOW_HINT
        )

    if args.engine_version:
        components.append(
            component(
                name="flutter-engine",
                version=args.engine_version,
                purl=f"pkg:github/flutter/flutter@{args.engine_version}",
                license_id="BSD-3-Clause",
                scope="required",
                ref=f"flutter-engine@{args.engine_version[:12]}",
                url="https://github.com/flutter/flutter",
            )
        )

    # One timestamp for both documents, so a reader can tell they are a pair.
    now = datetime.now(timezone.utc).isoformat(timespec="seconds")

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
            "timestamp": now,
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
    if found_vulns:
        document["vulnerabilities"] = found_vulns

    text = json.dumps(document, indent=2) + "\n"
    if args.output == "-":
        sys.stdout.write(text)
    else:
        Path(args.output).write_text(text)
        scannable = sum(1 for c in components if "cpe" in c)
        print(
            f"gen_sbom: {len(components)} components "
            f"({scannable} with a CPE), {len(found_vulns)} VEX -> {args.output}"
        )
    if args.vex_output:
        if not openvex:
            raise SystemExit(
                "gen_sbom: --vex-output was asked for but there is nothing to "
                "assert. An empty VEX document reads as 'we checked and found "
                "nothing to say', which is not the same as not having looked."
            )
        Path(args.vex_output).write_text(
            json.dumps(
                {
                    "@context": "https://openvex.dev/ns/v0.2.0",
                    "@id": (
                        "https://github.com/toyota-connected/ivi-homescreen"
                        f"/vex/{described}"
                    ),
                    "author": "ivi-homescreen maintainers",
                    "timestamp": now,
                    "version": 1,
                    "statements": openvex,
                },
                indent=2,
            )
            + "\n"
        )
        print(f"gen_sbom: {len(openvex)} VEX statements -> {args.vex_output}")



if __name__ == "__main__":
    main()
