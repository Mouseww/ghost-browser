"""Build the body of a GitHub release from CHANGELOG.md plus a how-to-run block.

    python tools/release_notes.py v0.2.0 --out release-notes.md

Keeping this out of the workflow means the text can be reviewed locally before a
tag is pushed, which is the only moment it is cheap to fix.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

HOW_TO_RUN = """\
## How to run it

`ghost.exe` is the whole product. There is nothing to install and no runtime to
fetch: it locates an installed Chrome or Edge, invents a coherent hardware and
locale profile, and injects its own native layer into the browser before the
browser executes a single instruction of its own.

```powershell
# prove the spoofing works on this machine, without opening a browser
ghost.exe selftest

# report what it found: the browser, the profile directory, the host locale
ghost.exe doctor

# make a profile and browse with it
ghost.exe profile new --id work --tz Europe/London --locale en-GB
ghost.exe browse --id work https://example.com

# everything it accepts
ghost.exe help
```

Windows may refuse to run it. The launcher creates a suspended process, writes a
DLL path into it and starts a remote thread — which is exactly what an injector
does, so Defender's heuristic flags it as `Behavior:Win32/DefenseEvasion.A!ml`.
The binaries are not code-signed. Add an exclusion for the folder you keep it in,
or build from source and sign it yourself.
"""


def section_for(changelog: str, version: str) -> str | None:
    """The `## [version] ...` block, up to the next `## [` heading."""
    match = re.search(
        r"^##\s*\[" + re.escape(version) + r"\].*?(?=^##\s*\[|\Z)",
        changelog,
        re.MULTILINE | re.DOTALL,
    )
    return match.group(0).strip() if match else None


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("tag", help="release tag, e.g. v0.2.0")
    ap.add_argument("--changelog", type=Path, default=ROOT / "CHANGELOG.md")
    ap.add_argument("--out", type=Path, default=None,
                    help="write here; stdout when omitted")
    args = ap.parse_args()

    version = args.tag.lstrip("v")
    changelog = args.changelog.read_text(encoding="utf-8")
    body = section_for(changelog, version)
    if body is None:
        print(f"warning: no [{version}] section in {args.changelog.name}", file=sys.stderr)
        body = f"## {version}\n\nSee CHANGELOG.md for the details."

    notes = f"{body}\n\n{HOW_TO_RUN}"
    if args.out:
        # newline='\n' keeps the file byte-identical across platforms, which
        # matters because the workflow reads it back on Windows.
        args.out.write_text(notes, encoding="utf-8", newline="\n")
        print(f"wrote {args.out} ({len(notes)} chars)")
    else:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
        print(notes)
    return 0


if __name__ == "__main__":
    sys.exit(main())
