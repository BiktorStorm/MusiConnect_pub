"""Connection Panel — IP address input and port configuration."""

from PyQt6.QtWidgets import (
    QWidget, QHBoxLayout, QVBoxLayout, QLabel, QLineEdit,
    QSpinBox, QPushButton, QGroupBox
)
from PyQt6.QtCore import pyqtSignal, QRegularExpression
from PyQt6.QtGui import QRegularExpressionValidator


class ConnectionPanel(QGroupBox):
    """Panel for entering remote peer IP and port."""

    connect_requested = pyqtSignal(str, int, int, int)  # host, remote_port, local_port, buffer_size
    disconnect_requested = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__("Connection", parent)
        self._connected = False
        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)

        # IP Address row
        ip_row = QHBoxLayout()
        ip_label = QLabel("Remote IP:")
        self._ip_input = QLineEdit()
        self._ip_input.setPlaceholderText("192.168.1.50")
        # Allow IP addresses and hostnames
        ip_regex = QRegularExpression(r"[\w\.\-]+")
        self._ip_input.setValidator(QRegularExpressionValidator(ip_regex))
        ip_row.addWidget(ip_label)
        ip_row.addWidget(self._ip_input, stretch=1)
        layout.addLayout(ip_row)

        # Port row
        port_row = QHBoxLayout()

        remote_port_label = QLabel("Remote Port:")
        self._remote_port = QSpinBox()
        self._remote_port.setRange(1024, 65535)
        self._remote_port.setValue(4464)

        local_port_label = QLabel("Local Port:")
        self._local_port = QSpinBox()
        self._local_port.setRange(1024, 65535)
        self._local_port.setValue(4464)

        port_row.addWidget(remote_port_label)
        port_row.addWidget(self._remote_port)
        port_row.addStretch()
        port_row.addWidget(local_port_label)
        port_row.addWidget(self._local_port)
        layout.addLayout(port_row)

        # Buffer size row
        buffer_row = QHBoxLayout()
        buffer_label = QLabel("Buffer Size (samples):")
        self._buffer_size = QSpinBox()
        self._buffer_size.setRange(32, 1024)
        self._buffer_size.setSingleStep(32)
        self._buffer_size.setValue(128)
        self._buffer_size.setToolTip(
            "Lower = less latency but higher CPU usage.\n"
            "64 = ~1.3ms, 128 = ~2.7ms, 256 = ~5.3ms at 48kHz.\n"
            "Not all hardware supports very low values."
        )
        buffer_row.addWidget(buffer_label)
        buffer_row.addWidget(self._buffer_size)
        buffer_row.addStretch()
        layout.addLayout(buffer_row)

        # Connect button
        self._connect_btn = QPushButton("Connect")
        self._connect_btn.setObjectName("connectButton")
        self._connect_btn.clicked.connect(self._on_connect_clicked)
        layout.addWidget(self._connect_btn)

    def _on_connect_clicked(self):
        if self._connected:
            self.disconnect_requested.emit()
        else:
            host = self._ip_input.text().strip()
            if not host:
                return
            self.connect_requested.emit(
                host,
                self._remote_port.value(),
                self._local_port.value(),
                self._buffer_size.value()
            )

    def set_connected(self, connected: bool):
        """Update UI to reflect connection state."""
        self._connected = connected
        if connected:
            self._connect_btn.setText("Disconnect")
            self._connect_btn.setObjectName("disconnectButton")
            self._ip_input.setEnabled(False)
            self._remote_port.setEnabled(False)
            self._local_port.setEnabled(False)
            self._buffer_size.setEnabled(False)
        else:
            self._connect_btn.setText("Connect")
            self._connect_btn.setObjectName("connectButton")
            self._ip_input.setEnabled(True)
            self._remote_port.setEnabled(True)
            self._local_port.setEnabled(True)
            self._buffer_size.setEnabled(True)
        # Force style refresh after objectName change
        self._connect_btn.style().unpolish(self._connect_btn)
        self._connect_btn.style().polish(self._connect_btn)
