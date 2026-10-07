'use strict';

/**
 * ghost.js -- a dependency-free Node.js client for the ghost control protocol.
 *
 * The protocol in four lines: the server listens on the byte-mode duplex named
 * pipe `\\.\pipe\<name>` (name defaults to `ghost-<profile id>`, and
 * `ghost serve --pipe <name>` overrides it with a literal pipe name). Both
 * directions speak one UTF-8 JSON object per line, `\n`-terminated. A request is
 * `{"cmd": "...", ...params}`; the response is always a JSON object carrying
 * `ok: true` or `ok: false` plus `error`. One connection may carry any number of
 * sequential requests, and the server never closes the pipe or misaligns the
 * stream because a command failed -- so always branch on `ok`, not on whether a
 * field happens to be present.
 *
 * CommonJS, standard library only (`node:fs`). No package.json is required.
 *
 * Usage:
 *
 *   const ghost = require('./ghost');
 *
 *   (async () => {
 *     // One connection per call; waits up to 30 s for the pipe to appear.
 *     const st = await ghost.status();                 // pipe "ghost-default"
 *     console.log(st.pid, st.window && st.window.title);
 *
 *     const other = { pipe: 'clientcheck-node' };      // \\.\pipe\clientcheck-node
 *     await ghost.navigate('https://example.com', other);
 *     const hits = await ghost.find({ role: 'link', name: 'more' }, other);
 *     await ghost.click({ index: hits.nodes[0].index }, other);
 *     await ghost.type('hello', other);
 *     await ghost.screenshot('C:\\temp\\shot.bmp', other);
 *     await ghost.shutdown(other);
 *   })();
 *
 * Every wrapper takes an options object as its last argument:
 *   { pipe, timeoutMs, retryMs }
 * and `call` takes the command plus a params object:
 *   ghost.call('key', { keys: ['ctrl', 'shift', 't'] }, { pipe: 'default' })
 *
 * Note on reads: a read from a pipe cannot be given a position and cannot be
 * interrupted, so this client reads with a blocking `fs.readSync` loop that stops
 * at the first `\n`. That is safe here because the server answers every request or
 * closes the pipe -- there is no case in the protocol where it stays silent while
 * holding the connection open. The deadline below therefore bounds *connecting*
 * (the server may still be starting, or may be between pipe instances), not
 * reading.
 */

const fs = require('node:fs');

/** Pipe name used when the caller passes none: `ghost serve` with the default profile. */
const DEFAULT_PIPE = 'ghost-default';

/** How long `call()` keeps retrying a missing/busy pipe before giving up. */
const DEFAULT_TIMEOUT_MS = 30000;

/** Gap between connection attempts. */
const DEFAULT_RETRY_MS = 250;

/** Read granularity; responses are far smaller than this in practice. */
const READ_CHUNK = 64 * 1024;

/**
 * Errors worth retrying. ENOENT is the normal "server not up yet" answer, and it
 * also happens legitimately between two connections: the server tears its single
 * pipe instance down with DisconnectNamedPipe and creates the next one a moment
 * later, so a client that connects in that window sees the pipe missing.
 */
const RETRYABLE = new Set(['ENOENT', 'EBUSY', 'EACCES', 'EPERM', 'EAGAIN', 'ECONNREFUSED']);

const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** `default` -> `ghost-default`, the name `ghost serve` derives from the profile id. */
function pipeNameFor(id) {
  return `ghost-${id || 'default'}`;
}

/** `default` -> `\\.\pipe\ghost-default`. A name already containing a prefix is used as given. */
function pipePath(name) {
  const pipe = name || DEFAULT_PIPE;
  return pipe.startsWith('\\\\') ? pipe : `\\\\.\\pipe\\${pipe}`;
}

function isRetryable(err) {
  return Boolean(err) && RETRYABLE.has(err.code);
}

/**
 * Open the control pipe, retrying until `deadline`. Resolves to an fd.
 * `opts.pipe` is the literal pipe name; `opts.pipeId` is a profile id.
 */
async function connect(fdOpts = {}, deadline = Date.now() + DEFAULT_TIMEOUT_MS) {
  const name = fdOpts.pipe || (fdOpts.pipeId ? pipeNameFor(fdOpts.pipeId) : DEFAULT_PIPE);
  const target = pipePath(name);
  const retryMs = fdOpts.retryMs || DEFAULT_RETRY_MS;
  const timeoutMs = fdOpts.timeoutMs || DEFAULT_TIMEOUT_MS;
  const started = Date.now();

  for (;;) {
    try {
      // 'r+' is O_RDWR / OPEN_EXISTING, which is what a duplex pipe instance wants.
      return { fd: fs.openSync(target, 'r+'), name, target };
    } catch (err) {
      if (!isRetryable(err)) throw err;
      if (Date.now() >= deadline) {
        throw new Error(
          `no ghost control pipe at ${target} after ${timeoutMs} ms ` +
            `(last error: ${err.code || err.message}); is \`ghost serve --pipe ${name}\` running?`
        );
      }
      await sleep(Math.min(retryMs, Math.max(1, deadline - Date.now())));
    }
  }
}

/** Write every byte; a pipe write may be short. */
function writeAll(fd, buffer) {
  let offset = 0;
  while (offset < buffer.length) {
    const written = fs.writeSync(fd, buffer, offset, buffer.length - offset, null);
    if (!written) throw new Error('the control pipe accepted no bytes');
    offset += written;
  }
}

