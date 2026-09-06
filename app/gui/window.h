#pragma once
#include <QMainWindow>
#include "control_client.h"
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSystemTrayIcon;
class QCheckBox;
class QAction;
class QCloseEvent;
class QTabWidget;
class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(bool smokeTest = false);
public slots:
    void ShowWindow();
    void RunInBackground();
    void Quit();
protected:
    void closeEvent(QCloseEvent* event) override;
private slots:
    void ApplyStatus(const QVariantMap& status);
private:
    bool ConfirmWifi(bool pairing);
    QString Mode() const;
    ControlClient m_client;
    QTabWidget* m_tabs;
    QComboBox *m_interface, *m_mode;
    QLineEdit *m_code, *m_endpoint;
    QLabel *m_status, *m_message, *m_description, *m_hint, *m_details, *m_pairHint;
    QLabel *m_gamepadState, *m_gamepadPhase, *m_gamepadMode, *m_gamepadIface;
    QLabel *m_appName, *m_appLock, *m_appLastSeen, *m_appSocket, *m_appIdleLogo, *m_appLogo;
    QWidget* m_pairSymbols;
    QPushButton *m_start, *m_stop, *m_pair, *m_copy, *m_prepare;
    QCheckBox* m_background;
    QSystemTrayIcon* m_tray;
    QAction *m_trayStart, *m_trayStop;
    QMap<QString,QLabel*> m_health;
    QVariantMap m_lastStatus;
    QString m_operationError;
    QString m_appLogoSource;
    bool m_pending = false;
    bool m_smokeTest = false, m_backgroundNotice = false;
    bool m_quitting = false;
};
