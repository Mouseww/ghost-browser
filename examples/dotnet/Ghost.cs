// ---------------------------------------------------------------------------
//  Ghost.cs -- a dependency-free C# client for the ghost control protocol.
//
//  The protocol, in five lines
//  ---------------------------
//    * Transport : a byte-mode duplex Windows named pipe at \\.\pipe\ghost-<id>.
//    * Framing   : one UTF-8 JSON object per line, '\n' terminated, both ways.
//    * Request   : {"cmd": "<command>", ...command-specific fields}
//    * Response  : {"ok": true, ...fields}  or  {"ok": false, "error": "..."}
//    * One connection carries any number of sequential request/response pairs.
//      There are no request ids and no pipelining, so this client serialises
//      its requests behind a semaphore and reads exactly one line per request.
//
//  Check `ok`, not the presence of a field: a failed command always answers with
//  ok:false plus an `error` string, never by closing the pipe. The full command
//  list lives in docs/PROTOCOL.md.
//
//  Usage
//  -----
//      using Ghost;
//
//      using var ghost = new GhostClient("clientcheck-cs");   // \\.\pipe\ghost-clientcheck-cs
//      await ghost.ConnectAsync();          // retries for ~30 s, then throws
//
//      GhostResponse status = await ghost.StatusAsync();
//      Console.WriteLine(status.Ok);                        // True
//      Console.WriteLine(status.Pid);                       // 42652
//      Console.WriteLine(status.WindowTitle);               // Example Domain
//
//      await ghost.NavigateAsync("https://example.com");
//      GhostResponse found = await ghost.FindAsync(name: "Example Domain");
//      await ghost.ScreenshotAsync(@"C:\Temp\shot.bmp");
//      await ghost.ShutdownAsync();
//
//  Anything without a wrapper goes through the one generic entry point:
//
//      GhostResponse r = await ghost.SendAsync("scroll", new { delta = 600 });
//      JsonElement? window = r.Get("window");               // raw, dotted paths ok
//
//  Every wrapper is a thin alias for SendAsync(cmd, params). Nothing is validated
//  locally -- inspect Ok / Error / the raw JSON and decide.
//
//  Naming: the documented pipe is \\.\pipe\ghost-<id>. `ghost serve --pipe <name>`
//  overrides that name outright (the flag is not in PROTOCOL.md), so ConnectAsync
//  also tries \\.\pipe\<id> and reports the one it reached in PipePathUsed. Pass a
//  full \\.\pipe\... path instead of an id to name exactly one pipe.
//
//  Building and running the demo (against a live server):
//      ghost.exe serve --id clientcheck-cs --pipe clientcheck-cs https://example.com
//      dotnet run --project examples/dotnet -- --id clientcheck-cs
// ---------------------------------------------------------------------------

using System.IO.Pipes;
using System.Text;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace Ghost;

/// <summary>
/// Raised when the protocol itself fails: the pipe cannot be reached, the reply
/// is not a JSON object, a request times out, or <see cref="GhostResponse.EnsureOk"/>
/// is called on an <c>ok:false</c> reply. Carries the server's own error text.
/// </summary>
public sealed class GhostException : Exception
{
    /// <summary>The server's <c>error</c> field, when the failure came from the server.</summary>
    public string? Error { get; }

    /// <summary>The raw reply line, when there was one.</summary>
    public string? RawJson { get; }

    public GhostException(string message, string? error = null, string? rawJson = null, Exception? innerException = null)
        : base(message, innerException)
    {
        Error = error;
        RawJson = rawJson;
    }
}

/// <summary>
/// One parsed reply. <see cref="Json"/> and <see cref="RawJson"/> expose everything
/// the server sent; the typed helpers are conveniences over the same tree.
/// </summary>
public sealed class GhostResponse
{
    /// <summary>True when the server reported <c>"ok": true</c>.</summary>
    public bool Ok { get; }

    /// <summary>The server's human-readable failure reason, or null on success.</summary>
    public string? Error { get; }

