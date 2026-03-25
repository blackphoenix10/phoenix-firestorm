#!/usr/bin/env python3
"""Interactive MCP client for Firestorm viewer.

This script connects to Firestorm over a localhost MCP transport,
optionally launches the viewer, and provides a small REPL.
"""

from __future__ import annotations

import argparse
import shutil
import json
import os
import pathlib
import shlex
import socket
import subprocess
import sys
import time
import threading
from typing import Any, Dict, List, Optional


class MCPClient:
    def __init__(
        self,
        viewer_exe: pathlib.Path,
        host: str = "127.0.0.1",
        port: int = 18080,
        timeout_s: float = 20.0,
        force_launch: bool = False,
    ) -> None:
        self.viewer_exe = viewer_exe
        self.proc: Optional[subprocess.Popen[bytes]] = None
        self.sock: Optional[socket.socket] = None
        self.next_id = 1
        self.host = host
        self.port = port
        self.timeout_s = timeout_s
        self.force_launch = force_launch
        self.attached_to_existing = False

    def _has_running_viewer_instance(self) -> bool:
        process_name = self.viewer_exe.name

        try:
            if os.name == "nt":
                result = subprocess.run(
                    ["tasklist", "/FI", f"IMAGENAME eq {process_name}"],
                    check=False,
                    capture_output=True,
                    text=True,
                )
                output = (result.stdout or "").lower()
                return process_name.lower() in output

            result = subprocess.run(
                ["ps", "-A", "-o", "comm="],
                check=False,
                capture_output=True,
                text=True,
            )
            processes = [line.strip() for line in (result.stdout or "").splitlines()]
            return process_name in processes or self.viewer_exe.name in processes
        except OSError:
            return False

    def _connect(self, deadline_s: float) -> None:
        end_time = time.time() + deadline_s
        last_error: Optional[Exception] = None

        while time.time() < end_time:
            try:
                sock = socket.create_connection((self.host, self.port), timeout=1.0)
                sock.settimeout(self.timeout_s)
                self.sock = sock
                return
            except OSError as exc:
                last_error = exc
                time.sleep(0.25)

        if last_error is None:
            raise RuntimeError(f"Timed out connecting to viewer MCP server at {self.host}:{self.port}")
        raise RuntimeError(
            f"Could not connect to viewer MCP server at {self.host}:{self.port}: {last_error}"
        )

    def start(self) -> None:
        if not self.viewer_exe.exists():
            raise FileNotFoundError(f"Viewer executable not found: {self.viewer_exe}")

        if self._has_running_viewer_instance() and not self.force_launch:
            try:
                self._connect(self.timeout_s)
            except RuntimeError as exc:
                raise RuntimeError(
                    "A Firestorm process is already running, but no MCP local transport "
                    f"was reachable at {self.host}:{self.port}. Restart the viewer with "
                    "the updated build or use --force-launch to start a new MCP-enabled instance. "
                    f"Details: {exc}"
                )
            self.attached_to_existing = True
            return

        self.proc = subprocess.Popen([str(self.viewer_exe)], cwd=str(self.viewer_exe.parent))
        self._connect(self.timeout_s)

    def stop(self) -> None:
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None

        if not self.proc:
            return

        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)

        self.proc = None

    def _ensure_connected(self) -> socket.socket:
        if self.proc and self.proc.poll() is not None:
            raise RuntimeError(f"Viewer exited with code {self.proc.returncode}")
        if not self.sock:
            raise RuntimeError("Viewer MCP socket is not connected")
        return self.sock

    def _read_exact(self, count: int) -> bytes:
        sock = self._ensure_connected()
        buf = bytearray()
        while len(buf) < count:
            chunk = sock.recv(count - len(buf))
            if not chunk:
                raise RuntimeError("Viewer closed the MCP socket while reading a response")
            buf.extend(chunk)
        return bytes(buf)

    def _read_message(self) -> Dict[str, Any]:
        sock = self._ensure_connected()
        header = bytearray()
        while b"\r\n\r\n" not in header:
            bch = sock.recv(1)
            if not bch:
                raise RuntimeError("Viewer closed the MCP socket while reading headers")
            header.extend(bch)
            if len(header) > 16384:
                raise RuntimeError("Invalid MCP header block (too large)")

        header_text = header.decode("utf-8", errors="replace")
        content_length = None
        for line in header_text.split("\r\n"):
            if line.lower().startswith("content-length:"):
                value = line.split(":", 1)[1].strip()
                content_length = int(value)
                break

        if content_length is None:
            raise RuntimeError("MCP response missing Content-Length header")

        body = self._read_exact(content_length)
        return json.loads(body.decode("utf-8"))

    def _write_message(self, payload: Dict[str, Any]) -> Dict[str, Any]:
        sock = self._ensure_connected()
        body = json.dumps(payload, separators=(",", ":")).encode("utf-8")
        frame = f"Content-Length: {len(body)}\r\n\r\n".encode("utf-8") + body
        sock.sendall(frame)

        result_box: Dict[str, Dict[str, Any]] = {}
        error_box: Dict[str, Exception] = {}

        def reader() -> None:
            try:
                result_box["result"] = self._read_message()
            except Exception as exc:
                error_box["error"] = exc

        t = threading.Thread(target=reader, daemon=True)
        t.start()
        t.join(self.timeout_s)

        if t.is_alive():
            raise RuntimeError(
                "Timed out waiting for MCP response from viewer. "
                "Check that FSMCPServerEnabled is true in viewer settings."
            )

        if "error" in error_box:
            raise error_box["error"]

        return result_box["result"]

    def request(self, method: str, params: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        req_id = self.next_id
        self.next_id += 1
        payload: Dict[str, Any] = {
            "jsonrpc": "2.0",
            "id": req_id,
            "method": method,
            "params": params or {},
        }
        return self._write_message(payload)

    def initialize(self) -> Dict[str, Any]:
        return self.request(
            "initialize",
            {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "firestorm-mcp-cli", "version": "1.0"},
            },
        )

    def tools_list(self) -> Dict[str, Any]:
        return self.request("tools/list", {})

    def tools_call(self, name: str, arguments: Optional[Dict[str, Any]] = None) -> Dict[str, Any]:
        return self.request("tools/call", {"name": name, "arguments": arguments or {}})


