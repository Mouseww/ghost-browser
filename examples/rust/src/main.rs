//! A dependency-free Rust client for the **ghost** browser control protocol.
//!
//! `ghost serve` runs the browser and listens on a byte-mode Windows named pipe
//! (`\\.\pipe\ghost-<id>`; `--pipe <name>` sets that name outright). The entire
//! protocol is: write one JSON object and a `\n`, read one JSON object
//! terminated by `\n`. There is no request id and no pipelining — request *n* is
//! answered by response *n* — so a single connection carries any number of
//! sequential commands. A command that fails still answers, with
//! `{"ok": false, "error": "..."}`, and never desynchronizes the stream.
//!
//! Nothing here needs a crate. The envelope is flat, so instead of pulling in a
//! JSON library the file carries a small recursive-descent scanner (about 90
//! lines) that is exact about strings and escapes — a page title is free to
//! contain `"`, `:` and `\`, and the responses are the only untrusted input.
//!
//! ```no_run
//! use std::time::Duration;
//!
//! fn demo(client: &Client) -> Result<(), String> {
//!     // `status` is the one command that answers before the browser is up, so
//!     // it doubles as the readiness poll. The window can still be null.
//!     let status = client.status()?;
//!     println!("pid {:?} title {:?}", status.int("pid"), status.str("window.title"));
//!
//!     // Fields of nested objects are reached with a dotted path.
//!     println!("{}x{}", status.int("window.width").unwrap_or(0),
//!                       status.int("window.height").unwrap_or(0));
//!
//!     client.navigate("https://example.com")?;
//!
//!     // `find` replaces the index cache that `click`'s `index` form uses.
//!     let hits = client.find("", "Example")?;
//!     if let Some(node) = hits.item("nodes", 0) {
//!         let (x, y) = (node.int("center_x"), node.int("center_y"));
//!         if let (Some(x), Some(y)) = (x, y) {
//!             client.click(x, y)?;           // absolute screen pixels
//!         }
//!     }
//!     client.type_text("hello")?;            // named `type_text`: `type` is a keyword
//!     client.screenshot(Some(r"C:\Temp\shot.bmp"))?;
//!     client.shutdown()?;
//!     Ok(())
//! }
//! ```
//!
//! Run the demo with `cargo run`; it takes an optional pipe name, defaulting to
//! the one this example's server uses (`clientcheck-rs`).

use std::cell::RefCell;
use std::fs::{File, OpenOptions};
use std::io::{BufRead, BufReader, Write};
use std::process::ExitCode;
use std::time::{Duration, Instant};

// ---------------------------------------------------------------- transport

/// The full path of the control pipe named `name`: `\\.\pipe\<name>`.
///
/// `ghost serve --id X` names its pipe `ghost-X`; `ghost serve --pipe Y`
/// replaces that name with `Y`, prefix included.
pub fn pipe_path(name: &str) -> String {
    format!(r"\\.\pipe\{name}")
}

/// A connection to a running `ghost serve`.
///
/// One connection is a stream of request/response pairs, so the reader is kept
/// across calls. `call` takes `&self` (several methods can share one client),
/// which is why the buffered reader is behind a `RefCell`.
pub struct Client {
    pipe: String,
    reader: RefCell<BufReader<File>>,
    writer: File,
}

impl Client {
    /// Connect to the pipe named `name`, retrying until `timeout` expires.
    ///
    /// The pipe does not exist until the server has created it, and can report
    /// "busy" while it is being created, so a client started alongside `serve`
    /// waits rather than failing. It gives up with the last OS error instead of
    /// hanging forever.
    pub fn connect(name: &str, timeout: Duration) -> Result<Client, String> {
        let pipe = pipe_path(name);
        let deadline = Instant::now() + timeout;
        loop {
            match OpenOptions::new().read(true).write(true).open(&pipe) {
                Ok(file) => {
                    let reader = file
                        .try_clone()
                        .map_err(|e| format!("{pipe}: cannot duplicate the pipe handle: {e}"))?;
                    return Ok(Client {
                        pipe,
                        reader: RefCell::new(BufReader::new(reader)),
                        writer: file,
                    });
                }
                Err(e) => {
                    if Instant::now() >= deadline {
                        return Err(format!(
                            "cannot open {pipe} after {}s: {e}",
                            timeout.as_secs()
                        ));
                    }
                    std::thread::sleep(Duration::from_millis(200));
                }
            }
        }
    }