    /// <summary>The whole reply as a detached <see cref="JsonElement"/>.</summary>
    public JsonElement Json { get; }

    /// <summary>The exact line the server sent, without the trailing newline.</summary>
    public string RawJson { get; }

    internal GhostResponse(JsonElement json, string rawJson)
    {
        Json = json;
        RawJson = rawJson;
        Ok = json.TryGetProperty("ok", out var ok) && ok.ValueKind == JsonValueKind.True;
        Error = json.TryGetProperty("error", out var err) && err.ValueKind == JsonValueKind.String
            ? err.GetString()
            : null;
    }

    /// <summary>Parses one reply line. Throws <see cref="GhostException"/> if it is not a JSON object.</summary>
    public static GhostResponse Parse(string rawJson)
    {
        JsonElement root;
        try
        {
            using var doc = JsonDocument.Parse(rawJson);
            root = doc.RootElement.Clone(); // clone so the document can be disposed
        }
        catch (JsonException ex)
        {
            throw new GhostException($"the server sent a line that is not JSON: {Truncate(rawJson)}",
                rawJson: rawJson, innerException: ex);
        }

        if (root.ValueKind != JsonValueKind.Object)
            throw new GhostException($"expected a JSON object, got {root.ValueKind}: {Truncate(rawJson)}",
                rawJson: rawJson);

        return new GhostResponse(root, rawJson);
    }

    /// <summary>
    /// Looks a field up by name; dots walk into nested objects
    /// (<c>Get("window.title")</c>). Returns null when the path does not exist.
    /// </summary>
    public JsonElement? Get(string path)
    {
        if (string.IsNullOrEmpty(path))
            return null;

        var current = Json;
        foreach (var part in path.Split('.'))
        {
            if (current.ValueKind != JsonValueKind.Object || !current.TryGetProperty(part, out var next))
                return null;
            current = next;
        }

        return current;
    }

    /// <summary>True when the path exists (even if its value is JSON null).</summary>
    public bool Has(string path) => Get(path) is not null;

    public string? GetString(string path) =>
        Get(path) is { } e && e.ValueKind == JsonValueKind.String ? e.GetString() : null;

    public bool? GetBool(string path) =>
        Get(path) is { } e
            ? e.ValueKind switch
            {
                JsonValueKind.True => true,
                JsonValueKind.False => false,
                _ => null,
            }
            : null;

    public int? GetInt(string path)
    {
        if (Get(path) is not { } e || e.ValueKind != JsonValueKind.Number)
            return null;
        if (e.TryGetInt32(out var i))
            return i;
        return e.TryGetDouble(out var d) ? (int)d : null;
    }

    public double? GetDouble(string path) =>
        Get(path) is { } e && e.ValueKind == JsonValueKind.Number && e.TryGetDouble(out var d) ? d : null;

    /// <summary>Enumerates an array field (<c>Items("nodes")</c>); empty when it is missing or not an array.</summary>
    public IEnumerable<JsonElement> Items(string path)
    {
        if (Get(path) is not { } e || e.ValueKind != JsonValueKind.Array)
            yield break;
        foreach (var item in e.EnumerateArray())
            yield return item;
    }

    // Handy aliases for the fields that show up in almost every session.
    public int? Pid => GetInt("pid");
    public string? Profile => GetString("profile");
    public string? Pipe => GetString("pipe");
    public string? WindowTitle => GetString("window.title");
    public string? WindowError => GetString("window_error");
    public int? Count => GetInt("count");

    /// <summary>Throws <see cref="GhostException"/> when the reply was <c>ok:false</c>.</summary>
    public GhostResponse EnsureOk()
    {
        if (!Ok)
            throw new GhostException($"ghost command failed: {Error ?? "no error field"}", Error, RawJson);
        return this;
    }

    public override string ToString() => RawJson;

    private static string Truncate(string s) => s.Length <= 200 ? s : s[..200] + "...";
}

