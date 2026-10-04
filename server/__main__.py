"""Entry point: python3 -m server [--host 0.0.0.0] [--port 8080]"""

from __future__ import annotations

import argparse
import os
import signal

from .app import build_server


def main() -> None:
    parser = argparse.ArgumentParser(description="Background publisher server")
    parser.add_argument("--host",
                        default=os.environ.get("BG_HOST", "0.0.0.0"))
    parser.add_argument("--port", type=int,
                        default=int(os.environ.get("BG_PORT", "8080")))
    parser.add_argument("--db",
                        default=os.environ.get("BG_DB", "data/publisher.db"))
    parser.add_argument("--data-dir",
                        default=os.environ.get("BG_DATA_DIR", "data"))
    args = parser.parse_args()

    httpd = build_server(args.host, args.port, args.db, args.data_dir)
    print(f"background publisher listening on http://{args.host}:{args.port}")
    print("web admin: /   (upload, preview, dispatch, revoke, devices)")

    def stop(*_):
        httpd.shutdown()

    signal.signal(signal.SIGTERM, stop)
    signal.signal(signal.SIGINT, stop)
    try:
        httpd.serve_forever()
    finally:
        httpd.server_close()


if __name__ == "__main__":
    main()
