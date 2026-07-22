#!/usr/bin/env python3
"""
Process SMS command rows from zbxalerts.decoded_sms and insert WeChat chatroom mappings.

Flow:
1. Read pending rows from decoded_sms where send_status == 0.
2. Parse command in the format: _smsRoute <nick_name>
3. Query wxid from local HTTP API: http://127.0.0.1:8080/api/execSql
4. Insert into wzwmonitor.wechat_chatroom(wxid, nickname, chatroom_type, tenantname=wxid)
5. Mark source row as processed.
"""

from __future__ import annotations

import argparse
import json
import logging
import shlex
import sys
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Dict, Iterable, List, Optional, Sequence, Set

try:
    import pymysql
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "Missing dependency: pymysql. Install with: pip install pymysql"
    ) from exc


LOGGER = logging.getLogger("sms_route_sync")

SOURCE_ID_COLUMN = "id"
SOURCE_MESSAGE_COLUMN = "message_content"
SOURCE_STATUS_COLUMN = "send_status"
SOURCE_SENDER_COLUMN = "sender"


@dataclass
class AppConfig:
    """Runtime configuration resolved from CLI args and optional config.ini."""

    db_host: str
    db_port: int
    db_user: str
    db_password: str
    source_schema: str
    source_table: str
    target_schema: str
    target_table: str
    api_url: str
    wechat_db_name: str
    batch_size: int
    http_timeout: float
    success_status: int
    error_status: int
    chatroom_type: str
    sender_column: Optional[str]
    allowed_senders: Set[str]


def setup_logging(level_name: str) -> None:
    """Configure global logging format and level.

    Args:
        level_name: One of DEBUG/INFO/WARNING/ERROR/CRITICAL (case-insensitive).
    """
    level = getattr(logging, level_name.upper(), logging.INFO)
    logging.basicConfig(
        level=level,
        format="%(asctime)s %(levelname)s [%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )


def parse_flat_ini(path: str) -> Dict[str, str]:
    """Parse a flat key=value ini-like file without sections.

    Args:
        path: File path to read.

    Returns:
        Dict of parsed key/value pairs. Returns empty dict when file does not exist.
    """
    values: Dict[str, str] = {}
    try:
        with open(path, "r", encoding="utf-8") as f:
            for raw in f:
                line = raw.strip()
                if not line or line.startswith("#") or line.startswith(";"):
                    continue
                if "=" not in line:
                    continue
                key, value = line.split("=", 1)
                values[key.strip()] = value.strip()
    except FileNotFoundError:
        LOGGER.debug("Config file not found: %s", path)
        return values
    return values


def quote_sql(value: str) -> str:
    """Escape a Python string for use inside a single-quoted SQL literal."""
    # Minimal SQL string escaping for single-quoted literals.
    return value.replace("\\", "\\\\").replace("'", "''")


def find_first_existing_column(
    conn: pymysql.connections.Connection,
    schema: str,
    table: str,
    candidates: Sequence[str],
) -> Optional[str]:
    """Return the first matching column that exists in the target table.

    Args:
        conn: MySQL connection.
        schema: Table schema.
        table: Table name.
        candidates: Column names in priority order.

    Returns:
        First existing column name, or None when none exist.
    """
    placeholders = ",".join(["%s"] * len(candidates))
    sql = (
        "SELECT column_name "
        "FROM information_schema.columns "
        "WHERE table_schema=%s AND table_name=%s AND column_name IN ("
        + placeholders
        + ")"
    )
    with conn.cursor() as cur:
        cur.execute(sql, [schema, table, *candidates])
        found = {row["column_name"] for row in cur.fetchall()}
    for c in candidates:
        if c in found:
            LOGGER.debug("Resolved column %s.%s -> %s", schema, table, c)
            return c
    LOGGER.debug("No matching columns found on %s.%s from %s", schema, table, candidates)
    return None


def parse_sender_whitelist(raw: str) -> Set[str]:
    """Parse comma-separated sender whitelist into a normalized set."""
    if not raw:
        return set()
    return {item.strip() for item in raw.split(",") if item.strip()}


def parse_sms_route_command(message: str) -> Optional[str]:
    """Parse command text and return nick_name for a valid _smsRoute command.

    Expected format:
        _smsRoute <nick_name>
    """
    if not message:
        return None
    text = message.strip()
    if not text.startswith("_smsRoute "):
        return None

    try:
        parts = shlex.split(text)
    except ValueError:
        return None

    if len(parts) < 2 or parts[0] != "_smsRoute":
        return None

    nick_name = parts[1].strip()
    if not nick_name:
        return None

    return nick_name


def http_exec_sql(api_url: str, db_name: str, sql: str, timeout: float) -> Any:
    """Call local execSql HTTP API and return parsed JSON result."""
    LOGGER.debug("Calling execSql API. url=%s db=%s timeout=%.2fs", api_url, db_name, timeout)
    payload = {"dbName": db_name, "sql": sql}
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        api_url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        body = resp.read().decode("utf-8", errors="replace")
    try:
        return json.loads(body)
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"execSql returned non-JSON response: {body[:300]}") from exc