/// <summary>
/// A connection to <c>\\.\pipe\ghost-&lt;id&gt;</c>. One instance is one pipe handle;
/// it stays open across requests and is closed by <see cref="Dispose"/>.
/// </summary>
public sealed class GhostClient : IDisposable, IAsyncDisposable
{
    /// <summary>How long <see cref="ConnectAsync"/> keeps retrying for a server that is still starting.</summary>
    public static readonly TimeSpan DefaultConnectTimeout = TimeSpan.FromSeconds(30);

    /// <summary>How long a single request waits for its reply before giving up.</summary>
    public static readonly TimeSpan DefaultResponseTimeout = TimeSpan.FromSeconds(60);

    private const string PipePrefix = @"\\.\pipe\";

    /// <summary>How long one connect attempt may wait before the next candidate is tried.</summary>
    private static readonly TimeSpan PerAttemptTimeout = TimeSpan.FromSeconds(2);

    private readonly string[] _candidateNames;
    private readonly TimeSpan _connectTimeout;
    private readonly TimeSpan _responseTimeout;
    private readonly SemaphoreSlim _gate = new(1, 1);
    private readonly byte[] _rx = new byte[8192];

    private int _rxLen;
    private int _rxPos;
    private NamedPipeClientStream? _pipe;
    private bool _disposed;

    /// <summary>The profile id this client speaks to.</summary>
    public string Id { get; }

    /// <summary>The documented pipe path, <c>\\.\pipe\ghost-&lt;id&gt;</c>.</summary>
    public string PipePath { get; }

    /// <summary>The pipe actually connected, once <see cref="ConnectAsync"/> has succeeded.</summary>
    public string? PipePathUsed { get; private set; }

    /// <summary>True once the pipe handle is open and the server has not closed it.</summary>
    public bool IsConnected => _pipe?.IsConnected == true;

    /// <param name="id">
    /// The profile id from <c>ghost serve --id &lt;id&gt;</c>, or a full
    /// <c>\\.\pipe\...</c> path if you would rather be explicit.
    /// </param>
    public GhostClient(string id = "default", TimeSpan? connectTimeout = null, TimeSpan? responseTimeout = null)
    {
        if (string.IsNullOrWhiteSpace(id))
            throw new ArgumentException("the profile id must not be empty", nameof(id));

        Id = id;
        PipePath = id.StartsWith(PipePrefix, StringComparison.OrdinalIgnoreCase) ? id : PipePrefix + "ghost-" + id;
        _candidateNames = BuildCandidateNames(id);
        _connectTimeout = connectTimeout ?? DefaultConnectTimeout;
        _responseTimeout = responseTimeout ?? DefaultResponseTimeout;
    }

    /// <summary>
    /// The pipe names to try, in order. The documented name is <c>ghost-&lt;id&gt;</c>.
    /// <c>ghost serve --pipe &lt;name&gt;</c> overrides the name completely (that flag is
    /// not in PROTOCOL.md), so the bare id is tried as well -- that is the exact shape
    /// the recommended verification command produces.
    /// </summary>
    private static string[] BuildCandidateNames(string id) =>
        id.StartsWith(PipePrefix, StringComparison.OrdinalIgnoreCase)
            ? new[] { id[PipePrefix.Length..] }   // an explicit path is taken literally
            : new[] { "ghost-" + id, id };

    /// <summary>Computes the pipe path a client would use for a profile id.</summary>
    public static string PipePathFor(string id) =>
        string.IsNullOrWhiteSpace(id) ? throw new ArgumentException("id must not be empty", nameof(id))
        : id.StartsWith(PipePrefix, StringComparison.OrdinalIgnoreCase) ? id
        : PipePrefix + "ghost-" + id;

    // -- connecting ---------------------------------------------------------

