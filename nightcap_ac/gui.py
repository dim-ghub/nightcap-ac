"""Settings window (PySide6)."""

from __future__ import annotations

import subprocess
import sys
from dataclasses import replace
from pathlib import Path

from PySide6.QtCore import QObject, Qt, QTimer, Signal
from PySide6.QtGui import QAction, QIcon
from PySide6.QtWidgets import (
    QApplication, QButtonGroup, QCheckBox, QComboBox, QFileDialog, QFormLayout,
    QGroupBox, QHBoxLayout, QLabel, QLineEdit, QMainWindow, QMenu,
    QMessageBox, QPlainTextEdit, QPushButton, QRadioButton, QSlider,
    QSystemTrayIcon, QVBoxLayout, QWidget,
)

from .animalese import STYLES, VOICES
from .audio import AudioError, list_microphones
from .config import OUTPUT_MODES, Config
from .engine import Engine, Status


class _Bridge(QObject):
    status = Signal(object)


def _slider(lo: int, hi: int, value: int) -> QSlider:
    s = QSlider(Qt.Orientation.Horizontal)
    s.setRange(lo, hi)
    s.setValue(value)
    return s


class Window(QMainWindow):
    def __init__(self, engine: Engine, bridge: _Bridge):
        super().__init__()
        self.engine = engine
        self.cfg = engine.config
        self.setWindowTitle("Nightcap AC")
        self.setMinimumWidth(460)
        self._save_timer = QTimer(self, singleShot=True, interval=400)
        self._save_timer.timeout.connect(self._save)

        root = QWidget()
        layout = QVBoxLayout(root)

        # Status
        self.enabled = QCheckBox("Speak my chat messages as Animalese")
        self.enabled.setChecked(self.cfg.enabled)
        self.enabled.setStyleSheet("font-weight: bold;")
        self.enabled.toggled.connect(lambda v: self._set(enabled=v))
        layout.addWidget(self.enabled)
        self.hook_label = QLabel()
        self.vc_label = QLabel()
        self.last_label = QLabel()
        self.skip_commands = QCheckBox("Don't speak chat commands (/e, /w, ...)")
        self.skip_commands.setChecked(self.cfg.skip_commands)
        self.skip_commands.toggled.connect(lambda v: self._set(skip_commands=v))
        layout.addWidget(self.skip_commands)
        for w in (self.hook_label, self.vc_label, self.last_label):
            w.setWordWrap(True)
            layout.addWidget(w)
        launch_row = QHBoxLayout()
        launch = QPushButton("Launch Nightcap")
        launch.setToolTip("Starts Nightcap with the chat hook loaded")
        launch.clicked.connect(self._launch)
        launch_row.addWidget(launch)
        launch_row.addStretch()
        layout.addLayout(launch_row)

        # Output
        out_box = QGroupBox("Play the voice to")
        out_layout = QVBoxLayout(out_box)
        self.output_group = QButtonGroup(self)
        for key, label in OUTPUT_MODES.items():
            rb = QRadioButton(label)
            rb.setChecked(self.cfg.output == key)
            rb.toggled.connect(lambda on, k=key: on and self._set(output=k))
            self.output_group.addButton(rb)
            out_layout.addWidget(rb)
        layout.addWidget(out_box)

        # Voice
        voice_box = QGroupBox("Voice")
        form = QFormLayout(voice_box)
        self.voice = QComboBox()
        for key, v in VOICES.items():
            self.voice.addItem(v.label, key)
        self.voice.setCurrentIndex(max(0, self.voice.findData(self.cfg.voice)))
        self.voice.currentIndexChanged.connect(
            lambda: self._set(voice=self.voice.currentData()))
        form.addRow("Personality", self.voice)
        self.style = QComboBox()
        for key, label in STYLES.items():
            self.style.addItem(label, key)
        self.style.setCurrentIndex(max(0, self.style.findData(self.cfg.style)))
        self.style.currentIndexChanged.connect(
            lambda: self._set(style=self.style.currentData()))
        form.addRow("Game", self.style)
        self.pitch = _slider(-12, 12, round(self.cfg.pitch))
        self.pitch_label = QLabel()
        self.pitch.valueChanged.connect(lambda v: self._set(pitch=float(v)))
        form.addRow("Pitch", self._with_label(self.pitch, self.pitch_label))
        self.speed = _slider(50, 200, round(self.cfg.speed * 100))
        self.speed_label = QLabel()
        self.speed.valueChanged.connect(lambda v: self._set(speed=v / 100))
        form.addRow("Speed", self._with_label(self.speed, self.speed_label))
        self.vc_volume = _slider(0, 150, round(self.cfg.vc_volume * 100))
        self.vc_volume_label = QLabel()
        self.vc_volume.valueChanged.connect(lambda v: self._set(vc_volume=v / 100))
        form.addRow("VC volume", self._with_label(self.vc_volume, self.vc_volume_label))
        self.local_volume = _slider(0, 150, round(self.cfg.local_volume * 100))
        self.local_volume_label = QLabel()
        self.local_volume.valueChanged.connect(lambda v: self._set(local_volume=v / 100))
        form.addRow("Speaker volume",
                    self._with_label(self.local_volume, self.local_volume_label))
        pack_row = QHBoxLayout()
        self.pack = QLineEdit(self.cfg.sample_pack)
        self.pack.setPlaceholderText("Built-in voice")
        self.pack.setToolTip("Folder with a.wav ... z.wav, or one WAV with the 26 "
                             "letters back to back (Classic only)")
        self.pack.editingFinished.connect(lambda: self._set(sample_pack=self.pack.text().strip()))
        browse = QPushButton("Browse")
        browse.clicked.connect(self._browse_pack)
        pack_row.addWidget(self.pack)
        pack_row.addWidget(browse)
        form.addRow("Letter samples", pack_row)
        layout.addWidget(voice_box)

        # Microphone
        mic_box = QGroupBox("Microphone")
        mic_layout = QVBoxLayout(mic_box)
        self.mix_mic = QCheckBox("Keep my real microphone in voice chat too")
        self.mix_mic.setChecked(self.cfg.mix_real_mic)
        self.mix_mic.toggled.connect(lambda v: self._set(mix_real_mic=v))
        mic_layout.addWidget(self.mix_mic)
        self.mic = QComboBox()
        self.mic.addItem("System default microphone", "")
        try:
            for name, desc in list_microphones():
                self.mic.addItem(desc, name)
        except AudioError:
            pass
        self.mic.setCurrentIndex(max(0, self.mic.findData(self.cfg.real_mic)))
        self.mic.currentIndexChanged.connect(lambda: self._set(real_mic=self.mic.currentData()))
        mic_layout.addWidget(self.mic)
        layout.addWidget(mic_box)

        # Test
        test_row = QHBoxLayout()
        self.test = QLineEdit("Hey there! How's it going?")
        self.test.returnPressed.connect(self._speak_test)
        speak = QPushButton("Speak")
        speak.clicked.connect(self._speak_test)
        stop = QPushButton("Stop")
        stop.clicked.connect(self.engine.stop_speaking)
        test_row.addWidget(self.test)
        test_row.addWidget(speak)
        test_row.addWidget(stop)
        layout.addLayout(test_row)

        self.log = QPlainTextEdit(readOnly=True)
        self.log.setMaximumBlockCount(200)
        self.log.setFixedHeight(110)
        layout.addWidget(self.log)

        self.setCentralWidget(root)
        self._log_seen = 0
        self._refresh_labels()
        bridge.status.connect(self._on_status)
        self._on_status(engine.status)

    @staticmethod
    def _with_label(slider: QSlider, label: QLabel) -> QWidget:
        w = QWidget()
        row = QHBoxLayout(w)
        row.setContentsMargins(0, 0, 0, 0)
        label.setMinimumWidth(56)
        row.addWidget(slider)
        row.addWidget(label)
        return w

    def _refresh_labels(self) -> None:
        self.pitch_label.setText(f"{self.cfg.pitch:+.0f} st")
        self.speed_label.setText(f"{self.cfg.speed * 100:.0f}%")
        self.vc_volume_label.setText(f"{self.cfg.vc_volume * 100:.0f}%")
        self.local_volume_label.setText(f"{self.cfg.local_volume * 100:.0f}%")
        self.mic.setEnabled(self.cfg.mix_real_mic)

    def _set(self, **changes) -> None:
        self.cfg = replace(self.cfg, **changes)
        self.engine.update_config(self.cfg)
        self._refresh_labels()
        self._save_timer.start()

    def _save(self) -> None:
        try:
            self.cfg.save()
        except OSError as e:
            self.log.appendPlainText(f"could not save settings: {e}")

    def _speak_test(self) -> None:
        text = self.test.text().strip()
        if text:
            self.engine.say(text, force=True)

    def _browse_pack(self) -> None:
        folder = QFileDialog.getExistingDirectory(self, "Letter sample folder",
                                                  str(Path.home()))
        if folder:
            self.pack.setText(folder)
            self._set(sample_pack=folder)

    def _launch(self) -> None:
        subprocess.Popen([sys.executable, "-m", "nightcap_ac", "launch"],
                         cwd=str(Path(__file__).resolve().parent.parent),
                         start_new_session=True)

    def _on_status(self, status: Status) -> None:
        if status.nightcap_pid:
            box = "chat box open" if status.text_box_open else "waiting for chat"
            self.hook_label.setText(f"Nightcap: connected (pid {status.nightcap_pid}), {box}")
        else:
            self.hook_label.setText("Nightcap: not connected. Start it with "
                                    "Launch Nightcap.")
        if status.error:
            self.vc_label.setText(f"Voice chat: {status.error}")
        elif not self.cfg.uses_vc or not self.cfg.enabled:
            self.vc_label.setText("Voice chat: off")
        elif status.routed_streams:
            self.vc_label.setText("Voice chat: Nightcap's mic is on the Animalese mic")
        elif status.vc_ready:
            self.vc_label.setText("Voice chat: virtual mic ready, waiting for "
                                  "Nightcap's mic (join a voice server)")
        else:
            self.vc_label.setText("Voice chat: starting")
        if status.last_message:
            self.last_label.setText(f"Last spoken: {status.last_message}")
        fresh = min(status.log_seq - self._log_seen, len(status.log))
        for line in status.log[len(status.log) - fresh:]:
            self.log.appendPlainText(line)
        self._log_seen = status.log_seq

    def closeEvent(self, event) -> None:  # noqa: N802 (Qt naming)
        if QSystemTrayIcon.isSystemTrayAvailable() and getattr(self, "tray", None):
            self.hide()
            event.ignore()
        else:
            event.accept()