def iter_dicts(obj: Any) -> Iterable[Dict[str, Any]]:
    """Yield all dict nodes found recursively in nested dict/list structures."""
    if isinstance(obj, dict):
        yield obj
        for v in obj.values():
            yield from iter_dicts(v)
    elif isinstance(obj, list):
        for item in obj:
            yield from iter_dicts(item)


def extract_wxid(api_result: Any) -> Optional[str]:
    """Extract first non-empty wxid-like field from arbitrary API JSON payload."""
    key_candidates = ("wxid", "UserName", "userName", "username", "wxId")
    for d in iter_dicts(api_result):
        for key in key_candidates:
            val = d.get(key)
            if isinstance(val, str) and val.strip():
                return val.strip()
    return None


def lookup_chatroom_wxid(
    api_url: str, wechat_db_name: str, nick_name: str, timeout: float
) -> str:
    """Query WeChat DB through execSql API and resolve chatroom wxid by nickname."""
    safe_name = quote_sql(nick_name)
    sql = (
        "SELECT UserName AS wxid, NickName "
        "FROM Contact "
        f"WHERE NickName = '{safe_name}' "
        "AND UserName LIKE '%@chatroom' "
        "LIMIT 1"
    )
    result = http_exec_sql(api_url=api_url, db_name=wechat_db_name, sql=sql, timeout=timeout)
    wxid = extract_wxid(result)
    if not wxid:
        raise RuntimeError(f"No wxid found for nick_name='{nick_name}'. API result: {result}")
    return wxid


def insert_chatroom_row(
    conn: pymysql.connections.Connection,
    wxid: str,
    nick_name: str,
    chatroom_type: str,
    tenantname: str,
) -> None:
    """Insert or update one mapping row in wechat_chatroom.

    The live schema uses primary key `wxid`, `nickname`, required `chatroom_type`, and
    `tenantname`, so repeated commands should refresh the existing row instead of failing.
    """
    sql = (
        "INSERT INTO `wzwmonitor`.`wechat_chatroom` "
        "(wxid, nickname, chatroom_type, tenantname) "
        "VALUES (%s, %s, %s, %s) "
        "ON DUPLICATE KEY UPDATE "
        "nickname=VALUES(nickname), "
        "chatroom_type=VALUES(chatroom_type), "
        "tenantname=VALUES(tenantname), "
        "update_time=CURRENT_TIMESTAMP"
    )

    with conn.cursor() as cur:
        cur.execute(sql, (wxid, nick_name, chatroom_type, tenantname))


def update_sms_status(
    conn: pymysql.connections.Connection,
    row_id: int,
    new_status: int,
) -> None:
    """Update one decoded_sms row status after processing."""
    sql = (
        "UPDATE `zbxalerts`.`decoded_sms` "
        "SET `send_status`=%s WHERE `id`=%s"
    )
    with conn.cursor() as cur:
        cur.execute(sql, (new_status, row_id))