/**
 * Read exactly one non-blank `\n`-terminated line from the pipe. Blocking; see the
 * header note. Throws if the pipe closes before a line arrives.
 */
function readLine(fd) {
  let buffered = Buffer.alloc(0);
  const chunk = Buffer.alloc(READ_CHUNK);

  for (;;) {
    const newline = buffered.indexOf(0x0a);
    if (newline !== -1) {
      const line = buffered.subarray(0, newline).toString('utf8').replace(/\r$/, '');
      buffered = buffered.subarray(newline + 1);
      if (line.trim() === '') continue; // the protocol skips blank lines
      return line;
    }

    let read;
    try {
      // position must be null: a pipe has no seekable offset.
      read = fs.readSync(fd, chunk, 0, chunk.length, null);
    } catch (err) {
      if (buffered.length) break;
      throw new Error(`the control pipe closed while reading (${err.code || err.message})`);
    }
    if (read === 0) break; // EOF: the server closed its end
    buffered = Buffer.concat([buffered, chunk.subarray(0, read)]);
  }

  const tail = buffered.toString('utf8').replace(/\r$/, '');
  if (tail.trim() !== '') return tail;
  throw new Error('the control pipe closed before the server sent a response');
}

function parseResponse(line) {
  let parsed;
  try {
    parsed = JSON.parse(line);
  } catch {
    const shown = line.length > 200 ? `${line.slice(0, 200)}...` : line;
    throw new Error(`the server sent a line that is not JSON: ${shown}`);
  }
  if (parsed === null || typeof parsed !== 'object' || Array.isArray(parsed)) {
    throw new Error(`the server sent a JSON value that is not an object: ${line}`);
  }
  return parsed;
}

/**
 * A connection that stays open for many sequential requests, which is what the
 * protocol is designed for. Use it when you are issuing more than one command and
 * want to skip the connect/teardown (and the reconnect race) per call.
 */
class Connection {
  constructor(fd, name) {
    this._fd = fd;
    this.pipe = name;
    this.closed = false;
  }

  /** Send one request and resolve to the parsed response object. */
  async call(cmd, params) {
    if (this.closed) throw new Error('this connection is closed');
    if (!cmd) throw new Error('every request needs a "cmd"');
    const request = { cmd, ...(params || {}) };
    writeAll(this._fd, Buffer.from(`${JSON.stringify(request)}\n`, 'utf8'));
    return parseResponse(readLine(this._fd));
  }

  close() {
    if (this.closed) return;
    this.closed = true;
    try {
      fs.closeSync(this._fd);
    } catch {
      /* the server may already have closed its end */
    }
  }
}

/** Open a connection, waiting for the pipe to appear. Resolves to a `Connection`. */
async function open(opts = {}) {
  const deadline = Date.now() + (opts.timeoutMs || DEFAULT_TIMEOUT_MS);
  const { fd, name } = await connect(opts, deadline);
  return new Connection(fd, name);
}

/**
 * Run one command on a fresh connection and resolve to the parsed response
 * object. The response is returned as-is for both `ok: true` and `ok: false` --
 * check `response.ok` yourself, per the protocol.
 *
 *   call(cmd, params?, opts?) -> Promise<object>
 */
async function call(cmd, params, opts = {}) {
  if (!cmd) throw new Error('every request needs a "cmd"');
  const deadline = Date.now() + (opts.timeoutMs || DEFAULT_TIMEOUT_MS);
  const { fd } = await connect(opts, deadline);
  try {
    const request = { cmd, ...(params || {}) };
    writeAll(fd, Buffer.from(`${JSON.stringify(request)}\n`, 'utf8'));
    return parseResponse(readLine(fd));
  } finally {
    try {
      fs.closeSync(fd);
    } catch {
      /* already gone */
    }
  }
}

/** Like `call`, but rejects when the server answers `ok: false`. */
async function callOrThrow(cmd, params, opts) {
  const response = await call(cmd, params, opts);
  if (!response.ok) throw new Error(response.error || `${cmd} failed`);
  return response;
}

// --- convenience wrappers -------------------------------------------------
// Each forwards `opts` so a caller can point at a different pipe.

const status = (opts) => call('status', {}, opts);
const windows = (opts) => call('windows', {}, opts);
const focus = (opts) => call('focus', {}, opts);
const navigate = (url, opts) => call('navigate', { url }, opts);
const tree = (params, opts) => call('tree', params, opts);
const find = (params, opts) => call('find', params, opts);
const click = (params, opts) => call('click', params, opts);
const type = (text, opts) => call('type', { text }, opts);
const key = (keys, opts) => call('key', Array.isArray(keys) ? { keys } : { key: keys }, opts);
const scroll = (params, opts) => call('scroll', params, opts);
const screenshot = (path, opts) => call('screenshot', path ? { path } : {}, opts);
const shutdown = (opts) => call('shutdown', {}, opts);

module.exports = {
  DEFAULT_PIPE,
  DEFAULT_TIMEOUT_MS,
  pipeNameFor,
  pipePath,
  connect,
  open,
  Connection,
  call,
  callOrThrow,
  status,
  windows,
  focus,
  navigate,
  tree,
  find,
  click,
  type,
  key,
  scroll,
  screenshot,
  shutdown,
};
