// Command ghostclient is a dependency-free Go client for the ghost browser
// control protocol (see docs/PROTOCOL.md).
//
// The whole protocol is four steps: open the byte-mode duplex named pipe
// \\.\pipe\ghost-<id>, write one JSON object followed by '\n', read one
// '\n'-terminated JSON object back with bufio.Reader.ReadString('\n'), and
// check the reply's "ok" field. Nothing else is required -- no CDP, no port,
// no injected script, standard library only (os.OpenFile + encoding/json).
//
// One connection carries any number of sequential requests: the server reads a
// line, answers it, and reads the next one. There are no request ids and no
// pipelining, so this client opens the pipe once and reuses it for every call.
// A failure is always a well-formed {"ok": false, "error": "..."} reply; the
// server never throws and never leaves the stream misaligned.
//
// The Client type below is self-contained and can be copied into any package.
// This file also runs as a program that demonstrates the protocol end to end:
//
//	ghost.exe serve --id demo https://example.com
//	go run . -id demo -timeout 30s
//	go run . -pipe clientcheck-go -tree 25 -screenshot shot.bmp
//
// Programmatic use:
//
//	c, err := ghostclient.Dial("demo", 30*time.Second)
//	if err != nil {
//		return err
//	}
//	defer c.Close()
//
//	status, err := c.Status()          // pid, pipe, profile, window.title, ...
//	_, err = c.Navigate("https://example.com")
//	nodes, err := c.Find("link", "more information")
//	_, err = c.ClickIndex(14)          // index from the last tree/find
//	_, err = c.Type("hello")
//	_, err = c.Screenshot("shot.bmp")
//	_, err = c.Shutdown()
package main

import (
	"bufio"
	"encoding/json"
	"errors"
	"flag"
	"fmt"
	"os"
	"strings"
	"time"
)

// pipePrefix is the Windows named-pipe namespace. The server creates
// \\.\pipe\ghost-<id>, where <id> is the profile id from `ghost serve --id <id>`
// and defaults to "default".
const pipePrefix = `\\.\pipe\`

// dialInterval is how often a not-yet-existing pipe is retried.
const dialInterval = 250 * time.Millisecond

// PipePath returns the pipe path the server listens on for a profile id, i.e.
// \\.\pipe\ghost-<id>. An empty id means the "default" profile.
func PipePath(id string) string {
	if id == "" {
		id = "default"
	}
	return pipePrefix + "ghost-" + id
}

// ServerError is a well-formed {"ok": false, "error": "..."} reply. The server
// reports failures this way instead of dropping the connection, so a caller can
// distinguish "the command was rejected" from "the transport broke".
type ServerError struct {
	Cmd     string // the command that was rejected
	Message string // the server's human-readable reason
}

func (e *ServerError) Error() string {
	return fmt.Sprintf("ghost %s: %s", e.Cmd, e.Message)
}

// Client is one open connection to a running `ghost serve`.
//
// It is not safe for concurrent use: the protocol is strictly one request then
// one response, so serialize calls or give each goroutine its own Client.
type Client struct {
	path string
	file *os.File
	rd   *bufio.Reader
}

// Dial connects to the pipe for a profile id (\\.\pipe\ghost-<id>), retrying
// until the server creates it or timeout elapses. The server takes many seconds
// to launch the browser, so a caller that expects to connect immediately should
// pass something like 30*time.Second. It never blocks forever: a timeout of
// zero or less means 30 seconds.
func Dial(id string, timeout time.Duration) (*Client, error) {
	return DialPipe(PipePath(id), timeout)
}

// DialPipe is Dial for an explicit pipe name, which is what `ghost serve --pipe
// <name>` uses. The name may be either the bare name ("myprofile", listening on
// \\.\pipe\myprofile) or the full path ("\\.\pipe\myprofile").
func DialPipe(name string, timeout time.Duration) (*Client, error) {
	path := name
	if !strings.HasPrefix(path, pipePrefix) {
		path = pipePrefix + path
	}
	if timeout <= 0 {
		timeout = 30 * time.Second
	}

	deadline := time.Now().Add(timeout)
	var lastErr error
	for {
		// A byte-mode duplex named pipe opens like a file. This also covers
		// ERROR_PIPE_BUSY, which surfaces as a plain error: retrying is the
		// simple portable answer to a pipe whose instances are all taken.
		f, err := os.OpenFile(path, os.O_RDWR, 0)
		if err == nil {
			return &Client{path: path, file: f, rd: bufio.NewReader(f)}, nil
		}
		lastErr = err
		if !time.Now().Before(deadline) {
			return nil, fmt.Errorf("ghost: %s is not available after %s (is `ghost serve` running?): %w",
				path, timeout, lastErr)
		}
		time.Sleep(dialInterval)
	}
}

// Path reports the pipe this client is connected to.
func (c *Client) Path() string { return c.path }

// Close closes the connection. The server simply sees the client disconnect and
// keeps running; call Shutdown to stop it.
func (c *Client) Close() error {
	if c.file == nil {
		return nil
	}
	err := c.file.Close()
	c.file = nil
	return err
}

