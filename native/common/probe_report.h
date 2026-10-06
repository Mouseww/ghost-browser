// The OS values the shim claims to spoof, read back through the real APIs.
//
// There is exactly one definition of "spoofed" on purpose, shared by two
// callers: tests/probe, a standalone executable for the build tree and CI, and
// ghost.exe's hidden `__probe` subcommand. The shipped single file therefore
// verifies itself rather than extracting a second unsigned executable to run,
// which is both a smaller payload and the pattern antivirus heuristics dislike
// most.
#pragma once

namespace ghost {

// Prints the report to stdout. Never fails; a value that cannot be read is
// simply omitted, and a missing line is what the assertions look for.
void probe_report();

}  // namespace ghost