    /// <summary>
    /// Opens the pipe, retrying until <see cref="DefaultConnectTimeout"/> elapses.
    /// This is what makes a client safe to start alongside the server: the pipe may
    /// not exist yet, and that is normal rather than an error. Each attempt gets a
    /// short slice so that a wrong-but-plausible name cannot eat the whole budget.
    /// </summary>
    public async Task ConnectAsync(CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        if (_pipe?.IsConnected == true)
            return;

        var deadline = DateTime.UtcNow + _connectTimeout;
        Exception? lastError = null;
        var attemptIndex = 0;

        while (true)
        {
            cancellationToken.ThrowIfCancellationRequested();

            var remaining = deadline - DateTime.UtcNow;
            if (remaining <= TimeSpan.Zero)
                break;

            var name = _candidateNames[attemptIndex % _candidateNames.Length];
            attemptIndex++;

            var pipe = new NamedPipeClientStream(".", name, PipeDirection.InOut, PipeOptions.Asynchronous);
            try
            {
                using var attempt = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
                attempt.CancelAfter(remaining < PerAttemptTimeout ? remaining : PerAttemptTimeout);
                await pipe.ConnectAsync(attempt.Token).ConfigureAwait(false);

                _pipe = pipe;
                PipePathUsed = PipePrefix + name;
                _rxLen = 0;
                _rxPos = 0;
                return;
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                // ConnectAsync keeps waiting for a pipe that never appears, so a
                // cancelled slice just means "not this name, not yet".
                pipe.Dispose();
            }
            catch (Exception ex)
            {
                // Usually "the system cannot find the file specified": no server yet.
                pipe.Dispose();
                lastError = ex;
            }

            var pause = deadline - DateTime.UtcNow;
            if (pause <= TimeSpan.Zero)
                break;
            if (pause > TimeSpan.FromMilliseconds(250))
                pause = TimeSpan.FromMilliseconds(250);
            await Task.Delay(pause, cancellationToken).ConfigureAwait(false);
        }

        throw new GhostException(
            $"could not connect to {PipePath} (or {PipePrefix}{Id}) within " +
            $"{_connectTimeout.TotalSeconds:0.#}s (is `ghost serve --id {Id}` running?)",
            innerException: lastError);
    }

    /// <summary>Blocking form of <see cref="ConnectAsync"/> for callers that are not async.</summary>
    public void Connect() => ConnectAsync().GetAwaiter().GetResult();

    // -- the one generic request --------------------------------------------

