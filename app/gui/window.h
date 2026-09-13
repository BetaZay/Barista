#pragma once
#include <QMainWindow>
#include "api/types.h"
#include "control_client.h"
class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSystemTrayIcon;
class QCheckBox;
class QAction;
class QCloseEvent;
class QTabWidget;
class QDialog;
class Window : public QMainWindow {
    Q_OBJECT
public:
    explicit Window(bool smokeTest = false);
public slots:
    void ShowWindow();
    void RunInBackground();
    void Quit();
    void OpenPairing();
protected:
    void closeEvent(QCloseEvent* event) override;
private slots:
    void ApplyStatus(const barista::api::SessionStatus& status);
private:
    bool ConfirmWifi(bool pairing, const QString& interface);
    barista::api::SessionMode Mode() const;
    void ApplyGamePads(const std::vector<barista::api::GamePad>& gamePads);
    void RefreshSavedGamePads();
    void RefreshDiagnostics();
    void ViewSelectedLog();
    void InitializePairingPattern();
    void UpdateHomeDevice();
    ControlClient m_client;
    QTabWidget *m_tabs, *m_settingsTabs;
    QComboBox *m_interface, *m_mode, *m_logFiles;
    QLineEdit *m_code, *m_country, *m_endpoint;
    QLabel *m_status, *m_message, *m_description, *m_hint, *m_details, *m_pairStatus, *m_supportId;
    QLabel *m_gamepadState, *m_gamepadPhase, *m_gamepadMode, *m_gamepadIface, *m_gamepadBattery;
    QLabel *m_appName, *m_appLock, *m_appLastSeen, *m_appSocket, *m_appIdleLogo, *m_appLogo;
    QLabel* m_appSummary;
    QLabel *m_homeStatus, *m_deviceTitle, *m_homeDeviceDetail, *m_waitingStatus, *m_waitingAdapter;
    QLabel* m_noGamePads;
    QDialog *m_pairDialog, *m_waitingDialog;
    std::array<QLabel*,4> m_pairSymbolLabels{};
    QWidget* m_pairSymbols;
    QPushButton *m_start, *m_stop, *m_pair, *m_copy, *m_prepare;
    QPushButton *m_screenMode, *m_controllerMode;
    QPushButton* m_waitingStop;
    QPushButton *m_renamePair, *m_removePair;
    QPushButton *m_viewLog, *m_openLogs, *m_copyDiagnostics, *m_saveDiagnostics;
    QCheckBox* m_background;
    QSystemTrayIcon* m_tray;
    QAction *m_trayStart, *m_trayStop;
    QListWidget* m_savedGamePads;
    QMap<QString,QLabel*> m_health;
    barista::api::SessionStatus m_lastStatus;
    QString m_operationError;
    QString m_logDirectory, m_supportReport;
    QString m_appLogoSource;
    bool m_pending = false;
    bool m_pairingRequested = false;
    bool m_smokeTest = false, m_backgroundNotice = false;
    bool m_quitting = false;
};
