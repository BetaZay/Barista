#include "cafe_theme.h"
#include <QPalette>
#include <QWidget>

void ApplyCafeTheme(QWidget* widget)
{
    QPalette colors = widget->palette();
    colors.setColor(QPalette::Window,QColor("#f5ecdf"));
    colors.setColor(QPalette::WindowText,QColor("#38261a"));
    colors.setColor(QPalette::Base,QColor("#fff8ef"));
    colors.setColor(QPalette::AlternateBase,QColor("#efe0cd"));
    colors.setColor(QPalette::Text,QColor("#38261a"));
    colors.setColor(QPalette::Button,QColor("#f8eee1"));
    colors.setColor(QPalette::ButtonText,QColor("#38261a"));
    colors.setColor(QPalette::Highlight,QColor("#70513a"));
    colors.setColor(QPalette::HighlightedText,QColor("#ffffff"));
    colors.setColor(QPalette::Mid,QColor("#bea48c"));
    colors.setColor(QPalette::Disabled,QPalette::Text,QColor("#99816d"));
    colors.setColor(QPalette::Disabled,QPalette::ButtonText,QColor("#99816d"));
    widget->setPalette(colors);
    widget->setStyleSheet(R"(
        QWidget { color: #38261a; font-size: 13px; }
        QMainWindow, QDialog, QWidget[cafePage="true"] { background: #f5ecdf; }
        QWidget#sidebar { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #2e1f17,stop:1 #1a120e); border-right: 1px solid #503727; }
        QWidget#homePage { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #faf3e8,stop:1 #ecdfcf); }
        QWidget#homePage QLabel { color: #38261a; }
        QLabel { background: transparent; }
        QLabel[heading="true"] { font-size: 21px; font-weight: 600; }
        QLabel[subheading="true"] { font-size: 14px; font-weight: 600; }
        QLabel#homeHeading { font-size: 29px; font-weight: 650; }
        QLabel#homeStatus { font-size: 21px; font-weight: 600; }
        QLabel#homeHint, QLabel#applicationSummary, QLabel#homeDeviceDetail { font-size: 12px; color: #78604b; }
        QLabel#deviceTitle { font-size: 14px; font-weight: 600; }
        QLabel#applicationName { font-size: 13px; font-weight: 600; }
        QFrame#readyCard { background: #f7eee2; border: 1px solid #deccba; border-radius: 13px; }
        QFrame#applicationCard { background: transparent; border: 0; }
        QFrame#waitingAdapterCard { background: #fff8ef; border: 1px solid #deccba; border-radius: 10px; }
        QPushButton { background: #fff8ef; border: 1px solid #cbb59f; border-radius: 7px; padding: 8px 14px; qproperty-iconSize: 20px 20px; }
        QPushButton:hover { background: #ead8c2; }
        QPushButton:disabled { color: #a18f7c; border-color: #dfd0bf; background: #eee4d7; }
        QPushButton:focus, QComboBox:focus, QLineEdit:focus, QListWidget:focus { border: 1px solid #00aeed; }
        QPushButton[primary="true"] { background: #e6d1b9; color: #38261a; padding: 11px 18px; }
        QPushButton[primary="true"]:disabled { background: #eee4d7; color: #a18f7c; border-color: #dfd0bf; }
        QPushButton[nav="true"] { background: transparent; border: 1px solid transparent; text-align: left; padding: 13px 12px; color: #d5bda8; qproperty-iconSize: 24px 24px; }
        QPushButton[nav="true"]:checked { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #624330,stop:1 #422d22); border: 1px solid #674b38; color: #fff7ee; }
        QPushButton[nav="true"]:hover { background: #402c20; }
        QPushButton[link="true"] { color: #806049; background: transparent; border: 0; padding: 5px; }
        QPushButton#addGamePadButton { border: 1px dashed #987452; padding: 20px; background: transparent; }
        QPushButton:checked { background: #e6d1b9; border-color: #a48769; }
        QComboBox, QLineEdit, QListWidget, QPlainTextEdit { background: #fff8ef; color: #38261a; border: 1px solid #cbb59f; border-radius: 6px; padding: 7px; selection-background-color: #70513a; }
        QComboBox:disabled, QLineEdit:disabled { color: #99816d; }
        QComboBox { padding-right: 34px; }
        QComboBox::drop-down { subcontrol-origin: padding; subcontrol-position: top right; width: 30px; border: 0; background: transparent; }
        QComboBox::down-arrow { image: url(:/barista/icons/dropdown.svg); width: 12px; height: 12px; }
        QDialog#pairingDialog { background: qlineargradient(x1:0,y1:0,x2:1,y2:1,stop:0 #faf3e8,stop:1 #ecdfcf); }
        QDialog#pairingDialog QLabel { color: #38261a; }
        QDialog#pairingDialog QLabel#pairingHint { color: #78604b; font-size: 12px; }
        QDialog#pairingDialog QLabel#pairingStage { font-size: 18px; }
        QDialog#pairingDialog QLabel[pairSymbol="true"] { font-size: 54px; background: #fff8ef; border: 1px solid #deccba; border-radius: 12px; }
        QListWidget#savedGamePads { background: transparent; border: 0; padding: 0; }
        QListWidget::item { padding: 14px 10px; border-bottom: 1px solid #deccba; }
        QListWidget#savedGamePads::item { background: #fff8ef; border: 1px solid #deccba; border-radius: 9px; padding: 10px; }
        QListWidget#savedGamePads::item:selected { background: #ead8c2; border: 1px solid #ac8d6e; }
        QListWidget::item:selected { background: #ead8c2; color: #38261a; border-radius: 6px; }
        QListWidget { selection-color: #38261a; selection-background-color: #ead8c2; }
        QComboBox QAbstractItemView { background: #fff8ef; color: #38261a; selection-background-color: #70513a; }
        QScrollArea, QTabWidget::pane { border: 0; background: #f5ecdf; }
        QTabWidget#settingsTabs QTabBar::tab { background: #eee1d0; color: #78604b; border: 0; border-bottom: 2px solid transparent; padding: 15px 20px; }
        QTabWidget#settingsTabs QTabBar::tab:selected { background: #f5ecdf; color: #38261a; border-bottom-color: #9b7350; }
        QTabWidget#settingsTabs QTabBar::tab:hover { background: #ead8c2; }
        QScrollBar:vertical { background: #f5ecdf; width: 10px; margin: 0; }
        QScrollBar::handle:vertical { background: #c4ab92; min-height: 28px; border-radius: 4px; }
        QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }
        QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }
        QGroupBox { border: 1px solid #d5bfaa; border-radius: 9px; margin-top: 16px; padding: 16px 10px 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 6px; }
        QMenuBar, QMenu { background: #fff8ef; color: #38261a; }
        QMenu::item:selected { background: #ead8c2; }
        QToolTip { background: #f3e3cf; color: #38261a; border: 1px solid #806047; }
    )");
}
