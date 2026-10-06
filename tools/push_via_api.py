"""Push the current HEAD to GitHub over the REST Git Data API.

Why this exists
---------------
On some networks `github.com:443` is unreachable or resets connections, so
`git push` fails with

    fatal: unable to access 'https://github.com/<owner>/<repo>.git/':
    Recv failure: Connection was reset

while `api.github.com` still answers. This script rebuilds the commit through
the REST API instead, which speaks to a different host.

It reproduces the *committed* objects, not the working tree -- content is read
with `git cat-file`, so line-ending normalisation from .gitattributes is
preserved exactly.

Keeping the SHA identical
-------------------------
The commit is created with the same tree, message, author/committer identities
and timestamps as the local commit, so the API-produced commit hashes to the
same SHA and the local and remote branches stay in sync. Every one of those
fields matters:

  * the message must keep its exact trailing newline (read in binary from the
    commit object; `git log --pretty=%B` would have it stripped)
  * the timestamps must come from the commit, not from `now`
  * the tree must be the tree of the commit, not of the working tree

If the reported SHA does not match `git rev-parse HEAD`, the remote is still
correct but the local branch will need `git reset --hard` after the next
successful fetch.

Bootstrapping an empty repository
---------------------------------
GitHub rejects every Git Data API write against a repository with no commits
("Git Repository is empty", 409). So a throwaway file is created through the
Contents API first, which also creates the branch ref. The real content is then
published as a parentless root commit and the ref is force-moved onto it, which
leaves the throwaway commit orphaned and the published history a single clean
commit.

Usage
-----
    python tools/push_via_api.py

Requires the `gh` CLI to be authenticated (the token is read from
`gh auth token` and never written to disk).
"""

import base64
import json
import re
import subprocess
import sys
import urllib.error
import urllib.request


def sh(*args, binary=False):
    out = subprocess.run(args, capture_output=True, check=True).stdout
    return out if binary else out.decode("utf-8", "replace")


def remote_repo():
    """Return (owner/repo, branch) from the git remote and current branch."""
    url = sh("git", "remote", "get-url", "origin").strip()
    m = re.search(r"github\.com[:/]+([^/]+/[^/]+?)(?:\.git)?$", url)
    if not m:
        sys.exit(f"cannot parse a GitHub owner/repo out of remote origin: {url}")
    branch = sh("git", "rev-parse", "--abbrev-ref", "HEAD").strip()
    if branch == "HEAD":
        sys.exit("HEAD is detached; check out a branch first")
    return m.group(1), branch


REPO, BRANCH = remote_repo()
TOKEN = sh("gh", "auth", "token").strip()


def api(method, path, payload=None):
    url = "https://api.github.com" + path
    data = json.dumps(payload).encode() if payload is not None else None
    req = urllib.request.Request(url, data=data, method=method)
    req.add_header("Authorization", "Bearer " + TOKEN)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    req.add_header("User-Agent", "ghost-browser-push")
    if data:
        req.add_header("Content-Type", "application/json")
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            body = r.read()
            return json.loads(body) if body else {}
    except urllib.error.HTTPError as e:
        print(f"  !! {method} {path} -> {e.code}: {e.read().decode()[:400]}", file=sys.stderr)
        raise


print(f"pushing {REPO} branch {BRANCH} via the REST API")

# ---- 0. bootstrap an empty repository ------------------------------------
try:
    api("PUT", f"/repos/{REPO}/contents/.bootstrap",
        {"message": "chore: bootstrap repository",
         "content": base64.b64encode(b"x").decode(), "branch": BRANCH})
    print("bootstrapped empty repo via Contents API")
except urllib.error.HTTPError as e:
    if e.code in (409, 422):
        print("repo already has commits; skipping bootstrap")
    else:
        raise

# ---- 1. read the exact tree from git -------------------------------------
entries = []
for line in sh("git", "ls-tree", "-r", "HEAD").splitlines():
    meta, path = line.split("\t", 1)
    mode, otype, sha = meta.split()
    assert otype == "blob", f"unexpected object type {otype} for {path}"
    entries.append((mode, sha, path))

print(f"HEAD has {len(entries)} blobs")

# ---- 2. create blobs -----------------------------------------------------
tree_items = []
for i, (mode, sha, path) in enumerate(entries, 1):
    raw = sh("git", "cat-file", "blob", sha, binary=True)
    res = api("POST", f"/repos/{REPO}/git/blobs",
              {"content": base64.b64encode(raw).decode(), "encoding": "base64"})
    tree_items.append({"path": path, "mode": mode, "type": "blob", "sha": res["sha"]})
    print(f"  [{i:>2}/{len(entries)}] {path}  ({len(raw):,} B)")

# ---- 3. tree -> commit -> ref -------------------------------------------
tree = api("POST", f"/repos/{REPO}/git/trees", {"tree": tree_items})
print(f"tree    = {tree['sha']}")

# Read the message straight out of the commit object, in binary, so its exact
# trailing newline survives. GitHub stores the message byte for byte, and
# dropping that final newline changes the resulting commit SHA.
message = sh("git", "cat-file", "commit", "HEAD", binary=True).partition(b"\n\n")[2].decode("utf-8")

author = {
    "name": sh("git", "show", "-s", "--format=%an", "HEAD").strip(),
    "email": sh("git", "show", "-s", "--format=%ae", "HEAD").strip(),
    "date": sh("git", "show", "-s", "--format=%aI", "HEAD").strip(),
}
committer = {
    "name": sh("git", "show", "-s", "--format=%cn", "HEAD").strip(),
    "email": sh("git", "show", "-s", "--format=%ce", "HEAD").strip(),
    "date": sh("git", "show", "-s", "--format=%cI", "HEAD").strip(),
}

commit = api("POST", f"/repos/{REPO}/git/commits",
             {"message": message, "tree": tree["sha"],
              "author": author, "committer": committer})
print(f"commit  = {commit['sha']}")

local = sh("git", "rev-parse", "HEAD").strip()
if commit["sha"] != local:
    print(f"  !! SHA MISMATCH: local {local} != remote {commit['sha']}")
    print("  !! the remote commit is correct, but the local branch will need")
    print("  !! `git reset --hard` after the next successful fetch")
else:
    print("  SHA matches the local commit -- histories stay in sync")

try:
    api("POST", f"/repos/{REPO}/git/refs",
        {"ref": f"refs/heads/{BRANCH}", "sha": commit["sha"]})
    print(f"created refs/heads/{BRANCH}")
except urllib.error.HTTPError:
    # The bootstrap commit already created the branch ref, so this becomes a
    # forced move onto our clean root commit. force must be true: the bootstrap
    # commit is not an ancestor, so a plain update is rejected as
    # "Update is not a fast forward".
    api("PATCH", f"/repos/{REPO}/git/refs/heads/{BRANCH}",
        {"sha": commit["sha"], "force": True})
    print(f"updated refs/heads/{BRANCH}")

print("\nDONE")
print(f"local  HEAD = {sh('git', 'rev-parse', 'HEAD').strip()}")
print(f"remote HEAD = {commit['sha']}")
print(f"url = https://github.com/{REPO}")
