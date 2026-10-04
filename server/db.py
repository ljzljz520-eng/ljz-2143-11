"""SQLite persistence for revisions, authorized devices and delivery state."""

from __future__ import annotations

import json
import os
import sqlite3
import threading
import time
from typing import Any, Optional

SCHEMA = """
CREATE TABLE IF NOT EXISTS resources (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    created_at  REAL NOT NULL
);

CREATE TABLE IF NOT EXISTS revisions (
    id              TEXT PRIMARY KEY,          -- rev-xxxx
    resource_id     INTEGER NOT NULL,
    seq             INTEGER NOT NULL,
    created_at      REAL NOT NULL,
    uploaded_name   TEXT NOT NULL,             -- as the admin uploaded it
    original_format TEXT NOT NULL,             -- sniffed, never from extension
    width           INTEGER NOT NULL,
    height          INTEGER NOT NULL,
    pixels          INTEGER NOT NULL,
    sha256          TEXT NOT NULL,
    size            INTEGER NOT NULL,
    original_path   TEXT NOT NULL,             -- 原件来源: kept forever
    png_path        TEXT,                      -- present iff transcode=server
    preview_path    TEXT,                      -- decoded preview (admin UI)
    width_cap       INTEGER NOT NULL,
    revoked         INTEGER NOT NULL DEFAULT 0
);

CREATE TABLE IF NOT EXISTS devices (
    device_id    TEXT PRIMARY KEY,
    name         TEXT NOT NULL,
    formats      TEXT NOT NULL DEFAULT '["png","jpeg"]',
    max_pixels   INTEGER NOT NULL DEFAULT 8300000,
    authorized   INTEGER NOT NULL DEFAULT 1,
    first_seen   REAL NOT NULL,
    last_seen    REAL NOT NULL
);

-- delivery state for every (revision, device) pair.
CREATE TABLE IF NOT EXISTS delivery (
    device_id       TEXT NOT NULL,
    revision_id     TEXT NOT NULL,
    state           TEXT NOT NULL,             -- pending/fetching/ready/
                                               -- showing/error/obsolete/
                                               -- revoked
    error_code      TEXT NOT NULL DEFAULT '',
    last_actual     TEXT NOT NULL DEFAULT '',
    updated_at      REAL NOT NULL,
    PRIMARY KEY (device_id, revision_id)
);

CREATE TABLE IF NOT EXISTS events (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    ts          REAL NOT NULL,
    device_id   TEXT NOT NULL DEFAULT '',
    kind        TEXT NOT NULL,
    revision_id TEXT NOT NULL DEFAULT '',
    detail      TEXT NOT NULL DEFAULT ''
);
"""

_db_lock = threading.RLock()


class DB:
    def __init__(self, path: str):
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        self.conn = sqlite3.connect(path, check_same_thread=False)
        self.conn.row_factory = sqlite3.Row
        self.conn.execute("PRAGMA journal_mode=WAL")
        self.conn.executescript(SCHEMA)
        self.conn.commit()

    def query(self, sql: str, args: tuple = ()) -> list[sqlite3.Row]:
        with _db_lock:
            cur = self.conn.execute(sql, args)
            return cur.fetchall()

    def query_one(self, sql: str, args: tuple = ()) -> Optional[sqlite3.Row]:
        with _db_lock:
            cur = self.conn.execute(sql, args)
            return cur.fetchone()

    def execute(self, sql: str, args: tuple = ()) -> sqlite3.Cursor:
        with _db_lock:
            cur = self.conn.execute(sql, args)
            self.conn.commit()
            return cur

    def event(self, kind: str, device_id: str = "", revision_id: str = "",
              detail: str = "") -> None:
        self.execute(
            "INSERT INTO events(ts, device_id, kind, revision_id, detail)"
            " VALUES (?,?,?,?,?)",
            (time.time(), device_id, kind, revision_id, detail),
        )

    # ----- delivery helpers ------------------------------------------

    def upsert_delivery(self, device_id: str, revision_id: str,
                        state: str, error_code: str = "",
                        actual: str = "",
                        is_latest: bool = False) -> None:
        """
        Insert/update a delivery row.  last_actual semantics:
          * always updated when the device reports THIS revision as actual;
          * for the latest row it additionally records what the device is
            currently painting while it drifts towards the wanted revision
            (web UI shows wanted vs actual side by side);
          * older rows keep the last revision they actually displayed.
        """
        set_actual = (actual == revision_id) or (
            is_latest and actual != "")
        if set_actual:
            self.execute(
                "INSERT INTO delivery(device_id, revision_id, state,"
                " error_code, last_actual, updated_at)"
                " VALUES (?,?,?,?,?,?)"
                " ON CONFLICT(device_id, revision_id) DO UPDATE SET"
                " state=excluded.state, error_code=excluded.error_code,"
                " last_actual=excluded.last_actual,"
                " updated_at=excluded.updated_at",
                (device_id, revision_id, state, error_code, actual,
                 time.time()),
            )
        else:
            self.execute(
                "INSERT INTO delivery(device_id, revision_id, state,"
                " error_code, last_actual, updated_at)"
                " VALUES (?,?,?,?, '', ?)"
                " ON CONFLICT(device_id, revision_id) DO UPDATE SET"
                " state=excluded.state, error_code=excluded.error_code,"
                " updated_at=excluded.updated_at",
                (device_id, revision_id, state, error_code, time.time()),
            )

    def latest_revision(self) -> Optional[sqlite3.Row]:
        return self.query_one(
            "SELECT * FROM revisions WHERE revoked=0 ORDER BY seq DESC LIMIT 1"
        )

    def revision(self, revision_id: str) -> Optional[sqlite3.Row]:
        return self.query_one(
            "SELECT * FROM revisions WHERE id=?", (revision_id,)
        )