    /// The pipe this client is connected to.
    pub fn pipe(&self) -> &str {
        &self.pipe
    }

    /// Send one request whose parameter values are all JSON **strings**.
    ///
    /// This is the form every command that takes text uses:
    /// `call("navigate", &[("url", "https://example.com")])`.
    pub fn call(&self, cmd: &str, params: &[(&str, &str)]) -> Result<Response, String> {
        let quoted: Vec<(&str, String)> = params.iter().map(|(k, v)| (*k, quote(v))).collect();
        let raw: Vec<(&str, &str)> = quoted.iter().map(|(k, v)| (*k, v.as_str())).collect();
        self.call_raw(cmd, &raw)
    }

    /// Send one request whose parameter values are raw JSON text, so numbers
    /// and booleans go out unquoted:
    /// `call_raw("click", &[("x", "144"), ("y", "256")])`.
    ///
    /// Prefer [`Client::call`] for anything textual — a value that is not valid
    /// JSON is a malformed request, not an escaped string.
    pub fn call_raw(&self, cmd: &str, params: &[(&str, &str)]) -> Result<Response, String> {
        let mut line = String::with_capacity(32 + 24 * params.len());
        line.push_str("{\"cmd\":");
        line.push_str(&quote(cmd));
        for (k, v) in params {
            line.push(',');
            line.push_str(&quote(k));
            line.push(':');
            line.push_str(v);
        }
        line.push('}');
        self.exchange(&line)
    }

    /// Write one request line and read exactly one response line.
    fn exchange(&self, request: &str) -> Result<Response, String> {
        // `&File` is a `Write`, so one handle serves both directions.
        let mut writer = &self.writer;
        writer
            .write_all(request.as_bytes())
            .and_then(|()| writer.write_all(b"\n"))
            .and_then(|()| writer.flush())
            .map_err(|e| format!("writing to {}: {e}", self.pipe))?;

        let mut reader = self.reader.borrow_mut();
        loop {
            let mut line = String::new();
            let read = reader
                .read_line(&mut line)
                .map_err(|e| format!("reading from {}: {e}", self.pipe))?;
            if read == 0 {
                return Err(format!("{}: the server closed the pipe", self.pipe));
            }
            // The protocol skips blank lines in both directions.
            let line = line.trim();
            if line.is_empty() {
                continue;
            }
            return Response::parse(line);
        }
    }

    // ------------------------------------------------------------ commands
    // One method per command in docs/PROTOCOL.md. Each returns the raw
    // `Response`; use `Response::into_result` to turn `ok: false` into an `Err`.

    /// `status` — what is attached and where the window is. The only command
    /// that succeeds before the browser is up, so it is the readiness poll.
    pub fn status(&self) -> Result<Response, String> {
        self.call("status", &[])
    }

    /// `navigate` — focus the window, press Ctrl+L, type the URL, press Enter.
    /// `navigated` is false when the title did not change, which is not
    /// necessarily a failure; check `title`.
    pub fn navigate(&self, url: &str) -> Result<Response, String> {
        self.call("navigate", &[("url", url)])
    }

    /// `find` — search the accessibility tree. `name` is a case-insensitive
    /// substring; empty parameters are omitted, since the server requires at
    /// least one of `role` or `name`.
    pub fn find(&self, role: &str, name: &str) -> Result<Response, String> {
        let mut params: Vec<(&str, &str)> = Vec::new();
        if !role.is_empty() {
            params.push(("role", role));
        }
        if !name.is_empty() {
            params.push(("name", name));
        }
        self.call("find", &params)
    }

    /// `tree` — dump the accessibility tree. Like `find`, this replaces the
    /// cache that `click`'s `index` form resolves against.
    pub fn tree(&self) -> Result<Response, String> {
        self.call("tree", &[])
    }

    /// `click` at an absolute screen point, in screen pixels.
    pub fn click(&self, x: i64, y: i64) -> Result<Response, String> {
        let (x, y) = (x.to_string(), y.to_string());
        self.call_raw("click", &[("x", &x), ("y", &y)])
    }

    /// `click` an element from the last `tree`/`find` by its index.
    pub fn click_index(&self, index: i64) -> Result<Response, String> {
        self.call_raw("click", &[("index", &index.to_string())])
    }

