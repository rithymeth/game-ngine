#!/usr/bin/env python3
"""Smoke-test a Lanternwake single-file build in Chromium (requires Playwright)."""

import argparse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from threading import Thread

from playwright.sync_api import sync_playwright


def check_layout(page):
    boxes = page.evaluate("""() => {
        const rect = id => document.getElementById(id).getBoundingClientRect().toJSON();
        return { canvas: rect('canvas'), footerButton: rect('fullscreen'),
                 width: innerWidth, height: innerHeight };
    }""")
    canvas, button = boxes["canvas"], boxes["footerButton"]
    expected_width = min(boxes["width"], (boxes["height"] - 48) * 1.6)
    expected_height = expected_width / 1.6
    if (abs(canvas["width"] - expected_width) > 1 or abs(canvas["height"] - expected_height) > 1
            or canvas["top"] < -1 or canvas["bottom"] > boxes["height"] + 1):
        raise RuntimeError(f"Game canvas does not fit the viewport: {boxes}")
    if button["top"] < canvas["bottom"] - 1 or button["bottom"] > boxes["height"] + 1:
        raise RuntimeError(f"Fullscreen button is outside the visible footer: {boxes}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("html", type=Path, help="built Lanternwake index.html")
    parser.add_argument("--chromium", type=Path, help="optional path to a Chromium executable")
    args = parser.parse_args()
    html = args.html.read_bytes()

    class SingleFileHandler(BaseHTTPRequestHandler):
        def do_GET(self):
            if self.path == "/favicon.ico":
                self.send_response(204)
                self.end_headers()
            elif self.path == "/index.html":
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(html)))
                self.end_headers()
                self.wfile.write(html)
            else:
                self.send_error(404)

    with ThreadingHTTPServer(("127.0.0.1", 0), SingleFileHandler) as server:
        worker = Thread(target=server.serve_forever, daemon=True)
        worker.start()
        try:
            url = f"http://127.0.0.1:{server.server_port}"
            with sync_playwright() as playwright:
                browser = playwright.chromium.launch(executable_path=args.chromium)
                page = browser.new_page(viewport={"width": 1360, "height": 848})
                errors = []
                requests = []
                page.on("pageerror", lambda error: errors.append(str(error)))
                page.on("console", lambda message: errors.append(message.text) if message.type == "error" else None)
                page.on("request", lambda request: requests.append(request.url))
                page.on("requestfailed", lambda request: errors.append(f"Failed request: {request.url}"))
                page.goto(url + "/index.html")
                page.locator("#loading.hidden").wait_for(state="attached")
                for width, height in ((1360, 848), (915, 412), (390, 844)):
                    page.set_viewport_size({"width": width, "height": height})
                    check_layout(page)
                    print(f"Lanternwake viewport {width}x{height}: fits")
                if errors or set(requests) - {url + "/index.html", url + "/favicon.ico"}:
                    raise RuntimeError(f"Browser errors or unexpected requests: {errors}, {requests}")
                browser.close()
        finally:
            server.shutdown()
            worker.join()


if __name__ == "__main__":
    main()
