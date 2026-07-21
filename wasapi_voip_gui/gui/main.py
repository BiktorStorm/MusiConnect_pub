"""
MusiConnect GUI — Entry Point

Launches the PyQt6 interface and connects it to the C++ audio engine
via the EngineBridge subprocess manager.
"""

import sys
from pathlib import Path

from PyQt6.QtWidgets import QApplication
from PyQt6.QtCore import QTimer

from engine_bridge import EngineBridge
from ui import MainWindow


def load_stylesheet(app: QApplication):
    """Load the QSS stylesheet for custom theming."""
    qss_path = Path(__file__).parent / "assets" / "style.qss"
    if qss_path.exists():
        app.setStyleSheet(qss_path.read_text(encoding="utf-8"))


def main():
    app = QApplication(sys.argv)
    app.setApplicationName("MusiConnect")

    # Load custom stylesheet (CSS-like theming)
    load_stylesheet(app)

    # Create main window
    window = MainWindow()

    # Create engine bridge
    bridge = EngineBridge()

    # --- Wire signals ---

    # Engine → GUI: device lists
    def on_devices(direction: str, devices: list):
        if direction == "input":
            window.device_panel.set_input_devices(devices)
        elif direction == "output":
            window.device_panel.set_output_devices(devices)

    bridge.devices_received.connect(on_devices)

    # Engine → GUI: audio started
    def on_started(info: dict):
        window.set_connected(True)
        latency = info.get("latencyMs", "?")
        window.set_status(f"Streaming — estimated latency: {latency}ms")

    bridge.started.connect(on_started)

    # Engine → GUI: audio stopped
    def on_stopped():
        window.set_connected(False)

    bridge.stopped.connect(on_stopped)

    # Engine → GUI: stats update
    bridge.stats_received.connect(window.stats_panel.update_stats)

    # Engine → GUI: errors
    def on_error(msg: str):
        window.set_status(f"⚠ Error: {msg}")

    bridge.error_received.connect(on_error)

    # Engine exited unexpectedly
    def on_exit(code: int):
        window.set_connected(False)
        if code != 0:
            window.set_status(f"Engine exited with code {code}")

    bridge.engine_exited.connect(on_exit)

    # GUI → Engine: connect
    def on_connect(host: str, remote_port: int, local_port: int, buffer_size: int):
        input_idx = window.device_panel.get_selected_input()
        output_idx = window.device_panel.get_selected_output()
        if input_idx < 0 or output_idx < 0:
            window.set_status("⚠ Please select both input and output devices")
            return
        bridge.start_audio(
            remote_host=host,
            remote_port=remote_port,
            local_port=local_port,
            input_device=input_idx,
            output_device=output_idx,
            buffer_size=buffer_size,
        )
        window.set_status("Connecting...")

    window.connection_panel.connect_requested.connect(on_connect)

    # GUI → Engine: disconnect
    def on_disconnect():
        bridge.stop_audio()

    window.connection_panel.disconnect_requested.connect(on_disconnect)

    # --- Launch engine and request devices ---
    def startup():
        bridge.launch()
        # Give engine a moment to initialize, then request device lists
        QTimer.singleShot(500, lambda: bridge.request_devices("input"))
        QTimer.singleShot(600, lambda: bridge.request_devices("output"))

    # Delay startup slightly so window is visible
    QTimer.singleShot(100, startup)

    # --- Show and run ---
    window.show()
    exit_code = app.exec()

    # Cleanup
    bridge.shutdown()
    sys.exit(exit_code)


if __name__ == "__main__":
    main()
