#pragma once
#include <QString>
#include <QColor>

namespace TshStyle {

// ── Color Palette ────────────────────────────────────────────────────────────
static const QColor BG_BASE        { "#0f1117" };
static const QColor BG_SIDEBAR     { "#13151c" };
static const QColor BG_CARD        { "#1c1f2b" };
static const QColor BG_CARD2       { "#22263a" };
static const QColor BG_INPUT       { "#1a1d2e" };
static const QColor BORDER         { "#2a2d3e" };
static const QColor BORDER_HOVER   { "#3d4166" };
static const QColor ACCENT         { "#00b4d8" };
static const QColor ACCENT_DIM     { "#0077a8" };
static const QColor ACCENT_BRIGHT  { "#48cae4" };
static const QColor SUCCESS        { "#4ade80" };
static const QColor WARNING        { "#fbbf24" };
static const QColor DANGER         { "#f87171" };
static const QColor TEXT_PRIMARY   { "#e8eaf0" };
static const QColor TEXT_SECONDARY { "#8891b0" };
static const QColor TEXT_MUTED     { "#4a5068" };

// ── Stylesheet ────────────────────────────────────────────────────────────────
inline QString appStyleSheet() {
    return R"(
/* ── Base ── */
QMainWindow, QWidget {
    background-color: #0f1117;
    color: #e8eaf0;
    font-family: "Inter", "Segoe UI", "SF Pro Display", sans-serif;
    font-size: 13px;
}

/* ── Scroll Bars ── */
QScrollBar:vertical {
    background: #13151c;
    width: 6px;
    margin: 0;
}
QScrollBar::handle:vertical {
    background: #2a2d3e;
    border-radius: 3px;
    min-height: 30px;
}
QScrollBar::handle:vertical:hover { background: #3d4166; }
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
QScrollBar:horizontal {
    background: #13151c;
    height: 6px;
    margin: 0;
}
QScrollBar::handle:horizontal {
    background: #2a2d3e;
    border-radius: 3px;
    min-width: 30px;
}
QScrollBar::handle:horizontal:hover { background: #3d4166; }
QScrollBar::add-line:horizontal, QScrollBar::sub-line:horizontal { width: 0; }

/* ── Labels ── */
QLabel { color: #e8eaf0; background: transparent; }

/* ── Line Edits / Inputs ── */
QLineEdit, QTextEdit, QPlainTextEdit, QSpinBox, QDoubleSpinBox, QComboBox {
    background-color: #1a1d2e;
    border: 1px solid #2a2d3e;
    border-radius: 6px;
    color: #e8eaf0;
    padding: 6px 10px;
    selection-background-color: #00b4d8;
    selection-color: #0f1117;
}
QLineEdit:focus, QTextEdit:focus, QPlainTextEdit:focus,
QSpinBox:focus, QDoubleSpinBox:focus {
    border-color: #00b4d8;
    outline: none;
}
QLineEdit:hover, QComboBox:hover { border-color: #3d4166; }
QComboBox::drop-down {
    border: none;
    padding-right: 8px;
}
QComboBox::down-arrow {
    image: none;
    width: 0; height: 0;
    border-left: 4px solid transparent;
    border-right: 4px solid transparent;
    border-top: 6px solid #8891b0;
}
QComboBox QAbstractItemView {
    background-color: #1c1f2b;
    border: 1px solid #2a2d3e;
    border-radius: 6px;
    selection-background-color: #00b4d8;
    selection-color: #0f1117;
    color: #e8eaf0;
    padding: 4px;
}
QSpinBox::up-button, QSpinBox::down-button,
QDoubleSpinBox::up-button, QDoubleSpinBox::down-button {
    background: #2a2d3e;
    border: none;
    border-radius: 3px;
}

/* ── Buttons ── */
QPushButton {
    background-color: #1c1f2b;
    border: 1px solid #2a2d3e;
    border-radius: 7px;
    color: #e8eaf0;
    padding: 7px 16px;
    font-weight: 500;
}
QPushButton:hover {
    background-color: #22263a;
    border-color: #3d4166;
}
QPushButton:pressed { background-color: #1a1d2e; }
QPushButton:disabled { color: #4a5068; border-color: #1c1f2b; }

QPushButton[role="primary"] {
    background-color: #00b4d8;
    border-color: #00b4d8;
    color: #0f1117;
    font-weight: 600;
}
QPushButton[role="primary"]:hover {
    background-color: #48cae4;
    border-color: #48cae4;
}
QPushButton[role="primary"]:pressed { background-color: #0077a8; }

QPushButton[role="danger"] {
    background-color: transparent;
    border-color: #f87171;
    color: #f87171;
}
QPushButton[role="danger"]:hover {
    background-color: #3d1515;
    border-color: #f87171;
}

QPushButton[role="success"] {
    background-color: transparent;
    border-color: #4ade80;
    color: #4ade80;
}
QPushButton[role="success"]:hover { background-color: #0d3020; }

/* ── Table Views ── */
QTableView, QTreeView {
    background-color: #1c1f2b;
    border: 1px solid #2a2d3e;
    border-radius: 8px;
    gridline-color: #22263a;
    color: #e8eaf0;
    selection-background-color: #0077a8;
    selection-color: #e8eaf0;
    alternate-background-color: #1f2236;
}
QTableView::item { padding: 6px 10px; border: none; }
QTableView::item:hover { background-color: #22263a; }
QTableView::item:selected { background-color: #0077a8; }
QHeaderView::section {
    background-color: #13151c;
    color: #8891b0;
    border: none;
    border-bottom: 1px solid #2a2d3e;
    padding: 8px 10px;
    font-weight: 600;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
}
QHeaderView::section:hover { background-color: #1c1f2b; color: #e8eaf0; }

/* ── Tab Widget ── */
QTabWidget::pane {
    background-color: #1c1f2b;
    border: 1px solid #2a2d3e;
    border-radius: 0 8px 8px 8px;
}
QTabBar::tab {
    background-color: #13151c;
    border: 1px solid #2a2d3e;
    border-bottom: none;
    border-radius: 6px 6px 0 0;
    color: #8891b0;
    padding: 8px 18px;
    margin-right: 2px;
    font-weight: 500;
}
QTabBar::tab:selected {
    background-color: #1c1f2b;
    color: #e8eaf0;
    border-bottom: 2px solid #00b4d8;
}
QTabBar::tab:hover:!selected { color: #c0c8e0; }

/* ── Group Box ── */
QGroupBox {
    background-color: #1c1f2b;
    border: 1px solid #2a2d3e;
    border-radius: 8px;
    margin-top: 16px;
    padding: 12px;
    font-weight: 600;
    color: #8891b0;
    font-size: 11px;
    text-transform: uppercase;
    letter-spacing: 0.5px;
}
QGroupBox::title {
    subcontrol-origin: margin;
    subcontrol-position: top left;
    padding: 0 8px;
    left: 12px;
}

/* ── Splitter ── */
QSplitter::handle {
    background-color: #2a2d3e;
}
QSplitter::handle:horizontal { width: 1px; }
QSplitter::handle:vertical { height: 1px; }

/* ── Menu / Context Menu ── */
QMenu {
    background-color: #1c1f2b;
    border: 1px solid #2a2d3e;
    border-radius: 8px;
    padding: 4px;
    color: #e8eaf0;
}
QMenu::item { padding: 6px 20px; border-radius: 4px; }
QMenu::item:selected { background-color: #0077a8; }
QMenu::separator { height: 1px; background: #2a2d3e; margin: 4px 8px; }

/* ── Tooltip ── */
QToolTip {
    background-color: #22263a;
    border: 1px solid #3d4166;
    border-radius: 6px;
    color: #e8eaf0;
    padding: 6px 10px;
    font-size: 12px;
}

/* ── CheckBox ── */
QCheckBox { color: #e8eaf0; spacing: 8px; }
QCheckBox::indicator {
    width: 16px; height: 16px;
    border: 1px solid #2a2d3e;
    border-radius: 4px;
    background: #1a1d2e;
}
QCheckBox::indicator:checked {
    background-color: #00b4d8;
    border-color: #00b4d8;
    image: none;
}
QCheckBox::indicator:hover { border-color: #3d4166; }

/* ── Radio Button ── */
QRadioButton { color: #e8eaf0; spacing: 8px; }
QRadioButton::indicator {
    width: 16px; height: 16px;
    border: 1px solid #2a2d3e;
    border-radius: 8px;
    background: #1a1d2e;
}
QRadioButton::indicator:checked {
    background-color: #00b4d8;
    border-color: #00b4d8;
}

/* ── Slider ── */
QSlider::groove:horizontal {
    height: 4px;
    background: #2a2d3e;
    border-radius: 2px;
}
QSlider::handle:horizontal {
    background: #00b4d8;
    border: none;
    width: 14px; height: 14px;
    border-radius: 7px;
    margin: -5px 0;
}
QSlider::sub-page:horizontal {
    background: #00b4d8;
    border-radius: 2px;
}

/* ── Progress Bar ── */
QProgressBar {
    background-color: #1a1d2e;
    border: 1px solid #2a2d3e;
    border-radius: 4px;
    text-align: center;
    color: #e8eaf0;
    font-size: 11px;
    height: 8px;
}
QProgressBar::chunk {
    background: qlineargradient(x1:0, y1:0, x2:1, y2:0,
        stop:0 #0077a8, stop:1 #00b4d8);
    border-radius: 3px;
}

/* ── Status Bar ── */
QStatusBar {
    background-color: #13151c;
    border-top: 1px solid #2a2d3e;
    color: #8891b0;
    font-size: 12px;
}

/* ── Message Box ── */
QMessageBox { background-color: #1c1f2b; }
QMessageBox QLabel { color: #e8eaf0; }
)";
}

} // namespace TshStyle
