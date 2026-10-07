'use strict';

/**
 * example.js -- drive a live `ghost serve` over the control pipe with ghost.js.
 *
 * Start a server first (a unique id keeps you off everyone else's pipe), then run
 * this. It retries the pipe for 30 s, so it is safe to launch immediately:
 *
 *   Start-Process -FilePath "E:\projects\unknowbrowser\native\build\bin\ghost.exe" `
 *     -ArgumentList 'serve --id myid --pipe myid https://example.com' -WindowStyle Hidden
 *   node E:\projects\unknowbrowser\examples\node\example.js myid
 *
 * Usage:
 *   node example.js [pipe-name | --id <profile-id>] [--navigate <url>] [--keep-alive]
 *
 * `--id foo` resolves to the pipe name `ghost serve --id foo` derives by itself,
 * `ghost-foo`. A bare positional argument is the literal pipe name, which is what
 * you need when the server was started with `--pipe <name>`. With neither, the
 * pipe comes from GHOST_PIPE or falls back to `ghost-default`.
 *
 * The demo runs the read-only half of the protocol (status, windows, find, tree,
 * screenshot) so it works even in a headless or disconnected session, then shuts
 * the server down. `--keep-alive` skips the shutdown; `--navigate <url>` adds the
 * one write command that is still useful without a real input session.
 */

const os = require('node:os');
const path = require('node:path');
const ghost = require('./ghost');

function parseArgs(argv) {
  const opts = { id: null, pipe: null, keepAlive: false, navigate: null };
  for (let i = 0; i < argv.length; i += 1) {
    const arg = argv[i];
    if (arg === '--keep-alive') opts.keepAlive = true;
    else if (arg === '--navigate') opts.navigate = argv[++i] || null;
    else if (arg === '--id') opts.id = argv[++i] || null;
    else if (arg.startsWith('--')) throw new Error(`unknown flag: ${arg}`);
    else opts.pipe = arg;
  }
  if (!opts.pipe) {
    opts.pipe = process.env.GHOST_PIPE || (opts.id ? ghost.pipeNameFor(opts.id) : ghost.DEFAULT_PIPE);
  }
  return opts;
}

function heading(label, response) {
  const verdict = response && response.ok === true ? 'ok' : 'ok:false';
  console.log(`\n=== ${label} -> ${verdict} ===`);
  if (!response || response.ok !== true) {
    console.log(`error: ${response ? response.error : '(no response)'}`);
  }
  return response;
}

async function main() {
  const opts = parseArgs(process.argv.slice(2));
  const call = { pipe: opts.pipe };

  console.log(`ghost.js example -- talking to ${ghost.pipePath(opts.pipe)}`);
  console.log('waiting up to 30 s for the pipe to appear...');

  const started = Date.now();
  const status = heading('status', await ghost.status(call));
  if (!status.ok) {
    console.log('\nthe server never became ready; nothing else to do.');
    process.exitCode = 1;
    return;
  }
  console.log(`connected in ${Date.now() - started} ms`);
  console.log(JSON.stringify(status, null, 2));

  const pid = status.pid;
  const title = status.window ? status.window.title : null;
  console.log(`\n  pid=${pid}  profile=${status.profile}  attached=${status.attached}`);
  console.log(`  window title=${JSON.stringify(title)}`);

  const windows = heading('windows', await ghost.windows(call));
  if (windows.ok) {
    for (const w of windows.windows) {
      console.log(`  handle=${w.handle} visible=${w.visible} ${w.width}x${w.height} ${JSON.stringify(w.title)}`);
    }
  }

  const found = heading('find { name: "Example" }', await ghost.find({ name: 'Example' }, call));
  if (found.ok) {
    console.log(`  ${found.count} match(es)`);
    for (const node of found.nodes.slice(0, 5)) {
      console.log(`  #${node.index} ${node.role} ${JSON.stringify(node.name)} at (${node.center_x},${node.center_y})`);
    }
  }

  const tree = heading('tree { max_depth: 3, max_nodes: 40 }', await ghost.tree({ max_depth: 3, max_nodes: 40 }, call));
  if (tree.ok) {
    console.log(`  ${tree.count} node(s), first five:`);
    for (const node of tree.nodes.slice(0, 5)) {
      console.log(`  #${node.index} d${node.depth} ${node.role} ${JSON.stringify(node.name)}`);
    }
  }

  const shot = path.join(os.tmpdir(), 'ghost-node-example.bmp');
  const image = heading(`screenshot { path: ${shot} }`, await ghost.screenshot(shot, call));
  if (image.ok) console.log(`  wrote ${image.path} (${image.width}x${image.height} BMP, captured through the OS)`);

  if (opts.navigate) {
    const moved = heading(`navigate { url: ${opts.navigate} }`, await ghost.navigate(opts.navigate, call));
    if (moved.ok) {
      console.log(`  navigated=${moved.navigated} title=${JSON.stringify(moved.title)}`);
      console.log('  (navigated:false is not a failure -- it only means the window title never changed)');
    }
  }

  if (opts.keepAlive) {
    console.log('\n--keep-alive: leaving the server running.');
  } else {
    heading('shutdown', await ghost.shutdown(call));
    console.log('\nserver stopped (and the browser it launched was closed with it).');
  }
}

main().catch((err) => {
  console.error(`\nexample failed: ${err.message}`);
  process.exitCode = 1;
});
