"""Tiny MCP stdio client for aether_mcp_server."""
import json, subprocess, sys, os

SERVER = os.environ.get("AETHER_MCP_SERVER") or os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "build_mcp", "mcp", "aether_mcp_server.exe")


class Mcp:
    def __init__(self, args=()):
        self.p = subprocess.Popen([SERVER, *args], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, encoding="utf-8")
        self.n = 0
        self.rpc("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "build", "version": "1"}})
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "method": "notifications/initialized"}) + "\n")
        self.p.stdin.flush()

    def rpc(self, method, params):
        self.n += 1
        self.p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": self.n, "method": method, "params": params}) + "\n")
        self.p.stdin.flush()
        line = self.p.stdout.readline()
        if not line:
            raise RuntimeError("server died: " + self.p.stderr.read())
        return json.loads(line)

    def call(self, name, args=None, quiet=False):
        r = self.rpc("tools/call", {"name": name, "arguments": args or {}})
        res = r.get("result", {})
        text = "".join(c.get("text", "") for c in res.get("content", []) if c.get("type") == "text")
        try:
            val = json.loads(text)
        except Exception:
            val = text
        if res.get("isError"):
            if not quiet:
                print("ERROR", name, val)
            return None
        return val

    def images(self, name, args):
        r = self.rpc("tools/call", {"name": name, "arguments": args})
        return [c for c in r["result"].get("content", []) if c.get("type") == "image"]

    def close(self):
        try:
            self.p.stdin.close()
            self.p.wait(timeout=10)
        except Exception:
            self.p.kill()


if __name__ == "__main__":
    m = Mcp()
    tools = m.rpc("tools/list", {})["result"]["tools"]
    print(len(tools), "tools")
    m.close()
