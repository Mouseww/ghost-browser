# CI: what runs free, and what needs a paid runner

This note records what was actually verified about GitHub Actions limits, so the
Track B (source-level Chromium build) decision isn't re-litigated from memory.

## Verified facts

| Fact | Source |
| --- | --- |
| Larger runners are **always billed per-minute, even for public repositories** | [`github/docs` `content/actions/concepts/runners/larger-runners.md`](https://github.com/github/docs/blob/main/content/actions/concepts/runners/larger-runners.md) |
| A GitHub-hosted job is capped at **6 hours** | [`github/docs` `content/actions/reference/limits.md`](https://github.com/github/docs/blob/main/content/actions/reference/limits.md) |
| A **self-hosted** runner job is capped at **5 days** | same file |
| Standard hosted runner: 4 vCPU / 16 GB RAM / ~14 GB free disk | GitHub's published runner specs |

> The exact quote from `larger-runners.md`:
> "For both private and public repositories, when larger runners are in use,
> they will always be billed at the per-minute rate."

## Consequence

**Open-sourcing does not unlock build resources.** A free public repo gets the
same 4 vCPU / 16 GB / 14 GB runner as a private one. Chromium's own build
instructions ask for roughly 100 GB of free disk and a full build is a multi-hour
link-heavy job on 4 cores — the 6-hour ceiling plus the disk ceiling rule it out.
This is a hard constraint, not a tuning problem.

So the split is:

| Workload | Runner | Why |
| --- | --- | --- |
| Shim + launcher build, probe smoke test, profile/patch validation, release packaging | **GitHub-hosted** `windows-latest` / `ubuntu-latest` (free) | seconds to minutes; fits the 6 h cap with room to spare |
| Full Chromium build, `ccache`-warm incremental patch rebuilds | **Self-hosted** runner on a spot VM (32 vCPU / 128 GB / 200 GB SSD) | needs the 5-day cap and the disk |

## Recommended Track B setup

1. Register a spot VM as a self-hosted runner
   (`./config.sh --url ... --token ... --labels chromium-builder`).
2. Keep `ccache` (or `sccache`) on a **persistent** volume — that is what turns a
   patch-iteration rebuild from hours into minutes. A runner that is destroyed
   after each job throws that away, which is the single biggest cost mistake here.
3. Trigger the full build **only** on tags and a nightly schedule, never per
   commit. Per-commit CI should build the shim and run the smoke test only.
4. Have the self-hosted job publish the built binary as an artifact, and let a
   free `ubuntu-latest` job do the release packaging and checksum/manifest work.

## What CI deliberately does not do

`harness/run_detect.py` is **not** wired into CI. It launches a real browser,
needs an interactive desktop session, and (as of the current slice) is flaky —
roughly half of runs end in `NO REPORT RECEIVED` with no crash event, which looks
like a Chrome startup race rather than a defect in the shim. A flaky job that
people learn to ignore is worse than no job. Fix the race first, then add it.

The `windows-build-and-smoke` job covers the same ground deterministically: it
asserts the spoofed CPU count, memory, screen geometry, DPI, time zone and locale
actually arrive in a target process, with a **negative control** (probe run with
no shim) proving the assertions measure the shim rather than the runner.
