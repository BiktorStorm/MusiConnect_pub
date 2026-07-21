"""Stats Panel — live display of connection statistics."""

from PyQt6.QtWidgets import (
    QFrame, QHBoxLayout, QVBoxLayout, QLabel
)
from PyQt6.QtCore import Qt


class StatsPanel(QFrame):
    """Live statistics display: packets sent/received, loss, underruns, latency."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setObjectName("statsFrame")
        self._setup_ui()

    def _setup_ui(self):
        layout = QHBoxLayout(self)
        layout.setContentsMargins(12, 8, 12, 8)

        self._sent_label = self._make_stat("Sent", "0")
        self._recv_label = self._make_stat("Received", "0")
        self._lost_label = self._make_stat("Lost", "0")
        self._underruns_label = self._make_stat("Underruns", "0")
        self._latency_label = self._make_stat("Latency", "—")

        layout.addWidget(self._sent_label)
        layout.addWidget(self._recv_label)
        layout.addWidget(self._lost_label)
        layout.addWidget(self._underruns_label)
        layout.addWidget(self._latency_label)

    def _make_stat(self, title: str, initial: str) -> QLabel:
        """Create a stat label with title above value."""
        label = QLabel(f"<small>{title}</small><br><b>{initial}</b>")
        label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        label.setTextFormat(Qt.TextFormat.RichText)
        return label

    def update_stats(self, stats: dict):
        """Update display with new stats from engine."""
        sent = stats.get("sent", 0)
        recv = stats.get("recv", 0)
        lost = stats.get("lost", 0)
        underruns = stats.get("underruns", 0)
        latency = stats.get("latencyMs", None)

        self._sent_label.setText(f"<small>Sent</small><br><b>{sent:,}</b>")
        self._recv_label.setText(f"<small>Received</small><br><b>{recv:,}</b>")
        self._lost_label.setText(f"<small>Lost</small><br><b>{lost:,}</b>")
        self._underruns_label.setText(f"<small>Underruns</small><br><b>{underruns:,}</b>")

        if latency is not None:
            self._latency_label.setText(f"<small>Latency</small><br><b>{latency:.1f}ms</b>")
        else:
            self._latency_label.setText("<small>Latency</small><br><b>—</b>")

    def reset(self):
        """Reset all stats to zero/default."""
        self._sent_label.setText("<small>Sent</small><br><b>0</b>")
        self._recv_label.setText("<small>Received</small><br><b>0</b>")
        self._lost_label.setText("<small>Lost</small><br><b>0</b>")
        self._underruns_label.setText("<small>Underruns</small><br><b>0</b>")
        self._latency_label.setText("<small>Latency</small><br><b>—</b>")