def build_parser() -> argparse.ArgumentParser:
    """Build command-line parser for script runtime options."""
    p = argparse.ArgumentParser(
        description=(
            "Read _smsRoute <nick_name> commands from decoded_sms, then sync to wechat_chatroom."
        )
    )
    p.add_argument("--config", default="config.ini", help="Path to flat key=value config file")
    p.add_argument("--db-host", default="192.168.18.130")
    p.add_argument("--db-port", type=int, default=3306)
    p.add_argument("--db-user", default="root")
    p.add_argument("--db-password", default="123456")
    p.add_argument("--source-schema", default="zbxalerts")
    p.add_argument("--source-table", default="decoded_sms")
    p.add_argument("--target-schema", default="wzwmonitor")
    p.add_argument("--target-table", default="wechat_chatroom")
    p.add_argument("--api-url", default="http://192.168.136.1:8080/api/execSql")
    p.add_argument("--wechat-db-name", default="MicroMsg")
    p.add_argument("--batch-size", type=int, default=100)
    p.add_argument("--http-timeout", type=float, default=5.0)
    p.add_argument("--success-status", type=int, default=1)
    p.add_argument("--error-status", type=int, default=-1)
    p.add_argument(
        "--chatroom-type",
        default="sms_route",
        help="Value written to wechat_chatroom.chatroom_type.",
    )
    p.add_argument(
        "--sender-column",
        help="Override sender column name in decoded_sms (e.g. sender_number/phone).",
    )
    p.add_argument(
        "--allowed-senders",
        help="Comma-separated sender whitelist. Only these senders are treated as commands.",
        default="8618710029810,8618612310179",
    )
    p.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        help="Logging verbosity level.",
    )
    return p


def load_config(args: argparse.Namespace) -> AppConfig:
    """Load configuration from config file and CLI arguments."""
    cfg = parse_flat_ini(args.config)
    db_host = args.db_host or cfg.get("DB_HOST", "127.0.0.1")
    db_port = args.db_port or int(cfg.get("DB_PORT", "3306"))
    db_user = args.db_user or cfg.get("DB_USER", "root")
    db_password = args.db_password or cfg.get("DB_PASSWORD", "")
    sender_column = args.sender_column or cfg.get("SENDER_COLUMN")
    allowed_senders = parse_sender_whitelist(
        args.allowed_senders or cfg.get("ALLOWED_SENDERS", "")
    )
    LOGGER.debug(
        "Config loaded. source=%s.%s target=%s.%s whitelist_size=%d",
        args.source_schema,
        args.source_table,
        args.target_schema,
        args.target_table,
        len(allowed_senders),
    )

    return AppConfig(
        db_host=db_host,
        db_port=db_port,
        db_user=db_user,
        db_password=db_password,
        source_schema=args.source_schema,
        source_table=args.source_table,
        target_schema=args.target_schema,
        target_table=args.target_table,
        api_url=args.api_url,
        wechat_db_name=args.wechat_db_name,
        batch_size=args.batch_size,
        http_timeout=args.http_timeout,
        success_status=args.success_status,
        error_status=args.error_status,
        chatroom_type=args.chatroom_type,
        sender_column=sender_column,
        allowed_senders=allowed_senders,
    )