    /// <summary>
    /// Sends one request and returns its reply.
    /// </summary>
    /// <param name="cmd">The command name, e.g. <c>"navigate"</c>.</param>
    /// <param name="parameters">
    /// Any of: null, an anonymous object (<c>new { url = "..." }</c>), a
    /// <c>Dictionary&lt;string, object?&gt;</c>, or a <see cref="JsonObject"/>. Its
    /// members are merged into the request next to <c>cmd</c>.
    /// </param>
    /// <exception cref="GhostException">Not connected, pipe closed, or no reply in time.</exception>
    public async Task<GhostResponse> SendAsync(string cmd, object? parameters = null,
        CancellationToken cancellationToken = default)
    {
        ThrowIfDisposed();
        if (string.IsNullOrWhiteSpace(cmd))
            throw new ArgumentException("cmd must not be empty", nameof(cmd));

        var request = new JsonObject { ["cmd"] = cmd };
        MergeParameters(request, parameters);
        var line = request.ToJsonString() + "\n";
        var bytes = Encoding.UTF8.GetBytes(line);

        // No request ids in the protocol, so only one request may be in flight.
        await _gate.WaitAsync(cancellationToken).ConfigureAwait(false);
        try
        {
            if (_pipe is null || !_pipe.IsConnected)
                throw new GhostException($"not connected to {PipePath}: call ConnectAsync() first");

            using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancellationToken);
            timeout.CancelAfter(_responseTimeout);

            try
            {
                await _pipe.WriteAsync(bytes.AsMemory(), timeout.Token).ConfigureAwait(false);
                await _pipe.FlushAsync(timeout.Token).ConfigureAwait(false);

                while (true)
                {
                    var reply = await ReadLineAsync(timeout.Token).ConfigureAwait(false);
                    if (reply is null)
                        throw new GhostException(
                            $"the server closed the pipe while waiting for the reply to \"{cmd}\"");
                    if (reply.Length == 0)
                        continue; // blank lines are skipped, never a reply
                    return GhostResponse.Parse(reply);
                }
            }
            catch (OperationCanceledException) when (!cancellationToken.IsCancellationRequested)
            {
                throw new GhostException(
                    $"timed out after {_responseTimeout.TotalSeconds:0.#}s waiting for the reply to \"{cmd}\"");
            }
        }
        finally
        {
            _gate.Release();
        }
    }

    /// <summary>Blocking form of <see cref="SendAsync"/>.</summary>
    public GhostResponse Send(string cmd, object? parameters = null) =>
        SendAsync(cmd, parameters).GetAwaiter().GetResult();

    // -- convenience wrappers ------------------------------------------------

    public Task<GhostResponse> StatusAsync(CancellationToken ct = default) => SendAsync("status", null, ct);
    public Task<GhostResponse> WindowsAsync(CancellationToken ct = default) => SendAsync("windows", null, ct);
    public Task<GhostResponse> FocusAsync(CancellationToken ct = default) => SendAsync("focus", null, ct);

    public Task<GhostResponse> NavigateAsync(string url, CancellationToken ct = default) =>
        SendAsync("navigate", Args(("url", url)), ct);

    public Task<GhostResponse> TreeAsync(int? maxDepth = null, int? maxNodes = null, bool? anonymous = null,
        CancellationToken ct = default) =>
        SendAsync("tree", Args(("max_depth", maxDepth), ("max_nodes", maxNodes), ("anonymous", anonymous)), ct);

    public Task<GhostResponse> FindAsync(string? role = null, string? name = null, int? maxDepth = null,
        int? maxNodes = null, CancellationToken ct = default) =>
        SendAsync("find", Args(("role", role), ("name", name), ("max_depth", maxDepth), ("max_nodes", maxNodes)), ct);

    /// <summary>Click an element by the <c>index</c> from the last tree/find.</summary>
    public Task<GhostResponse> ClickAsync(int index, string? button = null, int? count = null,
        CancellationToken ct = default) =>
        SendAsync("click", Args(("index", index), ("button", button), ("count", count)), ct);

    /// <summary>Click a point in absolute screen pixels.</summary>
    public Task<GhostResponse> ClickPointAsync(int x, int y, string? button = null, int? count = null,
        CancellationToken ct = default) =>
        SendAsync("click", Args(("x", x), ("y", y), ("button", button), ("count", count)), ct);

    /// <summary>Click the first element matching role and/or name, exactly like find.</summary>
    public Task<GhostResponse> ClickAsync(string role, string? name = null, string? button = null, int? count = null,
        CancellationToken ct = default) =>
        SendAsync("click", Args(("role", role), ("name", name), ("button", button), ("count", count)), ct);

    /// <summary>Types into whatever currently has focus -- click the field first.</summary>
    public Task<GhostResponse> TypeAsync(string text, CancellationToken ct = default) =>
        SendAsync("type", Args(("text", text)), ct);

    /// <summary>Presses one key, e.g. <c>"enter"</c>.</summary>
    public Task<GhostResponse> KeyAsync(string key, CancellationToken ct = default) =>
        SendAsync("key", Args(("key", key)), ct);

    /// <summary>Presses a combination together, e.g. <c>["ctrl", "shift", "t"]</c>.</summary>
    public Task<GhostResponse> KeyAsync(IEnumerable<string> keys, CancellationToken ct = default) =>
        SendAsync("key", Args(("keys", keys)), ct);

    public Task<GhostResponse> ScrollAsync(int? delta = null, CancellationToken ct = default) =>
        SendAsync("scroll", Args(("delta", delta)), ct);

    /// <summary>Captures the window to a BMP file. Pass null to use the server's default path.</summary>
    public Task<GhostResponse> ScreenshotAsync(string? path = null, CancellationToken ct = default) =>
        SendAsync("screenshot", Args(("path", path)), ct);

    /// <summary>Stops the server, and the browser it launched. The pipe closes after the reply.</summary>
    public Task<GhostResponse> ShutdownAsync(CancellationToken ct = default) => SendAsync("shutdown", null, ct);

    // -- internals -----------------------------------------------------------

    private async Task<string?> ReadLineAsync(CancellationToken cancellationToken)
    {
        var pipe = _pipe!;
        var line = new MemoryStream(256);

        while (true)
        {
            if (_rxPos >= _rxLen)
            {
                _rxLen = await pipe.ReadAsync(_rx.AsMemory(), cancellationToken).ConfigureAwait(false);
                _rxPos = 0;
                if (_rxLen == 0)
                    return line.Length == 0 ? null : Decode(line); // EOF
            }

            var newline = Array.IndexOf(_rx, (byte)'\n', _rxPos, _rxLen - _rxPos);
            if (newline >= 0)
            {
                line.Write(_rx, _rxPos, newline - _rxPos);
                _rxPos = newline + 1;
                return Decode(line);
            }

            line.Write(_rx, _rxPos, _rxLen - _rxPos);
            _rxPos = _rxLen;
        }
    }

    private static string Decode(MemoryStream line)
    {
        var text = Encoding.UTF8.GetString(line.GetBuffer(), 0, (int)line.Length);
        return text.EndsWith('\r') ? text[..^1] : text; // a client may write \r\n
    }

    private static void MergeParameters(JsonObject request, object? parameters)
    {
        switch (parameters)
        {
            case null:
                return;

            case JsonObject json:
                foreach (var pair in json)
                    request[pair.Key] = pair.Value?.DeepClone();
                return;

            case IDictionary<string, object?> map:
                foreach (var pair in map)
                    request[pair.Key] = ToNode(pair.Value);
                return;

            default:
                // Anonymous object, or anything else JsonSerializer understands.
                if (JsonSerializer.SerializeToNode(parameters) is JsonObject shaped)
                {
                    foreach (var pair in shaped)
                        request[pair.Key] = pair.Value?.DeepClone();
                }
                return;
        }
    }

    private static JsonNode? ToNode(object? value) =>
        value is null ? null : JsonSerializer.SerializeToNode(value);

    /// <summary>Builds a parameter map, dropping nulls so absent fields stay absent.</summary>
    private static Dictionary<string, object?> Args(params (string Key, object? Value)[] pairs)
    {
        var map = new Dictionary<string, object?>(pairs.Length, StringComparer.Ordinal);
        foreach (var (key, value) in pairs)
        {
            if (value is not null)
                map[key] = value;
        }

        return map;
    }

    private void ThrowIfDisposed()
    {
        if (_disposed)
            throw new ObjectDisposedException(nameof(GhostClient));
    }

    public void Dispose()
    {
        if (_disposed)
            return;
        _disposed = true;
        _pipe?.Dispose();
        _pipe = null;
        _gate.Dispose();
    }

    public async ValueTask DisposeAsync()
    {
        if (_disposed)
            return;
        _disposed = true;
        if (_pipe is not null)
            await _pipe.DisposeAsync().ConfigureAwait(false);
        _pipe = null;
        _gate.Dispose();
    }
}