    /// `type` — type literal text into whatever has focus. Named `type_text`
    /// because `type` is a Rust keyword. Click the target field first: the
    /// protocol has no "type into element" form on purpose.
    pub fn type_text(&self, text: &str) -> Result<Response, String> {
        self.call("type", &[("text", text)])
    }

    /// `key` — press one key, e.g. `"enter"` or `"escape"`.
    pub fn key(&self, key: &str) -> Result<Response, String> {
        self.call("key", &[("key", key)])
    }

    /// `screenshot` — capture the window to a BMP. `None` uses the server's
    /// default path. Works in any session, unlike the input commands.
    pub fn screenshot(&self, path: Option<&str>) -> Result<Response, String> {
        match path {
            Some(path) => self.call("screenshot", &[("path", path)]),
            None => self.call("screenshot", &[]),
        }
    }

    /// `shutdown` — stop the server, and the browser it launched with it.
    pub fn shutdown(&self) -> Result<Response, String> {
        self.call("shutdown", &[])
    }
}

// ----------------------------------------------------------------- envelope

/// One response object, parsed into the flat envelope the protocol uses.
pub struct Response {
    raw: String,
    value: Value,
}

impl Response {
    fn parse(line: &str) -> Result<Response, String> {
        let mut parser = Parser::new(line);
        let value = parser.value()?;
        parser.skip_ws();
        if parser.pos() != line.len() {
            return Err(format!(
                "trailing bytes after the response object: {line:?}"
            ));
        }
        Ok(Response {
            raw: line.to_string(),
            value,
        })
    }

    /// The response line exactly as the server sent it — the honest record of
    /// what happened, and what to log when a field is missing.
    pub fn raw(&self) -> &str {
        &self.raw
    }

    /// The `ok` field. `false` means the command failed; see [`Response::error`].
    pub fn ok(&self) -> bool {
        self.bool("ok").unwrap_or(false)
    }

    /// The `error` field, present when `ok` is false.
    pub fn error(&self) -> Option<&str> {
        self.str("error")
    }

    /// `Ok(self)` when the server reported success, `Err(reason)` when it did
    /// not — so a failed command can be propagated with `?`.
    pub fn into_result(self) -> Result<Response, String> {
        if self.ok() {
            Ok(self)
        } else {
            Err(self
                .error()
                .unwrap_or("the server reported failure without an error message")
                .to_string())
        }
    }

    /// Look up a field by name, or by a dotted path into a nested object such
    /// as `window.title` in the `status` response.
    pub fn field(&self, path: &str) -> Option<&Value> {
        let mut value = &self.value;
        for part in path.split('.') {
            value = value.field(part)?;
        }
        Some(value)
    }

    /// A string field.
    pub fn str(&self, path: &str) -> Option<&str> {
        self.field(path)?.as_str()
    }

    /// An integer field. The protocol's numbers are pids, pixel coordinates,
    /// counts and handles — all far inside the exact range of `f64`.
    pub fn int(&self, path: &str) -> Option<i64> {
        self.field(path)?.as_i64()
    }

    /// A boolean field.
    pub fn bool(&self, path: &str) -> Option<bool> {
        self.field(path)?.as_bool()
    }

    /// An array field, for `nodes` and `windows`. Each element is a `Value`
    /// that carries the same object accessors as a `Response`.
    pub fn array(&self, path: &str) -> Option<&[Value]> {
        match self.field(path)? {
            Value::Array(items) => Some(items),
            _ => None,
        }
    }

    /// One element of an array field: `item("nodes", 0)`.
    pub fn item(&self, path: &str, index: usize) -> Option<&Value> {
        self.array(path)?.get(index)
    }
}

/// A JSON value. Only the protocol's shapes are modelled, and numbers share one
/// `f64` representation so there is no int/float split to get wrong.
#[derive(Debug, Clone, PartialEq)]
pub enum Value {
    Null,
    Bool(bool),
    Num(f64),
    Str(String),
    Array(Vec<Value>),
    Object(Vec<(String, Value)>),
}

impl Value {
    /// The value of a member of this object, by exact key.
    pub fn field(&self, key: &str) -> Option<&Value> {
        match self {
            Value::Object(members) => members.iter().find(|(k, _)| k == key).map(|(_, v)| v),
            _ => None,
        }
    }