def print_json(data: Dict[str, Any]) -> None:
    print(json.dumps(data, indent=2, sort_keys=True))


def fetch_available_tools(client: MCPClient) -> List[Dict[str, Any]]:
    response = client.tools_list()
    result = response.get("result", {})
    tools = result.get("tools", [])
    if not isinstance(tools, list):
        raise RuntimeError("Viewer returned an invalid tools/list payload")
    return tools


def index_tools(tools: List[Dict[str, Any]]) -> Dict[str, Dict[str, Any]]:
    tool_index: Dict[str, Dict[str, Any]] = {}
    for tool in tools:
        name = tool.get("name")
        if isinstance(name, str) and name:
            tool_index[name] = tool
    return tool_index


def print_tool_summary(tools: List[Dict[str, Any]]) -> None:
    if not tools:
        print("Viewer reported no MCP tools.")
        return

    print("Available viewer tools:")
    for tool in tools:
        name = tool.get("name", "<unnamed>")
        description = tool.get("description", "")
        print(f"  {name}")
        if description:
            print(f"    {description}")


def print_tool_schema(tool: Dict[str, Any]) -> None:
    print_json(
        {
            "name": tool.get("name"),
            "description": tool.get("description"),
            "inputSchema": tool.get("inputSchema"),
        }
    )


def parse_args() -> argparse.Namespace:
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    default_viewer = repo_root / "build-vc180-64" / "newview" / "Release" / "firestorm-bin.exe"

    parser = argparse.ArgumentParser(description="Connect to Firestorm over localhost MCP")
    parser.add_argument(
        "--viewer",
        type=pathlib.Path,
        default=default_viewer,
        help=f"Path to viewer executable (default: {default_viewer})",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=20.0,
        help="Seconds to wait for each MCP response before failing (default: 20)",
    )
    parser.add_argument(
        "--host",
        default="127.0.0.1",
        help="MCP server host (default: 127.0.0.1)",
    )
    parser.add_argument(
        "--port",
        type=int,
        default=18080,
        help="MCP server port (default: 18080)",
    )
    parser.add_argument(
        "--run",
        action="append",
        default=[],
        help="Run a REPL command non-interactively. Can be specified multiple times.",
    )
    parser.add_argument(
        "--leave-running",
        action="store_true",
        help="Do not terminate the viewer process when the client exits.",
    )
    parser.add_argument(
        "--force-launch",
        action="store_true",
        help="Launch a new viewer instance even if another one is already running.",
    )
    return parser.parse_args()


