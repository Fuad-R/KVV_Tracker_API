import json
from http.server import BaseHTTPRequestHandler, HTTPServer
from pathlib import Path
from urllib.parse import parse_qs, urlparse

FIXTURE_DIR = Path(__file__).parent / "fixtures"


def load_json(name: str):
    return json.loads((FIXTURE_DIR / name).read_text(encoding="utf-8"))


class Handler(BaseHTTPRequestHandler):
    def _json(self, payload, status=200):
        body = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, format, *args):
        return

    def do_GET(self):
        parsed = urlparse(self.path)
        query = parse_qs(parsed.query)

        if parsed.path == "/health":
            return self._json({"status": "ok"})

        if parsed.path.endswith("/XSLT_STOPFINDER_REQUEST"):
            search = query.get("name_sf", [""])[0]
            if search.lower() == "knielingen":
                return self._json(load_json("search_response.json"))
            return self._json([])

        if parsed.path.endswith("/XSLT_DM_REQUEST"):
            stop_id = query.get("name_dm", [""])[0]
            if stop_id == "7000107":
                return self._json(load_json("departures_7000107.json"))
            return self._json(load_json("departures_default.json"))

        if parsed.path.endswith("/XML_ADDINFO_REQUEST"):
            stop_id = query.get("itdLPxx_selStop", [""])[0]
            if stop_id == "7000107":
                return self._json(load_json("notifs_7000107.json"))
            return self._json(load_json("notifs_default.json"))

        self._json({"error": "Not Found"}, status=404)


if __name__ == "__main__":
    server = HTTPServer(("0.0.0.0", 18080), Handler)
    server.serve_forever()
