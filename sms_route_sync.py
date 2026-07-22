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
import os
import re
import shlex
import signal
import subprocess
import sys
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
    "LOG_FILE",
    "PID_FILE",
}

STOP_REQUESTED = False


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
    log_file: str
    pid_file: str


@dataclass
class SmsCommand:
    """Parsed SMS command payload."""

    kind: str
    nick_name: str
    ip_list: List[str]


def setup_logging(level_name: str, log_file: str, enable_console: bool = True) -> None:
    """Configure global logging format and level.

    Args:
        level_name: One of DEBUG/INFO/WARNING/ERROR/CRITICAL (case-insensitive).
        log_file: Log file path for file output.
        enable_console: Whether to enable console log output.
    """
    level = getattr(logging, level_name.upper(), logging.INFO)
    formatter = logging.Formatter(
        fmt="%(asctime)s %(levelname)s [%(name)s] %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    root_logger = logging.getLogger()
    root_logger.setLevel(level)
    root_logger.handlers.clear()

    if enable_console:
        console_handler = logging.StreamHandler()
        console_handler.setLevel(level)
        console_handler.setFormatter(formatter)
        root_logger.addHandler(console_handler)

    log_dir = os.path.dirname(log_file)
    if log_dir:
        os.makedirs(log_dir, exist_ok=True)
    file_handler = logging.FileHandler(log_file, encoding="utf-8")
    file_handler.setLevel(level)
    file_handler.setFormatter(formatter)
    root_logger.addHandler(file_handler)

    LOGGER.info(
        "Logging configured. level=%s file=%s console=%s",
        level_name.upper(),
        log_file,
        enable_console,
    )


def request_stop(signum: int, _frame: Any) -> None:
    """Signal handler to stop service loop gracefully."""
    global STOP_REQUESTED
    STOP_REQUESTED = True
    LOGGER.warning("Stop requested by signal=%s", signum)


def register_signal_handlers() -> None:
    """Register process signal handlers for graceful shutdown."""
    signal.signal(signal.SIGINT, request_stop)
    if hasattr(signal, "SIGTERM"):
        signal.signal(signal.SIGTERM, request_stop)


def ensure_single_instance(pid_file: str) -> int | None:
    """Create PID file if no running instance exists; return existing PID otherwise."""
    if os.path.exists(pid_file):
        try:
            with open(pid_file, "r", encoding="utf-8") as f:
                raw = f.read().strip()
            existing_pid = int(raw)
        except Exception:
            existing_pid = None

        if existing_pid:
            try:
                os.kill(existing_pid, 0)
                return existing_pid
            except OSError:
                LOGGER.warning("Removing stale PID file: %s", pid_file)
                os.remove(pid_file)
        else:
            LOGGER.warning("Removing invalid PID file: %s", pid_file)
            os.remove(pid_file)

    pid_dir = os.path.dirname(pid_file)
    if pid_dir:
        os.makedirs(pid_dir, exist_ok=True)
    with open(pid_file, "w", encoding="utf-8") as f:
        f.write(str(os.getpid()))
    return None


def remove_pid_file(pid_file: str) -> None:
    """Best-effort PID file cleanup."""
    try:
        if os.path.exists(pid_file):
            os.remove(pid_file)
    except OSError as exc:
        LOGGER.warning("Failed to remove PID file %s: %s", pid_file, exc)


def launch_detached_child(args: argparse.Namespace) -> int:
    """Launch detached background child process on Windows and return immediately."""
    child_argv = [
        arg
        for arg in sys.argv[1:]
        if arg not in ("--daemon", "--stop")
    ]
    child_argv.append("--daemon-child")
    cmd = [sys.executable, os.path.abspath(__file__), *child_argv]

    creationflags = 0
    if os.name == "nt":
        creationflags = subprocess.CREATE_NEW_PROCESS_GROUP | subprocess.DETACHED_PROCESS

    LOGGER.info("Launching detached child process: %s", cmd)
    subprocess.Popen(
        cmd,
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        creationflags=creationflags,
        close_fds=True,
        cwd=os.getcwd(),
    )
    return 0


def stop_background_process(pid_file: str) -> int:
    """Stop background process by PID file."""
    if not os.path.exists(pid_file):
        print(f"PID file not found: {pid_file}")
        return 1

    try:
        with open(pid_file, "r", encoding="utf-8") as f:
            pid = int(f.read().strip())
    except Exception as exc:
        print(f"Invalid PID file {pid_file}: {exc}")
        return 1

    try:
        os.kill(pid, signal.SIGTERM)
        print(f"Stop signal sent to PID {pid}")
        return 0
    except OSError as exc:
        print(f"Failed to stop PID {pid}: {exc}")
        return 1


def parse_flat_ini(path: str) -> Dict[str, str]:
    """Parse a flat key=value ini-like file without sections.

    Args:
        path: File path to read.

    Returns:
        Dict of parsed key/value pairs. Returns empty dict when file does not exist.
    """
    values: Dict[str, str] = {}
    LOGGER.debug("Loading config file: path=%s", path)
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

    LOGGER.info("Config file loaded: path=%s keys=%s", path, sorted(values.keys()))
    return values


def quote_sql(value: str) -> str:
    """Escape a Python string for use inside a single-quoted SQL literal."""
    # Minimal SQL string escaping for single-quoted literals.
    return value.replace("\\", "\\\\").replace("'", "''")


def parse_sender_whitelist(raw: str) -> Set[str]:
    """Parse comma-separated sender whitelist into a normalized set."""
    if not raw:
        LOGGER.debug("Sender whitelist input is empty")
        return set()
    senders = {item.strip() for item in raw.split(",") if item.strip()}
    LOGGER.debug("Parsed sender whitelist: count=%d values=%s", len(senders), sorted(senders))
    return senders


def parse_sms_command(message: str) -> SmsCommand | None:
    """Parse supported SMS commands.

    Supported formats:
        _smsRoute <nick_name>
        _smsRegister <nick_name> <ip_list>
    """
    if not message:
        LOGGER.debug("SMS command parse skipped: empty message")
        return None
    text = message.strip()
    if not (text.startswith("_smsRoute ") or text.startswith("_smsRegister ")):
        LOGGER.debug("SMS command parse skipped: unsupported prefix message=%r", message)
        return None

    try:
        parts = shlex.split(text)
    except ValueError as exc:
        LOGGER.warning("SMS command parse failed: shlex error message=%r error=%s", message, exc)
        return None

    LOGGER.debug("SMS command tokenized: parts=%r", parts)

    command = parts[0]
    if command == "_smsRoute":
        if len(parts) != 2:
            LOGGER.warning("_smsRoute parse failed: expected 2 parts actual=%d message=%r", len(parts), message)
            return None
        nick_name = parts[1].strip()
        if not nick_name:
            LOGGER.warning("_smsRoute parse failed: empty nick_name message=%r", message)
            return None
        LOGGER.info("Parsed SMS command: kind=route nick_name=%s", nick_name)
        return SmsCommand(kind="route", nick_name=nick_name, ip_list=[])

    if command == "_smsRegister":
        if len(parts) < 3:
            LOGGER.warning("_smsRegister parse failed: insufficient parts actual=%d message=%r", len(parts), message)
            return None
        nick_name = parts[1].strip()
        if not nick_name:
            LOGGER.warning("_smsRegister parse failed: empty nick_name message=%r", message)
            return None

        ip_values: List[str] = []
        for part in parts[2:]:
            for item in re.split(r"[;,\s]+", part):
                ip = item.strip()
                if ip:
                    ip_values.append(ip)

        if not ip_values:
            LOGGER.warning("_smsRegister parse failed: empty ip list message=%r", message)
            return None
        LOGGER.info(
            "Parsed SMS command: kind=register nick_name=%s ip_count=%d ips=%s",
            nick_name,
            len(ip_values),
            ip_values,
        )
        return SmsCommand(kind="register", nick_name=nick_name, ip_list=ip_values)

    LOGGER.debug("SMS command parse ended with unsupported command=%s", command)
    return None


def http_exec_sql(api_url: str, db_name: str, sql: str, timeout: float) -> Any:
    """Call local execSql HTTP API and return parsed JSON result."""
    LOGGER.info("Calling execSql API: url=%s db=%s timeout=%.2fs", api_url, db_name, timeout)
    LOGGER.debug("execSql request SQL: %s", sql)
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
        LOGGER.debug("execSql HTTP response status=%s", getattr(resp, "status", "unknown"))
        body = resp.read().decode("utf-8", errors="replace")
    LOGGER.debug("execSql raw response body=%s", body)
    try:
        parsed = json.loads(body)
        LOGGER.info("execSql API call finished successfully")
        return parsed
    except json.JSONDecodeError as exc:
        raise RuntimeError(f"execSql returned non-JSON response: {body[:300]}") from exc


def http_post_json(url: str, payload: Dict[str, Any], timeout: float) -> Any:
    """POST JSON to a local HTTP API and return parsed JSON when possible."""
    LOGGER.info("Calling JSON API: url=%s timeout=%.2fs", url, timeout)
    LOGGER.debug("JSON API request payload=%s", payload)
    data = json.dumps(payload, ensure_ascii=False).encode("utf-8")
    req = urllib.request.Request(
        url,
        data=data,
        headers={"Content-Type": "application/json"},
        method="POST",
    )
    with urllib.request.urlopen(req, timeout=timeout) as resp:
        LOGGER.debug("JSON API response status=%s", getattr(resp, "status", "unknown"))
        body = resp.read().decode("utf-8", errors="replace")
    LOGGER.debug("JSON API raw response body=%s", body)
    if not body:
        LOGGER.info("JSON API returned empty body")
        return None
    try:
        parsed = json.loads(body)
        LOGGER.info("JSON API call finished successfully")
        return parsed
    except json.JSONDecodeError:
        LOGGER.info("JSON API returned non-JSON body")
        return body


def build_sendtxtmsg_url(api_url: str) -> str:
    """Build sendtxtmsg endpoint from execSql endpoint."""
    marker = "/api/execSql"
    if api_url.endswith(marker):
        send_url = api_url[: -len(marker)] + "/api/sendtxtmsg"
        LOGGER.debug("Derived sendtxtmsg URL from execSql URL: %s", send_url)
        return send_url
    send_url = api_url.rstrip("/") + "/api/sendtxtmsg"
    LOGGER.debug("Derived sendtxtmsg URL by suffix append: %s", send_url)
    return send_url


def extract_wxid(api_result: Any) -> str | None:
    """Extract wxid from execSql fixed response: {code, msg, data:[{wxid,...}]}"""
    if not isinstance(api_result, dict):
        LOGGER.warning("extract_wxid failed: api_result is not dict actual_type=%s", type(api_result).__name__)
        return None
    if api_result.get("code") != 200:
        LOGGER.warning("extract_wxid failed: code=%r response=%r", api_result.get("code"), api_result)
        return None
    if api_result.get("msg") != "success":
        LOGGER.warning("extract_wxid failed: msg=%r response=%r", api_result.get("msg"), api_result)
        return None

    data = api_result.get("data")
    if not isinstance(data, list):
        LOGGER.warning("extract_wxid failed: data is not list response=%r", api_result)
        return None

    for item in data:
        if not isinstance(item, dict):
            continue
        wxid = item.get("wxid")
        if isinstance(wxid, str) and wxid.strip():
            LOGGER.info("extract_wxid succeeded: wxid=%s", wxid.strip())
            return wxid.strip()
    LOGGER.warning("extract_wxid failed: no valid wxid found response=%r", api_result)
    return None


def lookup_chatroom_wxid(api_url: str, nick_name: str, timeout: float) -> str:
    """Query WeChat DB through execSql API and resolve chatroom wxid by nickname."""
    # Use hex(NickName) matching to avoid any server-side Unicode decoding issues
    # when the SQL string contains Chinese characters.
    nickname_hex = nick_name.encode("utf-8").hex().upper()
    LOGGER.info("Looking up chatroom wxid via execSql: nick_name=%s nickname_hex=%s", nick_name, nickname_hex)
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
    LOGGER.info("Chatroom wxid lookup succeeded via execSql: nick_name=%s wxid=%s", nick_name, wxid)
    return wxid


def lookup_chatroom_wxid_by_nickname(
    conn: pymysql.connections.Connection,
    nick_name: str,
) -> str:
    """Look up wxid from local wechat_chatroom table by nickname."""
    sql = (
        "SELECT wxid FROM `wzwmonitor`.`wechat_chatroom` "
        "WHERE nickname=%s LIMIT 1"
    )
    LOGGER.info("Looking up local chatroom wxid: nick_name=%s", nick_name)
    with conn.cursor() as cur:
        cur.execute(sql, (nick_name,))
        row = cur.fetchone()
    if not row or not isinstance(row.get("wxid"), str) or not row["wxid"].strip():
        raise RuntimeError(f"No wxid found in wzwmonitor.wechat_chatroom for nick_name='{nick_name}'")
    LOGGER.info("Local chatroom wxid lookup succeeded: nick_name=%s wxid=%s", nick_name, row["wxid"].strip())
    return row["wxid"].strip()


def insert_tenant_ip_rows(
    conn: pymysql.connections.Connection,
    tenantname: str,
    ip_list: List[str],
) -> List[str]:
    """Insert one paas_cluster_tenantname_ip row for each IP using tenantname=wxid."""
    sql = (
        "INSERT INTO `wzwmonitor`.`paas_cluster_tenantname_ip` "
        "(tenantname, clustername, tenantip) VALUES (%s, %s, %s)"
    )
    inserted_ips: List[str] = []
    LOGGER.info("Starting tenant IP inserts: tenantname=%s ip_count=%d", tenantname, len(ip_list))
    with conn.cursor() as cur:
        for ip in ip_list:
            LOGGER.debug("Executing tenant IP insert: tenantname=%s ip=%s", tenantname, ip)
            cur.execute(sql, (tenantname, tenantname, ip))
            inserted_ips.append(ip)
            LOGGER.info(
                "Tenant IP insert succeeded. tenantname=%s clustername=%s tenantip=%s affected_rows=%d",
                tenantname,
                tenantname,
                ip,
                cur.rowcount,
            )
    LOGGER.info("Completed tenant IP inserts: tenantname=%s inserted_ips=%s", tenantname, inserted_ips)
    return inserted_ips


def send_register_result_message(
    api_url: str,
    wxid: str,
    nick_name: str,
    inserted_ips: List[str],
    timeout: float,
) -> None:
    """Send a summary message to the target WeChat chatroom after register succeeds."""
    if not inserted_ips:
        raise RuntimeError(f"No IPs inserted for nick_name='{nick_name}', message will not be sent")

    sendtxtmsg_url = build_sendtxtmsg_url(api_url)
    message = "_smsRegister succeeded. inserted IPs: " + ", ".join(inserted_ips)
    payload = {"wxid": wxid, "content": message}
    LOGGER.info(
        "Sending register result message: nick_name=%s wxid=%s ip_count=%d url=%s",
        nick_name,
        wxid,
        len(inserted_ips),
        sendtxtmsg_url,
    )
    result = http_post_json(sendtxtmsg_url, payload, timeout)
    LOGGER.info(
        "sendtxtmsg succeeded. nick_name=%s wxid=%s ip_count=%d response=%r",
        nick_name,
        wxid,
        len(inserted_ips),
        result,
    )


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

    LOGGER.info("Starting chatroom upsert: wxid=%s nick_name=%s tenantname=%s", wxid, nick_name, tenantname)
    with conn.cursor() as cur:
        old_tenantname = None
        cur.execute(select_old_sql, (wxid,))
        row = cur.fetchone()
        if row and isinstance(row.get("tenantname"), str):
            old_tenantname = row["tenantname"].strip()
        LOGGER.debug("Existing tenantname lookup: wxid=%s old_tenantname=%s", wxid, old_tenantname)

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
    LOGGER.info("Updating SMS status: row_id=%s new_status=%s", row_id, new_status)
    with conn.cursor() as cur:
        cur.execute(sql, (new_status, row_id))
        LOGGER.debug("SMS status update affected_rows=%d row_id=%s", cur.rowcount, row_id)


def build_parser() -> argparse.ArgumentParser:
    """Build command-line parser for script runtime options."""
    p = argparse.ArgumentParser(
        description=(
            "Read _smsRoute/_smsRegister commands from decoded_sms and sync related tables."
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
    p.add_argument(
        "--log-file",
        default="sms_route_sync.log",
        help="Path to log file. Logs are written to both console and this file.",
    )
    p.add_argument(
        "--pid-file",
        default="sms_route_sync.pid",
        help="Path to PID file for background watchdog process.",
    )
    p.add_argument(
        "--daemon",
        action="store_true",
        help="Start as detached background watchdog process (no console required).",
    )
    p.add_argument(
        "--stop",
        action="store_true",
        help="Stop detached background watchdog process using --pid-file.",
    )
    p.add_argument(
        "--daemon-child",
        action="store_true",
        help=argparse.SUPPRESS,
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
    log_file = args.log_file or cfg.get("LOG_FILE", "sms_route_sync.log")
    pid_file = args.pid_file or cfg.get("PID_FILE", "sms_route_sync.pid")
    if interval_seconds <= 0:
        interval_seconds = 60
    LOGGER.debug(
        "Config loaded. whitelist_size=%d db_host=%s db_port=%d api_url=%s batch_size=%d interval_seconds=%d success_status=%d error_status=%d log_file=%s pid_file=%s",
        len(allowed_senders),
        db_host,
        db_port,
        args.api_url,
        args.batch_size,
        interval_seconds,
        args.success_status,
        args.error_status,
        log_file,
        pid_file,
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
        log_file=log_file,
        pid_file=pid_file,
    )


def run_sync_cycle(cfg: AppConfig) -> int:
    """Run one sync cycle and process current pending rows."""
    LOGGER.info(
        "Opening DB connection for sync cycle: host=%s port=%d user=%s batch_size=%d",
        cfg.db_host,
        cfg.db_port,
        cfg.db_user,
        cfg.batch_size,
    )
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
        LOGGER.info("Sync cycle started")
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
            LOGGER.debug("Executing pending SMS select: batch_size=%d", cfg.batch_size)
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
            LOGGER.info("Processing row: row_id=%s sender=%s message=%r", row_id, sender, message)

            # A command must come from a sender in the whitelist (when configured).
            if cfg.allowed_senders and sender not in cfg.allowed_senders:
                skipped += 1
                LOGGER.info(
                    "Skip row_id=%s: sender=%s not in whitelist=%s",
                    row_id,
                    sender,
                    sorted(cfg.allowed_senders),
                )
                continue

            command = parse_sms_command(message)
            if not command:
                skipped += 1
                LOGGER.info(
                    "Skip row_id=%s: unsupported command format message=%r",
                    row_id,
                    message,
                )
                continue

            try:
                if command.kind == "route":
                    LOGGER.info("Executing route command: row_id=%s nick_name=%s", row_id, command.nick_name)
                    wxid = lookup_chatroom_wxid(
                        api_url=cfg.api_url,
                        nick_name=command.nick_name,
                        timeout=cfg.http_timeout,
                    )
                    tenantname = wxid
                    insert_chatroom_row(
                        conn=conn,
                        wxid=wxid,
                        nick_name=command.nick_name,
                        tenantname=tenantname,
                    )
                    LOGGER.info(
                        "Processed _smsRoute row_id=%s nick_name=%s wxid=%s tenantname=%s",
                        row_id,
                        command.nick_name,
                        wxid,
                        tenantname,
                    )
                else:
                    LOGGER.info(
                        "Executing register command: row_id=%s nick_name=%s ip_count=%d",
                        row_id,
                        command.nick_name,
                        len(command.ip_list),
                    )
                    wxid = lookup_chatroom_wxid_by_nickname(conn=conn, nick_name=command.nick_name)
                    inserted_ips = insert_tenant_ip_rows(
                        conn=conn,
                        tenantname=wxid,
                        ip_list=command.ip_list,
                    )
                    send_register_result_message(
                        api_url=cfg.api_url,
                        wxid=wxid,
                        nick_name=command.nick_name,
                        inserted_ips=inserted_ips,
                        timeout=cfg.http_timeout,
                    )
                    LOGGER.info(
                        "Processed _smsRegister row_id=%s nick_name=%s wxid=%s ip_count=%d",
                        row_id,
                        command.nick_name,
                        wxid,
                        len(inserted_ips),
                    )

                update_sms_status(
                    conn=conn,
                    row_id=row_id,
                    new_status=cfg.success_status,
                )
                success += 1
            except Exception as exc:  # pylint: disable=broad-except
                failed += 1
                LOGGER.exception("Failed processing row_id=%s: %s", row_id, exc)
                update_sms_status(
                    conn=conn,
                    row_id=row_id,
                    new_status=cfg.error_status,
                )

        LOGGER.info("Committing sync cycle transaction")
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
        LOGGER.info("Closing DB connection for sync cycle")
        conn.close()


def main() -> int:
    """Run as a long-running service and execute one cycle per interval."""
    parser = build_parser()
    args = parser.parse_args()
    if args.stop:
        cfg = load_config(args)
        return stop_background_process(cfg.pid_file)

    if args.daemon and not args.daemon_child:
        cfg = load_config(args)
        setup_logging(args.log_level, cfg.log_file, enable_console=True)
        return launch_detached_child(args)

    cfg = load_config(args)
    setup_logging(args.log_level, cfg.log_file, enable_console=not args.daemon_child)
    register_signal_handlers()

    existing_pid = ensure_single_instance(cfg.pid_file)
    if existing_pid:
        LOGGER.error("Another instance is already running. pid=%s pid_file=%s", existing_pid, cfg.pid_file)
        return 1

    LOGGER.info("PID file created: %s pid=%s", cfg.pid_file, os.getpid())
    LOGGER.info("Program started with args=%s", vars(args))

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
        LOGGER.info(
            "Sender whitelist enabled. count=%d values=%s",
            len(cfg.allowed_senders),
            sorted(cfg.allowed_senders),
        )
    else:
        LOGGER.warning("Sender whitelist is empty. Any sender can trigger SMS commands.")

    if args.run_once:
        LOGGER.info("Run-once mode enabled")
        try:
            return run_sync_cycle(cfg)
        finally:
            remove_pid_file(cfg.pid_file)

    try:
        while not STOP_REQUESTED:
            cycle_started_at = time.time()
            LOGGER.info("Service loop iteration started")
            try:
                code = run_sync_cycle(cfg)
                if code != 0:
                    LOGGER.warning("Sync cycle completed with non-zero code=%d", code)
            except Exception as exc:  # pylint: disable=broad-except
                LOGGER.exception("Sync cycle crashed: %s", exc)

            elapsed = time.time() - cycle_started_at
            LOGGER.info("Service loop iteration finished: elapsed=%.3fs", elapsed)
            wait_seconds = max(0.0, float(cfg.interval_seconds) - elapsed)
            LOGGER.info("Sleeping %.1f seconds before next cycle", wait_seconds)
            sleep_end = time.time() + wait_seconds
            while time.time() < sleep_end:
                if STOP_REQUESTED:
                    break
                time.sleep(min(0.5, max(0.0, sleep_end - time.time())))
    except KeyboardInterrupt:
        LOGGER.info("Service stopped by user")
        return 0
    finally:
        remove_pid_file(cfg.pid_file)
        LOGGER.info("Service exited. PID file removed: %s", cfg.pid_file)

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