/// <summary>
/// The demo. Talks to a real server: status, windows, navigate, find, screenshot,
/// shutdown, printing the raw reply of each so the transcript is self-evident.
/// </summary>
internal static class Program
{
    private static async Task<int> Main(string[] args)
    {
        var id = Environment.GetEnvironmentVariable("GHOST_ID") ?? "clientcheck-cs";
        var url = "https://example.com";
        string? pipeOverride = null;

        for (var i = 0; i < args.Length - 1; i++)
        {
            switch (args[i])
            {
                case "--id":
                    id = args[i + 1];
                    break;
                case "--pipe":
                    // `ghost serve --pipe <name>` sets the pipe name exactly, with no
                    // "ghost-" prefix, so take it literally.
                    pipeOverride = args[i + 1];
                    break;
                case "--url":
                    url = args[i + 1];
                    break;
            }
        }

        var target = pipeOverride is null ? id : @"\\.\pipe\" + pipeOverride;
        using var ghost = new GhostClient(target);
        Console.WriteLine($"connecting to {ghost.PipePath} (retries for 30 s while the server starts)...");

        try
        {
            await ghost.ConnectAsync();
        }
        catch (GhostException ex)
        {
            Console.Error.WriteLine($"FAILED: {ex.Message}");
            if (ex.InnerException is not null)
                Console.Error.WriteLine($"        last connect error: {ex.InnerException.Message}");
            return 2;
        }

        Console.WriteLine($"connected on {ghost.PipePathUsed}.");
        Console.WriteLine();

        // 1. status -- the one command that answers before the browser is up.
        var status = await ghost.StatusAsync();
        Print("status", status);
        if (!status.Ok)
            return 1;

        Console.WriteLine($"STATUS: ok={status.Ok} pid={status.Pid} profile={status.Profile} " +
                          $"title=\"{status.WindowTitle}\" pipe={status.Pipe}");
        if (status.WindowTitle is null)
            Console.WriteLine($"        (no window yet: {status.WindowError})");
        Console.WriteLine();

        // 2. windows -- every top-level window the browser process owns.
        var windows = await ghost.WindowsAsync();
        Print("windows", windows);
        foreach (var window in windows.Items("windows"))
            Console.WriteLine($"  window handle={window.GetProperty("handle").GetInt64()} " +
                              $"title=\"{window.GetProperty("title").GetString()}\" " +
                              $"class={window.GetProperty("class").GetString()}");
        Console.WriteLine();

        // 3. navigate -- a no-op here, the server was launched on this URL already,
        //    but it proves the round trip. Needs a foreground-capable session.
        Print("navigate", await ghost.NavigateAsync(url));

        // 4. find -- the accessibility tree, by name substring.
        var found = await ghost.FindAsync(name: "Example");
        Print("find name=Example", found);
        foreach (var node in found.Items("nodes"))
        {
            var role = node.TryGetProperty("role", out var r) ? r.GetString() : "?";
            var name = node.TryGetProperty("name", out var n) ? n.GetString() : "";
            var center = node.TryGetProperty("center_x", out var cx) && node.TryGetProperty("center_y", out var cy)
                ? $"center=({cx.GetInt32()},{cy.GetInt32()})"
                : "center=(none)";
            Console.WriteLine($"  node role={role} name=\"{name}\" {center}");
        }

        // 5. screenshot -- written by the server, read back here as proof it exists.
        var shot = Path.Combine(Path.GetTempPath(), "ghost-dotnet-demo.bmp");
        var screenshot = await ghost.ScreenshotAsync(shot);
        Print("screenshot", screenshot);
        if (screenshot.Ok)
        {
            var info = new FileInfo(screenshot.GetString("path") ?? shot);
            Console.WriteLine($"  file exists={info.Exists} bytes={info.Length} " +
                              $"{screenshot.GetInt("width")}x{screenshot.GetInt("height")}");
        }

        Console.WriteLine();
        Console.WriteLine("all commands answered on one pipe handle; shutting down.");
        var bye = await ghost.ShutdownAsync();
        Print("shutdown", bye);

        return status.Ok ? 0 : 1;
    }

    private static void Print(string label, GhostResponse response)
    {
        Console.WriteLine($"--- {label} ---");
        Console.WriteLine(response.RawJson);
        if (!response.Ok)
            Console.WriteLine($"  ! ok:false -- {response.Error}");
        Console.WriteLine();
    }
}
