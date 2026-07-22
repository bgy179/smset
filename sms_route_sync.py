#!/usr/bin/env python3
"""
Process SMS command rows from zbxalerts.decoded_sms and insert WeChat chatroom mappings.

Flow:
1. Read pending rows from decoded_sms where send_status == 0.
2. Parse command in the format: _smsRoute <nick_name>
3. Query wxid from local HTTP API: http://127.0.0.1:8080/api/execSql
4. Insert into wzwmonitor.wechat_chatroom(wxid, nickname, chatroom_type, tenantname=wxid)
5. Mark source row as processed.
6. Loop forever and run one sync cycle every 60 seconds.
"""

from __future__ import annotations

import argparse
import json
import logging
import shlex
import time
import urllib.error
import urllib.request
from dataclasses import dataclass
from typing import Any, Dict, List, Set

try:
    import pymysql
except ImportError as exc:  # pragma: no cover
    raise SystemExit(
        "Missing dependency: pymysql. Install with: pip install pymysql"
    ) from exc


LOGGER = logging.getLogger("sms_route_sync")

DEFAULT_ALLOWED_SENDERS = "8618710029810,8618612310179"
DEFAULT_CHATROOM_TYPE = "【中国移动磐基PaaS平台】zabbix 监控"
USED_CONFIG_KEYS = {
    "DB_HOST",
    "DB_PORT",
    "DB_USER",
    "DB_PASSWORD",
    "ALLOWED_SENDERS",
    "INTERVAL_SECONDS",
}


@dataclass
class AppConfig:
    """Runtime configuration resolved from CLI args and optional config.ini."""

    db_host: str
    db_port: int
    db_user: str
    db_password: str
    api_url: str
    batch_size: int
    http_timeout: float
    success_status: int
    error_status: int
    allowed_senders: Set[str]
    interval_seconds: int


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
                normalized_key = key.strip()
                if normalized_key not in USED_CONFIG_KEYS:
                    continue
                values[normalized_key] = value.strip()
    except FileNotFoundError:
        LOGGER.debug("Config file not found: %s", path)
        return values
         
    return values


def quote_sql(value: str) -> str:
    """Escape a Python string for use inside a single-quoted SQL literal."""
    # Minimal SQL string escaping for single-quoted literals.
    return value.replace("\\", "\\\\").replace("'", "''")


def parse_sender_whitelist(raw: str) -> Set[str]:
    """Parse comma-separated sender whitelist into a normalized set."""
    if not raw:
        return set()
    return {item.strip() for item in raw.split(",") if item.strip()}


def parse_sms_route_command(message: str) -> str | None:
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
    # Send UTF-8 JSON body and declare charset explicitly.
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


def extract_wxid(api_result: Any) -> str | None:
    """Extract wxid from execSql fixed response: {code, msg, data:[{wxid,...}]}"""
    if not isinstance(api_result, dict):
        return None
    if api_result.get("code") != 200:
        return None
    if api_result.get("msg") != "success":
        return None

    data = api_result.get("data")
    if not isinstance(data, list):
        return None

    for item in data:
        if not isinstance(item, dict):
            continue
        wxid = item.get("wxid")
        if isinstance(wxid, str) and wxid.strip():
            return wxid.strip()
    return None


def lookup_chatroom_wxid(api_url: str, nick_name: str, timeout: float) -> str:
    """Query WeChat DB through execSql API and resolve chatroom wxid by nickname."""
    # Use hex(NickName) matching to avoid any server-side Unicode decoding issues
    # when the SQL string contains Chinese characters.
    nickname_hex = nick_name.encode("utf-8").hex().upper()
    sql = (
        "SELECT UserName AS wxid, NickName "
        "FROM Contact "
        f"WHERE Type=2 AND hex(NickName)='{nickname_hex}' "
        "LIMIT 1"
    )
    result = http_exec_sql(api_url=api_url, db_name="MicroMsg.db", sql=sql, timeout=timeout)
    wxid = extract_wxid(result)
    if not wxid:
        raise RuntimeError(f"No wxid found for nick_name='{nick_name}'. API result: {result}")
    return wxid


