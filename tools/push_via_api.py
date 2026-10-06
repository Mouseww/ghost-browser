"""Push the current branch to GitHub over the REST Git Data API.

Why this exists
---------------
On some networks `github.com:443` is unreachable or resets connections, so
`git push` fails with

    fatal: unable to access 'https://github.com/<owner>/<repo>.git/':
    Recv failure: Connection was reset

while `api.github.com` still answers. This script publishes the same objects
through the Git Data API, which speaks to a different host.

What it publishes
-----------------
Every commit between the remote branch tip and the local HEAD, **oldest first,
each with its real parent list**. Commits that already exist on the remote are
skipped, so this is an incremental push rather than a re-upload.

An earlier version of this script omitted `parents` entirely and then force-moved
the branch ref. That turned every push into a parentless root commit and
orphaned the entire remote history: the remote branch ended up with one commit
and no ancestry. Parents are mandatory now. The only commit ever published
without one is a genuine root commit.

Safety
------
The branch ref is never force-moved past commits that the remote has and the
local repository does not. If the remote tip is not an ancestor of the local
HEAD, the script stops and asks for --force.

Keeping the SHA identical
-------------------------
The commit is recreated with the same tree, message and author/committer
identities as the local commit. GitHub normalises commit messages (it folds
CRLF to LF and drops the trailing newline), so the recreated object can hash
differently from the local one. When that happens the script reconstructs the
exact object GitHub stored, writes it into the local object database and moves
the local branch onto it. Both sides then sit on the same SHA and the next
`git push` is a fast-forward instead of a force-push.

Usage
-----
    python tools/push_via_api.py [--force]

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


# --------------------------------------------------------------------------
# git helpers
# --------------------------------------------------------------------------
def sh(*args, binary=False, check=True):
    p = subprocess.run(args, capture_output=True)
    if check and p.returncode != 0:
        sys.exit(f"git {' '.join(args)} failed:\n{p.stderr.decode('utf-8', 'replace')}")
    out = p.stdout
    return out if binary else out.decode("utf-8", "replace")


def remote_repo():
    """Return (owner/repo, branch) from the git remote and the current branch."""
    url = sh("git", "remote", "get-url", "origin").strip()
    m = re.search(r"github\.com[:/]+([^/]+/[^/]+?)(?:\.git)?$", url)
    if not m:
        sys.exit(f"cannot parse a GitHub owner/repo out of remote origin: {url}")
    branch = sh("git", "rev-parse", "--abbrev-ref", "HEAD").strip()
    if branch == "HEAD":
        sys.exit("HEAD is detached; check out a branch first")
    return m.group(1), branch


def parse_commit(sha):
    """Split a raw commit object into (header lines, message bytes)."""
    raw = sh("git", "cat-file", "commit", sha, binary=True)
    header, sep, message = raw.partition(b"\n\n")
    if not sep:
        sys.exit(f"commit {sha} has no header/message separator")
    return raw, header.decode("utf-8"), message


# --------------------------------------------------------------------------
# API
# --------------------------------------------------------------------------
class ApiError(Exception):
    def __init__(self, method, path, code, body):
        super().__init__(f"{method} {path} -> {code}: {body[:400]}")
        self.code = code


REPO, BRANCH = remote_repo()
TOKEN = sh("gh", "auth", "token").strip()


def api(method, path, payload=None, allow=()):
    """Call the API. Returns (status, body). `allow` lists non-2xx codes to
    return instead of raising."""
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
            return r.status, (json.loads(body) if body else {})
    except urllib.error.HTTPError as e:
        body = e.read().decode("utf-8", "replace")
        if e.code in allow:
            return e.code, {}
        raise ApiError(method, path, e.code, body)


def remote_commit(sha):
    """The commit object as the remote sees it, or None if it is not there."""
    status, body = api("GET", f"/repos/{REPO}/git/commits/{sha}", allow=(404,))
    return body if status == 200 else None


def remote_blob(sha):
    status, _ = api("GET", f"/repos/{REPO}/git/blobs/{sha}", allow=(404,))
    return status == 200


# --------------------------------------------------------------------------
# 0. bootstrap a repository that has no commits at all
# --------------------------------------------------------------------------
# GitHub rejects every Git Data API write against a repository with no commits
# ("Git Repository is empty", 409). A throwaway file through the Contents API
# makes the repository non-empty and creates the branch ref. It stays orphaned:
# the real history is published below as its own root commit.
status, ref = api("GET", f"/repos/{REPO}/git/ref/heads/{BRANCH}", allow=(404,))
if status == 404:
    print(f"branch {BRANCH} does not exist yet; bootstrapping")
    api("PUT", f"/repos/{REPO}/contents/.bootstrap",
        {"message": "chore: bootstrap repository",
         "content": base64.b64encode(b"x").decode(), "branch": BRANCH})
    status, ref = api("GET", f"/repos/{REPO}/git/ref/heads/{BRANCH}")

remote_tip = ref["object"]["sha"]
print(f"pushing {REPO} branch {BRANCH} via the REST API")
print(f"remote tip = {remote_tip}")

# --------------------------------------------------------------------------
# 1. work out which commits are missing remotely
# --------------------------------------------------------------------------
# A commit can only exist remotely if its parents do, because the API refuses
# to create a commit with an unknown parent. So the first commit we find that
# already exists marks the end of the missing run.
#
# Existing as an object is NOT the same as being published. If the ref update
# fails -- a dropped connection is enough -- the commits are left behind as
# orphans while the branch still points at the old tip, and treating them as
# published makes the next run print "nothing to publish" and exit 0 without
# ever moving the ref. Only reachability from the remote tip counts.
def commit_exists_locally(sha):
    return subprocess.run(["git", "cat-file", "-e", sha + "^{commit}"],
                          capture_output=True).returncode == 0


def published_on_remote(sha, tip):
    """True when `sha` is the remote tip, or an ancestor of it."""
    if sha == tip:
        return True
    if not commit_exists_locally(tip):
        # We cannot walk a tip we do not have, so only an exact match counts.
        return False
    return subprocess.run(["git", "merge-base", "--is-ancestor", sha, tip],
                          capture_output=True).returncode == 0


chain = sh("git", "rev-list", "HEAD").split()
missing = []
for sha in chain:                       # newest first
    if remote_commit(sha) and published_on_remote(sha, remote_tip):
        print(f"already published on the remote: {sha[:8]}")
        break
    missing.append(sha)
else:
    print("no commit of the local history is on the remote yet")

missing.reverse()                       # publish oldest first
if not missing:
    print("\nnothing to publish; the remote already has every commit")
    print(f"local  HEAD = {chain[0]}")
    print(f"remote HEAD = {remote_tip}")
    if chain[0] != remote_tip:
        sys.exit(f"!! the remote tip {remote_tip[:8]} is not the local HEAD "
                 f"{chain[0][:8]}; the histories have diverged, re-run with "
                 f"--force only if dropping the remote commits is intended")
    sys.exit(0)

print(f"{len(missing)} commit(s) to publish: "
      + ", ".join(s[:8] for s in missing))

if remote_tip not in chain and remote_tip not in missing:
    print(f"\n!! the remote tip {remote_tip[:8]} is not in the local history.")
    print("!! publishing would drop commits that only exist on the remote.")
    if "--force" not in sys.argv:
        sys.exit("!! refusing to move the branch ref; re-run with --force to override")

# --------------------------------------------------------------------------
# 2. create blobs and trees, then the commits themselves
# --------------------------------------------------------------------------
blob_cache = {}                         # local blob sha -> remote blob sha
published = []

for sha in missing:
    raw, header, message = parse_commit(sha)
    subject = sh("git", "log", "-1", "--format=%s", sha).strip()
    print(f"\n{sha[:8]}  {subject}")

    tree_items = []
    for line in sh("git", "ls-tree", "-r", sha).splitlines():
        meta, path = line.split("\t", 1)
        mode, otype, blob_sha = meta.split()
        assert otype == "blob", f"unexpected object type {otype} for {path}"

        if blob_sha not in blob_cache:
            if remote_blob(blob_sha):
                blob_cache[blob_sha] = blob_sha
            else:
                content = sh("git", "cat-file", "blob", blob_sha, binary=True)
                _, res = api("POST", f"/repos/{REPO}/git/blobs",
                             {"content": base64.b64encode(content).decode(),
                              "encoding": "base64"})
                blob_cache[blob_sha] = res["sha"]
        tree_items.append({"path": path, "mode": mode,
                           "type": "blob", "sha": blob_cache[blob_sha]})

    _, tree = api("POST", f"/repos/{REPO}/git/trees", {"tree": tree_items})
    if tree["sha"] != sh("git", "rev-parse", f"{sha}^{{tree}}").strip():
        sys.exit(f"!! tree mismatch for {sha[:8]}: {tree['sha']}")
    print(f"  tree {tree['sha']}  ({len(tree_items)} blobs, "
          f"{len(blob_cache)} unique)")

    parents = [ln.split()[1] for ln in header.splitlines()
               if ln.startswith("parent ")]
    author_line = next(ln for ln in header.splitlines() if ln.startswith("author "))
    committer_line = next(ln for ln in header.splitlines() if ln.startswith("committer "))

    def ident(line):
        m = re.match(r"^(?:author|committer) (.*) <(.*)> (\d+) ([+-]\d{4})$", line)
        if not m:
            sys.exit(f"cannot parse identity line: {line}")
        name, email, when, tz = m.groups()
        import datetime
        dt = datetime.datetime.fromtimestamp(int(when), datetime.timezone.utc)
        # GitHub wants ISO 8601; the offset is carried by the local object.
        off = datetime.timezone(datetime.timedelta(
            hours=int(tz[:3]), minutes=int(tz[0] + tz[3:5])))
        return {"name": name, "email": email,
                "date": dt.astimezone(off).strftime("%Y-%m-%dT%H:%M:%S") + tz[:3] + ":" + tz[3:]}

    _, commit = api("POST", f"/repos/{REPO}/git/commits",
                    {"message": message.decode("utf-8"),
                     "tree": tree["sha"],
                     "parents": parents,
                     "author": ident(author_line),
                     "committer": ident(committer_line)})
    print(f"  commit {commit['sha']}  parents={[p[:8] for p in parents] or '(root)'}")
    published.append((sha, commit["sha"], raw, header, message))

# --------------------------------------------------------------------------
# 3. move the branch ref
# --------------------------------------------------------------------------
new_tip = published[-1][1]
status, _ = api("POST", f"/repos/{REPO}/git/refs",
                {"ref": f"refs/heads/{BRANCH}", "sha": new_tip}, allow=(422,))
if status == 201:
    print(f"\ncreated refs/heads/{BRANCH} -> {new_tip}")
else:
    _, res = api("PATCH", f"/repos/{REPO}/git/refs/heads/{BRANCH}",
                 {"sha": new_tip, "force": True})
    print(f"\nupdated refs/heads/{BRANCH} -> {res['object']['sha']}")

# Read the ref back. The whole point of this script is to move it, so claiming
# success without checking is how a dropped connection turns into a silent
# no-op that the next run then mistakes for "already published".
_, ref_after = api("GET", f"/repos/{REPO}/git/ref/heads/{BRANCH}")
if ref_after["object"]["sha"] != new_tip:
    sys.exit(f"!! the ref update did not take: refs/heads/{BRANCH} is "
             f"{ref_after['object']['sha'][:8]}, expected {new_tip[:8]}")
print(f"verified refs/heads/{BRANCH} -> {new_tip}")

# --------------------------------------------------------------------------
# 4. put the local repository on the same SHAs
# --------------------------------------------------------------------------
# GitHub rewrites the message, so the recreated commit can hash differently.
# Rebuild the exact object it stored, write it locally, and move the branch.
# Reconstructing it beats a later `git reset --hard`: it keeps the local commit
# objects intact and makes the next `git push` a fast-forward.
def candidates(raw, header, message):
    """Byte-level variants of the commit object, in likelihood order."""
    head = header.encode() + b"\n\n"
    yield raw
    yield head + message.rstrip(b"\r\n")                       # trailing newline dropped
    yield head + message.replace(b"\r\n", b"\n")               # CRLF folded
    yield head + message.replace(b"\r\n", b"\n").rstrip(b"\r\n")
    yield head + message.replace(b"\r\n", b"\n").rstrip(b"\n") + b"\n"


ok = True
for local_sha, remote_sha, raw, header, message in published:
    if local_sha == remote_sha:
        print(f"  {local_sha[:8]} identical on both sides")
        continue

    match = None
    for cand in candidates(raw, header, message):
        h = subprocess.run(["git", "hash-object", "-t", "commit", "--stdin"],
                           input=cand, capture_output=True, check=True
                           ).stdout.decode().strip()
        if h == remote_sha:
            match = cand
            break

    if match is None:
        ok = False
        print(f"  !! {local_sha[:8]} -> remote {remote_sha[:8]}: could not "
              f"reconstruct the stored object")
        continue

    subprocess.run(["git", "hash-object", "-t", "commit", "-w", "--stdin"],
                   input=match, capture_output=True, check=True)
    sh("git", "update-ref", f"refs/heads/{BRANCH}", remote_sha)
    print(f"  {local_sha[:8]} -> {remote_sha[:8]}: local branch moved onto "
          f"the object GitHub stored")

# Drop any stale remote-tracking ref so `git status` reports the truth.
if sh("git", "rev-parse", "--verify", "--quiet", f"refs/remotes/origin/{BRANCH}",
      check=False).strip():
    sh("git", "update-ref", f"refs/remotes/origin/{BRANCH}", new_tip)

print("\nDONE")
print(f"local  HEAD = {sh('git', 'rev-parse', 'HEAD').strip()}")
print(f"remote HEAD = {new_tip}")
print(f"url = https://github.com/{REPO}")
if not ok:
    print("\nnote: at least one local commit was not reconciled with the remote.")
    print("the trees are identical, so `git fetch` + `git reset --hard "
          f"origin/{BRANCH}` settles it once github.com is reachable again.")
