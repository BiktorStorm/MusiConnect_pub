"""Device Panel — clickable audio device selectors for input and output."""

from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QHBoxLayout, QLabel, QListWidget,
    QListWidgetItem, QGroupBox
)
from PyQt6.QtCore import pyqtSignal, Qt


class DevicePanel(QGroupBox):
    """Panel showing input and output audio devices as clickable lists."""

    input_device_changed = pyqtSignal(int)   # selected index
    output_device_changed = pyqtSignal(int)  # selected index

    def __init__(self, parent=None):
        super().__init__("Audio Devices", parent)
        self._setup_ui()

    def _setup_ui(self):
        layout = QHBoxLayout(self)

        # Input devices (left)
        input_col = QVBoxLayout()
        input_label = QLabel("🎤  Input Device")
        input_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._input_list = QListWidget()
        self._input_list.currentRowChanged.connect(self.input_device_changed.emit)
        input_col.addWidget(input_label)
        input_col.addWidget(self._input_list)
        layout.addLayout(input_col)

        # Output devices (right)
        output_col = QVBoxLayout()
        output_label = QLabel("🔊  Output Device")
        output_label.setAlignment(Qt.AlignmentFlag.AlignCenter)
        self._output_list = QListWidget()
        self._output_list.currentRowChanged.connect(self.output_device_changed.emit)
        output_col.addWidget(output_label)
        output_col.addWidget(self._output_list)
        layout.addLayout(output_col)

    def set_input_devices(self, devices: list[str]):
        """Populate input device list."""
        self._input_list.clear()
        for name in devices:
            self._input_list.addItem(name)
        if devices:
            self._input_list.setCurrentRow(0)

    def set_output_devices(self, devices: list[str]):
        """Populate output device list."""
        self._output_list.clear()
        for name in devices:
            self._output_list.addItem(name)
        if devices:
            self._output_list.setCurrentRow(0)

    def get_selected_input(self) -> int:
        """Return currently selected input device index (-1 if none)."""
        return self._input_list.currentRow()

    def get_selected_output(self) -> int:
        """Return currently selected output device index (-1 if none)."""
        return self._output_list.currentRow()

    def set_enabled(self, enabled: bool):
        """Enable/disable device selection (disable while streaming)."""
        self._input_list.setEnabled(enabled)
        self._output_list.setEnabled(enabled)