def insert_chatroom_row(
    conn: pymysql.connections.Connection,
    wxid: str,
    nick_name: str,
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
    select_old_sql = (
        "SELECT tenantname FROM `wzwmonitor`.`wechat_chatroom` WHERE wxid=%s LIMIT 1"
    )
    sync_tenant_sql = (
        "UPDATE `wzwmonitor`.`paas_cluster_tenantname_ip` "
        "SET tenantname=%s WHERE tenantname=%s"
    )

    with conn.cursor() as cur:
        old_tenantname = None
        cur.execute(select_old_sql, (wxid,))
        row = cur.fetchone()
        if row and isinstance(row.get("tenantname"), str):
            old_tenantname = row["tenantname"].strip()

        params = (wxid, nick_name, DEFAULT_CHATROOM_TYPE, tenantname)
        LOGGER.debug(
            "Executing chatroom upsert. sql=%s params=%r",
            sql,
            params,
        )
        cur.execute(sql, params)
        LOGGER.info(
            "Chatroom upsert succeeded. wxid=%s nickname=%s tenantname=%s affected_rows=%d",
            wxid,
            nick_name,
            tenantname,
            cur.rowcount,
        )

        if old_tenantname and old_tenantname != tenantname:
            cur.execute(sync_tenant_sql, (tenantname, old_tenantname))
            LOGGER.info(
                (
                    "Tenantname changed in %s.%s for wxid=%s: old=%s new=%s; "
                    "synced %s.%s affected_rows=%d"
                ),
                "wzwmonitor",
                "wechat_chatroom",
                wxid,
                old_tenantname,
                tenantname,
                "wzwmonitor",
                "paas_cluster_tenantname_ip",
                cur.rowcount,
            )


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
    p.add_argument("--api-url", default="http://192.168.136.1:8080/api/execSql")
    p.add_argument("--batch-size", type=int, default=100)
    p.add_argument("--http-timeout", type=float, default=5.0)
    p.add_argument("--success-status", type=int, default=1)
    p.add_argument("--error-status", type=int, default=-1)
    p.add_argument(
        "--allowed-senders",
        help="Comma-separated sender whitelist. Only these senders are treated as commands.",
        default=DEFAULT_ALLOWED_SENDERS,
    )
    p.add_argument(
        "--log-level",
        default="INFO",
        choices=["DEBUG", "INFO", "WARNING", "ERROR", "CRITICAL"],
        help="Logging verbosity level.",
    )
    p.add_argument(
        "--interval-seconds",
        type=int,
        default=60,
        help="Service loop interval in seconds.",
    )
    p.add_argument(
        "--run-once",
        action="store_true",
        help="Run only one sync cycle and exit.",
    )
    return p


def load_config(args: argparse.Namespace) -> AppConfig:
    """Load configuration from config file and CLI arguments."""
    cfg = parse_flat_ini(args.config)
    db_host = args.db_host or cfg.get("DB_HOST", "127.0.0.1")
    db_port = args.db_port or int(cfg.get("DB_PORT", "3306"))
    db_user = args.db_user or cfg.get("DB_USER", "root")
    db_password = args.db_password or cfg.get("DB_PASSWORD", "")
    allowed_senders = parse_sender_whitelist(
        args.allowed_senders or cfg.get("ALLOWED_SENDERS", "")
    )
    interval_seconds = args.interval_seconds or int(cfg.get("INTERVAL_SECONDS", "60"))
    if interval_seconds <= 0:
        interval_seconds = 60
    LOGGER.debug(
        "Config loaded. whitelist_size=%d",
        len(allowed_senders),
    )

    return AppConfig(
        db_host=db_host,
        db_port=db_port,
        db_user=db_user,
        db_password=db_password,
        api_url=args.api_url,
        batch_size=args.batch_size,
        http_timeout=args.http_timeout,
        success_status=args.success_status,
        error_status=args.error_status,
        allowed_senders=allowed_senders,
        interval_seconds=interval_seconds,
    )


def run_sync_cycle(cfg: AppConfig) -> int:
    """Run one sync cycle and process current pending rows."""
    conn = pymysql.connect(
        host=cfg.db_host,
        port=cfg.db_port,
        user=cfg.db_user,
        password=cfg.db_password,
        database="zbxalerts",
        charset="utf8mb4",
        cursorclass=pymysql.cursors.DictCursor,
        autocommit=False,
    )

    try:
        LOGGER.info(
            "Resolved columns. id=%s message=%s status=%s sender=%s",
            "id",
            "message_content",
            "send_status",
            "sender",
        )

        select_sql = (
            "SELECT `id` AS row_id, `message_content` AS message, `sender` AS sender "
            "FROM `zbxalerts`.`decoded_sms` "
            "WHERE `send_status`=0 "
            "ORDER BY `id` ASC LIMIT %s"
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
                    nick_name=nick_name,
                    timeout=cfg.http_timeout,
                )
                tenantname = wxid
                insert_chatroom_row(
                    conn=conn,
                    wxid=wxid,
                    nick_name=nick_name,
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
            "send_status",
            "message_content",
            "sender",
        )
        return 0 if failed == 0 else 2
    finally:
        LOGGER.debug("Closing DB connection")
        conn.close()


def main() -> int:
    """Run as a long-running service and execute one cycle per interval."""
    parser = build_parser()
    args = parser.parse_args()
    setup_logging(args.log_level)
    cfg = load_config(args)

    LOGGER.info(
        "Starting sync service. source=%s.%s target=%s.%s batch_size=%d interval=%ds",
        "zbxalerts",
        "decoded_sms",
        "wzwmonitor",
        "wechat_chatroom",
        cfg.batch_size,
        cfg.interval_seconds,
    )
    if cfg.allowed_senders:
        LOGGER.info("Sender whitelist enabled. count=%d", len(cfg.allowed_senders))
    else:
        LOGGER.warning("Sender whitelist is empty. Any sender can trigger _smsRoute commands.")

    if args.run_once:
        return run_sync_cycle(cfg)

    try:
        while True:
            cycle_started_at = time.time()
            try:
                code = run_sync_cycle(cfg)
                if code != 0:
                    LOGGER.warning("Sync cycle completed with non-zero code=%d", code)
            except Exception as exc:  # pylint: disable=broad-except
                LOGGER.exception("Sync cycle crashed: %s", exc)

            elapsed = time.time() - cycle_started_at
            wait_seconds = max(0.0, float(cfg.interval_seconds) - elapsed)
            LOGGER.info("Sleeping %.1f seconds before next cycle", wait_seconds)
            time.sleep(wait_seconds)
    except KeyboardInterrupt:
        LOGGER.info("Service stopped by user")
        return 0


if __name__ == "__main__":
    raise SystemExit(main())