def run() -> int:
    app = QApplication(sys.argv)
    app.setApplicationName("Nightcap AC")
    app.setDesktopFileName("nightcap-ac")
    bridge = _Bridge()
    engine = Engine(Config.load(), bridge.status.emit)
    try:
        engine.start()
    except RuntimeError as e:
        QMessageBox.information(None, "Nightcap AC", str(e))
        return 1
    except AudioError as e:
        QMessageBox.warning(None, "Nightcap AC", str(e))
    window = Window(engine, bridge)

    if QSystemTrayIcon.isSystemTrayAvailable():
        tray = QSystemTrayIcon(QIcon.fromTheme("audio-input-microphone"), app)
        menu = QMenu()
        show = QAction("Settings", menu)
        show.triggered.connect(window.showNormal)
        toggle = QAction("Enabled", menu, checkable=True, checked=window.cfg.enabled)
        toggle.toggled.connect(window.enabled.setChecked)
        window.enabled.toggled.connect(toggle.setChecked)
        quit_action = QAction("Quit", menu)
        quit_action.triggered.connect(app.quit)
        menu.addActions([show, toggle, quit_action])
        tray.setContextMenu(menu)
        tray.setToolTip("Nightcap AC")
        tray.activated.connect(lambda reason: reason == QSystemTrayIcon.ActivationReason.Trigger
                               and window.showNormal())
        tray.show()
        window.tray = tray

    window.show()
    app.aboutToQuit.connect(engine.stop)
    import signal
    signal.signal(signal.SIGINT, lambda *_: app.quit())
    signal.signal(signal.SIGTERM, lambda *_: app.quit())
    # Let Python see signals while Qt's loop is running.
    poke = QTimer(interval=300)
    poke.timeout.connect(lambda: None)
    poke.start()
    return app.exec()