    /// The text of a string value.
    pub fn as_str(&self) -> Option<&str> {
        match self {
            Value::Str(s) => Some(s),
            _ => None,
        }
    }

    /// A number as an integer.
    pub fn as_i64(&self) -> Option<i64> {
        match self {
            Value::Num(n) => Some(*n as i64),
            _ => None,
        }
    }

    /// A boolean.
    pub fn as_bool(&self) -> Option<bool> {
        match self {
            Value::Bool(b) => Some(*b),
            _ => None,
        }
    }

    /// The elements of an array value.
    pub fn as_array(&self) -> Option<&[Value]> {
        match self {
            Value::Array(items) => Some(items),
            _ => None,
        }
    }
}

// ---------------------------------------------------------------- JSON scan

/// Encode a string as a JSON string literal. Bytes outside the escapes are
/// passed through as UTF-8, which JSON allows.
fn quote(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => out.push_str(&format!("\\u{:04x}", c as u32)),
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

/// A recursive-descent reader for the responses. It is small because the
/// protocol never sends anything exotic, and strict because the values it
/// decodes (window titles, element names) come from the web page.
struct Parser<'a> {
    bytes: &'a [u8],
    at: usize,
}

impl<'a> Parser<'a> {
    fn new(text: &'a str) -> Parser<'a> {
        Parser {
            bytes: text.as_bytes(),
            at: 0,
        }
    }

    fn pos(&self) -> usize {
        self.at
    }

    fn fail(&self, expected: &str) -> String {
        let rest = String::from_utf8_lossy(&self.bytes[self.at.min(self.bytes.len())..]);
        let rest: String = rest.chars().take(40).collect();
        format!("malformed JSON at byte {}: expected {expected}, found {rest:?}", self.at)
    }

    fn peek(&self) -> Option<u8> {
        self.bytes.get(self.at).copied()
    }

    fn skip_ws(&mut self) {
        while matches!(self.peek(), Some(b' ' | b'\t' | b'\n' | b'\r')) {
            self.at += 1;
        }
    }

    fn eat(&mut self, byte: u8) -> Result<(), String> {
        if self.peek() == Some(byte) {
            self.at += 1;
            Ok(())
        } else {
            Err(self.fail(&format!("'{}'", byte as char)))
        }
    }

    /// Parse one value of any type.
    fn value(&mut self) -> Result<Value, String> {
        self.skip_ws();
        match self.peek() {
            None => Err(self.fail("a value")),
            Some(b'{') => self.object(),
            Some(b'[') => self.array(),
            Some(b'"') => Ok(Value::Str(self.string()?)),
            Some(b't') => self.literal("true", Value::Bool(true)),
            Some(b'f') => self.literal("false", Value::Bool(false)),
            Some(b'n') => self.literal("null", Value::Null),
            Some(_) => self.number(),
        }
    }

    fn literal(&mut self, word: &str, value: Value) -> Result<Value, String> {
        if self.bytes[self.at..].starts_with(word.as_bytes()) {
            self.at += word.len();
            Ok(value)
        } else {
            Err(self.fail(word))
        }
    }

    fn number(&mut self) -> Result<Value, String> {
        let start = self.at;
        while matches!(
            self.peek(),
            Some(b'-' | b'+' | b'.' | b'e' | b'E' | b'0'..=b'9')
        ) {
            self.at += 1;
        }
        let text = std::str::from_utf8(&self.bytes[start..self.at])
            .map_err(|_| self.fail("a number"))?;
        text.parse::<f64>()
            .map(Value::Num)
            .map_err(|_| format!("malformed JSON at byte {start}: {text:?} is not a number"))
    }

    fn object(&mut self) -> Result<Value, String> {
        self.eat(b'{')?;
        let mut members = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b'}') {
            self.at += 1;
            return Ok(Value::Object(members));
        }
        loop {
            self.skip_ws();
            let key = self.string()?;
            self.skip_ws();
            self.eat(b':')?;
            let value = self.value()?;
            members.push((key, value));
            self.skip_ws();
            match self.peek() {
                Some(b',') => self.at += 1,
                Some(b'}') => {
                    self.at += 1;
                    return Ok(Value::Object(members));
                }
                _ => return Err(self.fail("',' or '}'")),
            }
        }
    }