// Call sends one request -- {"cmd": cmd, ...params} plus '\n' -- and returns the
// parsed reply.
//
// A reply with "ok": false is returned as a *ServerError carrying the server's
// message (the reply itself is returned too, so a caller that wants the raw
// fields still has them). Transport problems, malformed JSON and a closed
// connection come back as ordinary errors.
func (c *Client) Call(cmd string, params map[string]any) (map[string]any, error) {
	if c.file == nil {
		return nil, errors.New("ghost: client is closed")
	}

	req := make(map[string]any, len(params)+1)
	for k, v := range params {
		req[k] = v
	}
	req["cmd"] = cmd // the explicit cmd always wins

	line, err := json.Marshal(req)
	if err != nil {
		return nil, fmt.Errorf("ghost: encoding %q request: %w", cmd, err)
	}
	line = append(line, '\n')
	if _, err := c.file.Write(line); err != nil {
		return nil, fmt.Errorf("ghost: sending %q: %w", cmd, err)
	}

	raw, err := c.readLine()
	if err != nil {
		return nil, fmt.Errorf("ghost: reading reply to %q: %w", cmd, err)
	}
	var resp map[string]any
	if err := json.Unmarshal([]byte(raw), &resp); err != nil {
		return nil, fmt.Errorf("ghost: reply to %q is not a JSON object (%q): %w", cmd, raw, err)
	}

	if ok, _ := resp["ok"].(bool); !ok {
		msg, _ := resp["error"].(string)
		if msg == "" {
			msg = "server replied ok:false without an error message"
		}
		return resp, &ServerError{Cmd: cmd, Message: msg}
	}
	return resp, nil
}

// readLine returns the next non-blank line of the reply stream. The protocol
// skips blank lines and tolerates a trailing '\r'.
func (c *Client) readLine() (string, error) {
	for {
		line, err := c.rd.ReadString('\n')
		if err != nil {
			return "", err
		}
		line = strings.TrimRight(line, "\r\n")
		if line == "" {
			continue
		}
		return line, nil
	}
}

// --- convenience wrappers ----------------------------------------------------

// Status reports what is attached and where the window is. It is the one command
// that succeeds before the browser is up, so it is the right readiness poll.
func (c *Client) Status() (map[string]any, error) {
	return c.Call("status", nil)
}

// Navigate goes to url the way a person would: focus, Ctrl+L, type, Enter.
func (c *Client) Navigate(url string) (map[string]any, error) {
	return c.Call("navigate", map[string]any{"url": url})
}

// Find searches the accessibility tree by role (exact) and/or name (substring,
// case-insensitive); at least one must be non-empty. The returned nodes replace
// the cache that ClickIndex resolves against.
func (c *Client) Find(role, name string) (map[string]any, error) {
	p := map[string]any{}
	if role != "" {
		p["role"] = role
	}
	if name != "" {
		p["name"] = name
	}
	return c.Call("find", p)
}

// ClickPoint clicks absolute screen pixels.
func (c *Client) ClickPoint(x, y int) (map[string]any, error) {
	return c.Call("click", map[string]any{"x": x, "y": y})
}

// ClickIndex clicks an element index from the last Tree or Find.
func (c *Client) ClickIndex(index int) (map[string]any, error) {
	return c.Call("click", map[string]any{"index": index})
}

// ClickElement clicks the first element matching role and/or name, resolved the
// same way Find resolves it.
func (c *Client) ClickElement(role, name string) (map[string]any, error) {
	p := map[string]any{}
	if role != "" {
		p["role"] = role
	}
	if name != "" {
		p["name"] = name
	}
	return c.Call("click", p)
}

// Type types text into whatever currently has focus. Click the field first;
// there is deliberately no "type into element" form.
func (c *Client) Type(text string) (map[string]any, error) {
	return c.Call("type", map[string]any{"text": text})
}

// Screenshot captures the window to a BMP at path. An empty path uses the
// server default (%LOCALAPPDATA%\GhostBrowser\screenshot.bmp).
func (c *Client) Screenshot(path string) (map[string]any, error) {
	p := map[string]any{}
	if path != "" {
		p["path"] = path
	}
	return c.Call("screenshot", p)
}

// Shutdown stops the server and, when it launched the browser, closes it too.
func (c *Client) Shutdown() (map[string]any, error) {
	return c.Call("shutdown", nil)
}

// --- small helpers for reading replies ---------------------------------------

// Str returns a string field of a reply, or "" if absent or not a string.
func Str(m map[string]any, key string) string {
	s, _ := m[key].(string)
	return s
}

// Num returns a numeric field of a reply, or 0 if absent or not a number.
func Num(m map[string]any, key string) float64 {
	n, _ := m[key].(float64)
	return n
}

// Bool returns a boolean field of a reply, or false if absent or not a bool.
func Bool(m map[string]any, key string) bool {
	b, _ := m[key].(bool)
	return b
}

// --- demonstration -----------------------------------------------------------

