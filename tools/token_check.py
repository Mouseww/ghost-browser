"""Acceptance for the token route: a solving service's token into a hidden field.

There is no key here and there is not going to be one, so the service is stood in
for by the same local server that hosts the page. That checks everything this
repository can get wrong -- reading the site key out of the document, the shape of
the request, the polling, the write into the field the page actually looks at, and
the submit -- and leaves the vendor's own accuracy to the vendor.

The page is a stand-in too, and deliberately so: the point of this route is that it
never needs the widget to be real, only the field the widget would have filled.

Run: python tools/token_check.py [--timeout 60]
"""

import argparse
import os
import pathlib
import shutil
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs

sys.stdout.reconfigure(encoding="utf-8", errors="replace")
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))

from ghost_client import Ghost, GhostError  # noqa: E402

API_KEY = "test-key-not-a-real-one"
SITE_KEY = "6Le-token-check-sitekey-0000000000"
TOKEN = "03AGdBq26-token-from-the-stand-in-service"

seen: dict = {}

PAGE = """<!doctype html>
<html><head><meta charset="utf-8"><title>Token Route Check</title></head>
<body>
<h1>Token Route Check</h1>
<form id="challenge" method="POST" action="/submit">
  <div class="g-recaptcha" data-sitekey="{sitekey}"></div>
  <textarea id="g-recaptcha-response" name="g-recaptcha-response"
            style="display:none"></textarea>
  <input type="submit" value="Submit">
</form>
<script>
window.___grecaptcha_cfg = {{clients: {{0: {{0: {{callback: function (token) {{
  window.__token_seen = token;
}}}}}}}}}};
</script>
</body></html>
""".format(sitekey=SITE_KEY)


class StandIn(BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def _reply(self, text, content_type="text/plain"):
        body = text.encode("utf-8")
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        if self.path.startswith("/res.php"):
            seen["polls"] = seen.get("polls", 0) + 1
            # The first poll is deliberately not ready: a service that answers
            # instantly is not the service this code has to survive.
            if seen["polls"] < 2:
                self._reply("CAPCHA_NOT_READY")
            else:
                self._reply(f"OK|{TOKEN}")
            return
        self._reply(PAGE, "text/html; charset=utf-8")

    def do_POST(self):
        length = int(self.headers.get("Content-Length") or 0)
        raw = self.rfile.read(length).decode("utf-8", "replace")
        if self.path.startswith("/in.php"):
            seen["submit"] = {k: v[0] for k, v in parse_qs(raw).items()}
            self._reply("OK|12345")
            return
        seen["posted"] = {k: v[0] for k, v in parse_qs(raw).items()}
        self._reply("accepted")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--timeout", type=float, default=60.0)
    args = parser.parse_args()

    server = ThreadingHTTPServer(("127.0.0.1", 0), StandIn)
    port = server.server_address[1]
    threading.Thread(target=server.serve_forever, daemon=True).start()
    page_url = f"http://127.0.0.1:{port}/"
    print(f"stand-in    {page_url}")

    os.environ["GHOST_CAPTCHA_URL"] = f"http://127.0.0.1:{port}"
    os.environ["GHOST_CAPTCHA_KEY"] = API_KEY

    profile = f"token-check-{os.getpid()}-{int(time.time())}"
    ghost = Ghost(profile)
    data_dir = None
    checks = 0
    failures: list[str] = []

    def check(name, ok, detail=""):
        nonlocal checks
        checks += 1
        if not ok:
            failures.append(name)
        print(f"[{'ok' if ok else 'FAIL'}] {name}" + (f": {detail}" if detail else ""))

    try:
        ghost.start(page_url)
        data_dir = ghost.data_dir()

        answer = ghost.call("captcha", action="solve-token",
                            timeout=int(args.timeout * 1000))
        print(f"answer      {answer}")

        submit = seen.get("submit") or {}
        check("the service was asked for a reCAPTCHA token",
              submit.get("method") == "userrecaptcha", f"method={submit.get('method')!r}")
        check("the request carried the configured key", submit.get("key") == API_KEY)
        check("the request carried the site key the page declared",
              submit.get("googlekey") == SITE_KEY, f"googlekey={submit.get('googlekey')!r}")
        check("the request carried the page URL",
              submit.get("pageurl") == page_url, f"pageurl={submit.get('pageurl')!r}")
        check("the not-ready answer was polled again", seen.get("polls", 0) >= 2,
              f"polls={seen.get('polls')}")

        check("the token was written into the hidden response field",
              (answer.get("fields_filled") or 0) >= 1,
              f"fields_filled={answer.get('fields_filled')!r}")
        check("the widget's own callback was called",
              answer.get("callback_called") is True,
              f"callback_called={answer.get('callback_called')!r}")
        check("the form was submitted", bool(seen.get("posted")),
              f"posted={seen.get('posted')!r}")
        check("the submitted form carried the service's token",
              (seen.get("posted") or {}).get("g-recaptcha-response") == TOKEN,
              f"field={(seen.get('posted') or {}).get('g-recaptcha-response')!r}")
        check("the answer is attributed to the service", answer.get("solved_by") == "api",
              f"solved_by={answer.get('solved_by')!r}")
        check("the site key came from the document", answer.get("site_key") == SITE_KEY,
              f"site_key={answer.get('site_key')!r}")
        check("no site key was invented", answer.get("ok") is True,
              f"error={answer.get('error')!r}")
    except GhostError as exc:
        check("the tier ran", False, str(exc))
    finally:
        try:
            ghost.stop()
        except Exception:  # noqa: BLE001 - shutdown best effort
            pass
        if data_dir:
            shutil.rmtree(data_dir, ignore_errors=True)
        server.shutdown()

    print(f"\n{checks} checks, {len(failures)} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