    fn array(&mut self) -> Result<Value, String> {
        self.eat(b'[')?;
        let mut items = Vec::new();
        self.skip_ws();
        if self.peek() == Some(b']') {
            self.at += 1;
            return Ok(Value::Array(items));
        }
        loop {
            items.push(self.value()?);
            self.skip_ws();
            match self.peek() {
                Some(b',') => self.at += 1,
                Some(b']') => {
                    self.at += 1;
                    return Ok(Value::Array(items));
                }
                _ => return Err(self.fail("',' or ']'")),
            }
        }
    }

    /// A string literal, with escapes decoded — including surrogate pairs, so a
    /// title containing an emoji survives.
    fn string(&mut self) -> Result<String, String> {
        self.eat(b'"')?;
        let mut out: Vec<u8> = Vec::new();
        loop {
            let byte = self.peek().ok_or_else(|| self.fail("a closing '\"'"))?;
            self.at += 1;
            match byte {
                b'"' => {
                    return String::from_utf8(out).map_err(|_| self.fail("valid UTF-8"));
                }
                b'\\' => {
                    let escape = self.peek().ok_or_else(|| self.fail("an escape"))?;
                    self.at += 1;
                    match escape {
                        b'"' => out.push(b'"'),
                        b'\\' => out.push(b'\\'),
                        b'/' => out.push(b'/'),
                        b'b' => out.push(0x08),
                        b'f' => out.push(0x0c),
                        b'n' => out.push(b'\n'),
                        b'r' => out.push(b'\r'),
                        b't' => out.push(b'\t'),
                        b'u' => {
                            let high = self.hex4()?;
                            let code = if (0xd800..0xdc00).contains(&high) {
                                if self.peek() == Some(b'\\')
                                    && self.bytes.get(self.at + 1) == Some(&b'u')
                                {
                                    self.at += 2;
                                    let low = self.hex4()?;
                                    0x10000 + ((high - 0xd800) << 10) + (low - 0xdc00)
                                } else {
                                    return Err(self.fail("a low surrogate after \\u"));
                                }
                            } else {
                                high
                            };
                            let c = char::from_u32(code)
                                .ok_or_else(|| self.fail("a Unicode scalar value"))?;
                            let mut buf = [0u8; 4];
                            out.extend_from_slice(c.encode_utf8(&mut buf).as_bytes());
                        }
                        _ => return Err(self.fail("a valid escape")),
                    }
                }
                _ => out.push(byte),
            }
        }
    }

    /// The four hex digits of a `\uXXXX` escape.
    fn hex4(&mut self) -> Result<u32, String> {
        let end = self.at + 4;
        if end > self.bytes.len() {
            return Err(self.fail("four hex digits"));
        }
        let text =
            std::str::from_utf8(&self.bytes[self.at..end]).map_err(|_| self.fail("four hex digits"))?;
        let code = u32::from_str_radix(text, 16).map_err(|_| self.fail("four hex digits"))?;
        self.at = end;
        Ok(code)
    }
}

// --------------------------------------------------------------------- demo

/// The pipe this demo connects to by default: the name its server is started
/// with (`ghost serve --id clientcheck-rs --pipe clientcheck-rs ...`). Pass
/// another name as the first argument to talk to a different server.
const DEFAULT_PIPE: &str = "clientcheck-rs";

