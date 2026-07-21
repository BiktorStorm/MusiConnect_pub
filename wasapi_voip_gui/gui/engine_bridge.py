"""
Engine Bridge — manages the C++ musiconnect subprocess.

Communicates via JSON lines over stdin/stdout:
  GUI → Engine:  {"cmd": "list-input-devices"}
  Engine → GUI:  {"type": "devices", "direction": "input", "devices": [...]}
"""

import json
import subprocess
import threading
from pathlib import Path
from typing import Optional, Callable

from PyQt6.QtCore import QObject, pyqtSignal


class EngineBridge(QObject):
    """Manages the musiconnect.exe subprocess with JSON protocol."""

    # Signals emitted when engine sends data
    devices_received = pyqtSignal(str, list)  # direction, device_list
    started = pyqtSignal(dict)                # start confirmation with config info
    stopped = pyqtSignal()
    stats_received = pyqtSignal(dict)         # periodic stats
    error_received = pyqtSignal(str)          # error messages
    engine_exited = pyqtSignal(int)           # exit code

    def __init__(self, engine_path: Optional[str] = None):
        super().__init__()
        self._process: Optional[subprocess.Popen] = None
        self._reader_thread: Optional[threading.Thread] = None
        self._running = False

        # Default engine path: look next to the gui folder
        if engine_path is None:
            base = Path(__file__).parent.parent / "engine" / "build" / "Release" / "musiconnect.exe"
            self._engine_path = str(base)
        else:
            self._engine_path = engine_path

    @property
    def is_running(self) -> bool:
        return self._running and self._process is not None and self._process.poll() is None

    def launch(self):
        """Launch the engine subprocess in JSON mode."""
        if self.is_running:
            return

        try:
            self._process = subprocess.Popen(
                [self._engine_path, "--json-mode"],
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                bufsize=1,  # Line-buffered
            )
            self._running = True
            self._reader_thread = threading.Thread(target=self._read_loop, daemon=True)
            self._reader_thread.start()
        except FileNotFoundError:
            self.error_received.emit(f"Engine not found: {self._engine_path}")
        except Exception as e:
            self.error_received.emit(f"Failed to launch engine: {e}")

    def shutdown(self):
        """Stop the engine subprocess."""
        if self._process and self._process.poll() is None:
            self._send({"cmd": "quit"})
            try:
                self._process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self._process.kill()
        self._running = False

    def request_devices(self, direction: str = "input"):
        """Request device list from engine. direction: 'input' or 'output'."""
        self._send({"cmd": f"list-{direction}-devices"})

    def start_audio(self, remote_host: str, remote_port: int, local_port: int,
                    input_device: int, output_device: int,
                    buffer_size: int = 128, bitrate: int = 64000):
        """Start the audio pipeline with given configuration."""
        self._send({
            "cmd": "start",
            "remoteHost": remote_host,
            "remotePort": remote_port,
            "localPort": local_port,
            "inputDevice": input_device,
            "outputDevice": output_device,
            "bufferSize": buffer_size,
            "bitrate": bitrate,
        })

    def stop_audio(self):
        """Stop the audio pipeline (keeps engine running for reconfiguration)."""
        self._send({"cmd": "stop"})

    def _send(self, msg: dict):
        """Send a JSON command to the engine's stdin."""
        if self._process and self._process.stdin:
            try:
                line = json.dumps(msg) + "\n"
                self._process.stdin.write(line)
                self._process.stdin.flush()
            except (BrokenPipeError, OSError):
                self.error_received.emit("Lost connection to engine")

    def _read_loop(self):
        """Background thread: read JSON lines from engine stdout."""
        try:
            while self._running and self._process and self._process.poll() is None:
                line = self._process.stdout.readline()
                if not line:
                    break
                line = line.strip()
                if not line:
                    continue
                try:
                    msg = json.loads(line)
                    self._dispatch(msg)
                except json.JSONDecodeError:
                    # Non-JSON output (e.g., debug prints) — ignore or log
                    pass
        finally:
            exit_code = self._process.returncode if self._process else -1
            self._running = False
            self.engine_exited.emit(exit_code or 0)

    def _dispatch(self, msg: dict):
        """Route incoming engine messages to appropriate signals."""
        msg_type = msg.get("type", "")

        if msg_type == "devices":
            self.devices_received.emit(msg.get("direction", ""), msg.get("devices", []))
        elif msg_type == "started":
            self.started.emit(msg)
        elif msg_type == "stopped":
            self.stopped.emit()
        elif msg_type == "stats":
            self.stats_received.emit(msg)
        elif msg_type == "error":
            self.error_received.emit(msg.get("message", "Unknown error"))
