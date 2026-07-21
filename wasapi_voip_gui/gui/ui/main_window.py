"""Main Window — assembles all UI panels into the application window."""

from PyQt6.QtWidgets import (
    QMainWindow, QWidget, QVBoxLayout, QLabel, QStatusBar
)
from PyQt6.QtCore import Qt

from .connection_panel import ConnectionPanel
from .device_panel import DevicePanel
from .stats_panel import StatsPanel


class MainWindow(QMainWindow):
    """MusiConnect main application window."""

    def __init__(self):
        super().__init__()
        self.setWindowTitle("MusiConnect — Ultra Low Latency P2P Audio")
        self.setMinimumSize(700, 550)
        self.resize(800, 620)

        # Central widget
        central = QWidget()
        self.setCentralWidget(central)
        layout = QVBoxLayout(central)
        layout.setSpacing(12)
        layout.setContentsMargins(16, 16, 16, 16)

        # Title
        title = QLabel("MusiConnect")
        title.setObjectName("titleLabel")
        title.setAlignment(Qt.AlignmentFlag.AlignCenter)
        layout.addWidget(title)

        # Connection panel (IP + ports + connect button)
        self.connection_panel = ConnectionPanel()
        layout.addWidget(self.connection_panel)

        # Device selection panel (input + output lists)
        self.device_panel = DevicePanel()
        layout.addWidget(self.device_panel, stretch=1)

        # Stats panel (bottom bar)
        self.stats_panel = StatsPanel()
        layout.addWidget(self.stats_panel)

        # Status bar
        self._status_bar = QStatusBar()
        self.setStatusBar(self._status_bar)
        self.set_status("Ready — select devices and enter remote IP to connect")

    def set_status(self, message: str):
        """Update the status bar text."""
        self._status_bar.showMessage(message)

    def set_connected(self, connected: bool):
        """Update all panels to reflect connection state."""
        self.connection_panel.set_connected(connected)
        self.device_panel.set_enabled(not connected)
        if connected:
            self.set_status("Connected — streaming audio")
        else:
            self.stats_panel.reset()
            self.set_status("Disconnected")