def main() -> int:
    """Run one batch of SMS command synchronization."""
    parser = build_parser()
    args = parser.parse_args()
    setup_logging(args.log_level)
    cfg = load_config(args)

    LOGGER.info(
        "Starting sync. source=%s.%s target=%s.%s batch_size=%d",
        cfg.source_schema,
        cfg.source_table,
        cfg.target_schema,
        cfg.target_table,
        cfg.batch_size,
    )
    if cfg.allowed_senders:
        LOGGER.info("Sender whitelist enabled. count=%d", len(cfg.allowed_senders))
    else:
        LOGGER.warning("Sender whitelist is empty. Any sender can trigger _smsRoute commands.")

    conn = pymysql.connect(
        host=cfg.db_host,
        port=cfg.db_port,
        user=cfg.db_user,
        password=cfg.db_password,
        database=cfg.source_schema,
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=False,
    )

    try:
        sender_col = cfg.sender_column or SOURCE_SENDER_COLUMN

        LOGGER.info(
            "Resolved columns. id=%s message=%s status=%s sender=%s",
            SOURCE_ID_COLUMN,
            SOURCE_MESSAGE_COLUMN,
            SOURCE_STATUS_COLUMN,
            sender_col,
        )

        select_fields = [
            f"`{SOURCE_ID_COLUMN}` AS row_id",
            f"`{SOURCE_MESSAGE_COLUMN}` AS message",
        ]
        if sender_col:
            select_fields.append(f"`{sender_col}` AS sender")

        select_sql = (
            "SELECT "
            + ", ".join(select_fields)
            + f" FROM `{cfg.source_schema}`.`{cfg.source_table}` "
            + f"WHERE `{SOURCE_STATUS_COLUMN}`=0 "
            + f"ORDER BY `{SOURCE_ID_COLUMN}` ASC LIMIT %s"
        )

        with conn.cursor() as cur:
            cur.execute(select_sql, (cfg.batch_size,))
            rows: List[Dict[str, Any]] = list(cur.fetchall())
        LOGGER.info("Fetched pending rows: %d", len(rows))

        if not rows:
            LOGGER.info("No pending rows found (send_status=0).")
            conn.rollback()
            return 0

        success = 0
        skipped = 0
        failed = 0

        for row in rows:
            row_id = int(row["row_id"])
            message = str(row.get("message") or "")
            sender = str(row.get("sender") or "").strip()
            LOGGER.debug("Processing row_id=%s sender=%s message=%r", row_id, sender, message)

            # A command must come from a sender in the whitelist (when configured).
            if cfg.allowed_senders and sender not in cfg.allowed_senders:
                skipped += 1
                LOGGER.debug("Skip row_id=%s: sender not in whitelist", row_id)
                continue

            parsed = parse_sms_route_command(message)
            if not parsed:
                skipped += 1
                LOGGER.debug("Skip row_id=%s: message is not a valid _smsRoute command", row_id)
                continue

            nick_name = parsed
            try:
                wxid = lookup_chatroom_wxid(
                    api_url=cfg.api_url,
                    wechat_db_name=cfg.wechat_db_name,
                    nick_name=nick_name,
                    timeout=cfg.http_timeout,
                )
                tenantname = wxid
                insert_chatroom_row(
                    conn=conn,
                    wxid=wxid,
                    nick_name=nick_name,
                    chatroom_type=cfg.chatroom_type,
                    tenantname=tenantname,
                )
                update_sms_status(
                    conn=conn,
                    row_id=row_id,
                    new_status=cfg.success_status,
                )
                success += 1
                LOGGER.info(
                    "Processed row_id=%s nick_name=%s wxid=%s tenantname=%s",
                    row_id,
                    nick_name,
                    wxid,
                    tenantname,
                )
            except Exception as exc:  # pylint: disable=broad-except
                failed += 1
                LOGGER.exception("Failed processing row_id=%s: %s", row_id, exc)
                update_sms_status(
                    conn=conn,
                    row_id=row_id,
                    new_status=cfg.error_status,
                )

        conn.commit()
        LOGGER.info(
            (
                "Sync done. total=%d success=%d skipped=%d failed=%d "
                "status_col=%s message_col=%s sender_col=%s"
            ),
            len(rows),
            success,
            skipped,
            failed,
            SOURCE_STATUS_COLUMN,
            SOURCE_MESSAGE_COLUMN,
            sender_col,
        )
        return 0 if failed == 0 else 2
    finally:
        LOGGER.debug("Closing DB connection")
        conn.close()


if __name__ == "__main__":
    raise SystemExit(main())