fn main() -> ExitCode {
    let name = std::env::args().nth(1).unwrap_or_else(|| DEFAULT_PIPE.to_string());
    let path = pipe_path(&name);
    println!("connecting to {path} (retrying for up to 30s while the server starts)");

    let client = match Client::connect(&name, Duration::from_secs(30)) {
        Ok(client) => client,
        Err(e) => {
            eprintln!("connect failed: {e}");
            return ExitCode::FAILURE;
        }
    };
    println!("connected to {}\n", client.pipe());

    // Each step prints the response line verbatim, so the output is the proof.
    // A command that fails (`ok: false`) is reported and the demo continues:
    // on a disconnected or headless session the input commands fail up front by
    // design, while status/find/screenshot still work.
    let mut failures = 0;

    match client.status() {
        Ok(r) => {
            println!("status    -> {}", r.raw());
            println!(
                "            pid={:?} profile={:?} attached={:?}",
                r.int("pid"),
                r.str("profile"),
                r.bool("attached")
            );
            println!(
                "            window={:?} {:?}x{:?} focused={:?} tree_nodes={:?}",
                r.str("window.title"),
                r.int("window.width"),
                r.int("window.height"),
                r.bool("window.focused"),
                r.int("tree_nodes")
            );
            if let Some(e) = r.error() {
                println!("            window_error={e:?}");
                failures += 1;
            }
        }
        Err(e) => {
            eprintln!("status failed: {e}");
            return ExitCode::FAILURE;
        }
    }

    // `navigated: false` only means the title did not change within 20s, which
    // is expected when the page is already open — so print it, do not judge it.
    if let Some(r) = step(client.navigate("https://example.com"), "navigate", &mut failures) {
        println!(
            "            navigated={:?} title={:?}",
            r.bool("navigated"),
            r.str("title")
        );
    }

    // `tree` replaces the same index cache `find` does; its response can be
    // hundreds of nodes, so only the count is printed.
    match client.tree() {
        Ok(r) => {
            println!("tree      -> ok={:?} count={:?}", r.ok(), r.int("count"));
            if let Some(node) = r.item("nodes", 0) {
                println!(
                    "            root: role={:?} name={:?}",
                    node.field("role").and_then(Value::as_str),
                    node.field("name").and_then(Value::as_str),
                );
            }
        }
        Err(e) => {
            eprintln!("tree failed: {e}");
            return ExitCode::FAILURE;
        }
    }

    match client.find("", "Example") {
        Ok(r) => {
            println!("find      -> {}", r.raw());
            println!("            count={:?}", r.int("count"));
            if let Some(node) = r.item("nodes", 0) {
                println!(
                    "            first: role={:?} name={:?} center=({:?},{:?})",
                    node.field("role").and_then(Value::as_str),
                    node.field("name").and_then(Value::as_str),
                    node.field("center_x").and_then(Value::as_i64),
                    node.field("center_y").and_then(Value::as_i64),
                );
                // Click whatever the find matched, at its centre in screen pixels.
                if let (Some(x), Some(y)) = (
                    node.field("center_x").and_then(Value::as_i64),
                    node.field("center_y").and_then(Value::as_i64),
                ) {
                    let _ = step(client.click(x, y), "click", &mut failures);
                }
            } else {
                println!("            (nothing matched)");
            }
        }
        Err(e) => {
            eprintln!("find failed: {e}");
            return ExitCode::FAILURE;
        }
    }

    // Types into whatever has focus. example.com has no text field, so this
    // proves the round trip rather than a visible effect.
    let _ = step(client.type_text("ghost"), "type", &mut failures);

    // One keystroke, to show the single-key form of `key`.
    let _ = step(client.key("escape"), "key", &mut failures);

    let shot = std::env::temp_dir().join("ghost-rust-client.bmp");
    match client.screenshot(Some(&shot.to_string_lossy())) {
        Ok(r) => {
            println!("screenshot-> {}", r.raw());
            println!(
                "            path={:?} {}x{}",
                r.str("path"),
                r.int("width").unwrap_or(0),
                r.int("height").unwrap_or(0)
            );
            if let Some(p) = r.str("path") {
                match std::fs::metadata(p) {
                    Ok(m) => println!("            {} bytes on disk", m.len()),
                    Err(e) => println!("            (cannot stat the file: {e})"),
                }
            }
        }
        Err(e) => {
            eprintln!("screenshot failed: {e}");
            return ExitCode::FAILURE;
        }
    }

    // Last: shutdown stops the server and closes the browser it launched. The
    // reply can race with the server tearing the pipe down, so a read error
    // here is not a failure.
    match client.shutdown() {
        Ok(r) => println!("shutdown  -> {}", r.raw()),
        Err(e) => println!("shutdown  -> (no reply, which is expected: {e})"),
    }

    if failures > 0 {
        println!("\n{failures} command(s) failed; see the ok/error fields above.");
    }
    ExitCode::SUCCESS
}

/// Run one command and print it the same way every time, returning the
/// response so the caller can print the fields it cares about.
fn step(result: Result<Response, String>, label: &str, failures: &mut usize) -> Option<Response> {
    match result {
        Ok(r) => {
            println!("{label:<10}-> {}", r.raw());
            if !r.ok() {
                *failures += 1;
            }
            Some(r)
        }
        Err(e) => {
            println!("{label:<10}-> transport error: {e}");
            *failures += 1;
            None
        }
    }
}