func main() {
	id := flag.String("id", "default", `profile id; connects to \\.\pipe\ghost-<id>`)
	pipe := flag.String("pipe", "", "raw pipe name, for `ghost serve --pipe <name>` (overrides -id)")
	timeout := flag.Duration("timeout", 30*time.Second, "how long to keep retrying the pipe before giving up")
	url := flag.String("url", "", "if set, navigate here")
	tree := flag.Int("tree", 0, "if > 0, dump the accessibility tree capped at this many nodes")
	findRole := flag.String("find-role", "", "role to pass to find")
	findName := flag.String("find-name", "more information", "name substring to pass to find")
	clickName := flag.String("click-name", "", "if set, click the first element whose name contains this")
	shot := flag.String("screenshot", "", "if set, write a BMP screenshot to this path")
	shutdown := flag.Bool("shutdown", true, "send {\"cmd\":\"shutdown\"} before exiting")
	flag.Parse()

	c, err := dial(*id, *pipe, *timeout)
	if err != nil {
		fmt.Fprintf(os.Stderr, "FAIL: %v\n", err)
		os.Exit(1)
	}
	defer c.Close()
	fmt.Printf("connected to %s\n\n", c.Path())

	// status is the readiness proof: pid, pipe, profile and the window title.
	status, err := c.Status()
	if err != nil {
		fmt.Fprintf(os.Stderr, "FAIL: status: %v\n", err)
		os.Exit(1)
	}
	dump("status", status)
	if w, ok := status["window"].(map[string]any); ok {
		fmt.Printf("  => pid=%d attached=%v window=%q %gx%g scale=%g focused=%v\n\n",
			int(Num(status, "pid")), Bool(status, "attached"), Str(w, "title"),
			Num(w, "width"), Num(w, "height"), Num(w, "scale"), Bool(w, "focused"))
	} else {
		fmt.Printf("  => pid=%d attached=%v window_error=%q\n\n",
			int(Num(status, "pid")), Bool(status, "attached"), Str(status, "window_error"))
	}

	// The rest of the demo is best-effort: input synthesis needs a connected,
	// foreground-capable session, and this program should still show a status
	// report on a headless one.
	if *url != "" {
		if reply, err := c.Navigate(*url); err != nil {
			warn("navigate", err)
		} else {
			dump("navigate", reply)
			fmt.Printf("  => navigated=%v title=%q\n\n", Bool(reply, "navigated"), Str(reply, "title"))
		}
	}

	if *tree > 0 {
		// Plain Call, for a command with no wrapper: the envelope is the same.
		if reply, err := c.Call("tree", map[string]any{"max_nodes": *tree}); err != nil {
			warn("tree", err)
		} else {
			nodes, _ := reply["nodes"].([]any)
			fmt.Printf("tree: count=%d (max_nodes=%d)\n", int(Num(reply, "count")), *tree)
			for i, n := range nodes {
				if i >= 8 {
					fmt.Printf("  ... %d more\n", len(nodes)-8)
					break
				}
				m, _ := n.(map[string]any)
				fmt.Printf("  [%d] role=%q name=%q\n", int(Num(m, "index")), Str(m, "role"), Str(m, "name"))
			}
			fmt.Println()
		}
	}

	if *findRole != "" || *findName != "" {
		if reply, err := c.Find(*findRole, *findName); err != nil {
			warn("find", err)
		} else {
			dump("find", reply)
			fmt.Printf("  => count=%d\n\n", int(Num(reply, "count")))
		}
	}

	if *clickName != "" {
		// An unmatched name is the cleanest way to see the ok:false path: the
		// server answers with a *ServerError instead of dropping the pipe.
		if reply, err := c.ClickElement("", *clickName); err != nil {
			warn("click", err)
		} else {
			dump("click", reply)
			fmt.Printf("  => clicked at %g,%g\n\n", Num(reply, "x"), Num(reply, "y"))
		}
	}

	if *shot != "" {
		if reply, err := c.Screenshot(*shot); err != nil {
			warn("screenshot", err)
		} else {
			dump("screenshot", reply)
			fmt.Printf("  => wrote %q (%gx%g)\n\n", Str(reply, "path"), Num(reply, "width"), Num(reply, "height"))
		}
	}

	if *shutdown {
		reply, err := c.Shutdown()
		if err != nil {
			warn("shutdown", err)
		} else {
			dump("shutdown", reply)
		}
	}
	fmt.Println("done")
}

func dial(id, pipe string, timeout time.Duration) (*Client, error) {
	if pipe != "" {
		return DialPipe(pipe, timeout)
	}
	return Dial(id, timeout)
}

func dump(label string, reply map[string]any) {
	b, err := json.MarshalIndent(reply, "", "  ")
	if err != nil {
		fmt.Printf("%s: %v\n", label, reply)
		return
	}
	fmt.Printf("%s:\n%s\n", label, b)
}

func warn(label string, err error) {
	var se *ServerError
	if errors.As(err, &se) {
		fmt.Printf("%s: rejected by server: %s\n\n", label, se.Message)
		return
	}
	fmt.Printf("%s: %v\n\n", label, err)
}