def ensure_runtime_mcp_settings(viewer_exe: pathlib.Path) -> None:
    """Ensure runtime app_settings/settings.xml contains MCP controls.

    Some local build flows can leave Release/app_settings/settings.xml stale.
    If MCP keys are missing, copy the source settings.xml from the repo.
    """
    release_settings = viewer_exe.parent / "app_settings" / "settings.xml"
    if not release_settings.exists():
        return

    try:
        text = release_settings.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return

    if "FSMCPServerEnabled" in text:
        return

    source_settings = pathlib.Path(__file__).resolve().parents[2] / "indra" / "newview" / "app_settings" / "settings.xml"
    if not source_settings.exists():
        return

    try:
        shutil.copy2(source_settings, release_settings)
        print("Synced runtime settings.xml to include MCP controls.")
    except OSError as exc:
        print(f"Warning: could not sync runtime settings.xml: {exc}", file=sys.stderr)


def print_help() -> None:
    print("Commands:")
    print("  help")
    print("  help <tool-name>")
    print("  init")
    print("  tools")
    print("  refresh")
    print("  schema <tool-name>")
    print("  call <tool-name> [json-arguments]")
    print("  raw <json-rpc-method> <json-params>")
    print("  quit")


def execute_repl_command(
    client: MCPClient,
    line: str,
    tool_cache: Dict[str, Dict[str, Any]],
) -> bool:
    parts = shlex.split(line)
    if not parts:
        return True

    cmd = parts[0].lower()

    if cmd in {"quit", "exit"}:
        return False

    if cmd == "help":
        if len(parts) > 1:
            tool = tool_cache.get(parts[1])
            if not tool:
                print(f"Unknown tool: {parts[1]}")
                return True
            print_tool_schema(tool)
            return True
        print_help()
        return True

    if cmd == "init":
        print_json(client.initialize())
        return True

    if cmd == "tools":
        print_tool_summary(list(tool_cache.values()))
        return True

    if cmd == "refresh":
        tool_cache.clear()
        tool_cache.update(index_tools(fetch_available_tools(client)))
        print_tool_summary(list(tool_cache.values()))
        return True

    if cmd == "schema":
        if len(parts) != 2:
            print("Usage: schema <tool-name>")
            return True
        tool = tool_cache.get(parts[1])
        if not tool:
            print(f"Unknown tool: {parts[1]}")
            return True
        print_tool_schema(tool)
        return True

    if cmd == "call":
        if len(parts) < 2:
            print("Usage: call <tool-name> [json-arguments]")
            return True

        tool_name = parts[1]
        if tool_name not in tool_cache:
            print(f"Unknown tool: {tool_name}")
            return True

        arguments: Dict[str, Any] = {}
        if len(parts) > 2:
            try:
                parsed = json.loads(" ".join(parts[2:]))
            except json.JSONDecodeError as exc:
                print(f"Invalid JSON arguments: {exc}")
                return True

            if not isinstance(parsed, dict):
                print("Tool arguments must be a JSON object.")
                return True
            arguments = parsed

        print_json(client.tools_call(tool_name, arguments))
        return True

    if cmd == "raw":
        if len(parts) < 3:
            print("Usage: raw <json-rpc-method> <json-params>")
            return True
        method = parts[1]
        params = json.loads(" ".join(parts[2:]))
        print_json(client.request(method, params))
        return True

    print("Unknown command. Type 'help' for available commands.")
    return True


def main() -> int:
    args = parse_args()
    ensure_runtime_mcp_settings(args.viewer)
    client = MCPClient(
        args.viewer,
        host=args.host,
        port=args.port,
        timeout_s=args.timeout,
        force_launch=args.force_launch,
    )
    keep_running = args.leave_running

    try:
        client.start()
        if client.attached_to_existing:
            print(f"Connected to existing viewer MCP server at {args.host}:{args.port}")
        else:
            print(f"Started viewer: {args.viewer}")

        print("initialize:")
        print_json(client.initialize())

        tool_cache = index_tools(fetch_available_tools(client))

        print("tools/list:")
        print_tool_summary(list(tool_cache.values()))

        for command_line in args.run:
            if not execute_repl_command(client, command_line, tool_cache):
                return 0

        if args.run:
            return 0

        print("Type 'help' for commands. Log in within the viewer window if needed.")

        while True:
            line = input("mcp> ").strip()
            if not line:
                continue

            if not execute_repl_command(client, line, tool_cache):
                break

        return 0
    except KeyboardInterrupt:
        print("\nInterrupted")
        return 130
    except Exception as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 1
    finally:
        if not keep_running:
            client.stop()


if __name__ == "__main__":
    raise SystemExit(main())
