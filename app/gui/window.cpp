#include "window.h"
#include "barista/controller.h"
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <array>

namespace {
enum class Tone { Neutral, Good, Warning, Bad };

void SetTone(QLabel* label, Tone tone, bool bold = false)
{
    QString color = "palette(windowText)";
    if (tone == Tone::Good) color = "#207a3b";
    if (tone == Tone::Warning) color = "#946200";
    if (tone == Tone::Bad) color = "#b3261e";
    label->setStyleSheet(QString("color: %1;%2").arg(color, bold ? " font-weight: 600;" : ""));
}

void ConfigureForm(QFormLayout* form)
{
    form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
    form->setRowWrapPolicy(QFormLayout::WrapLongRows);
    form->setHorizontalSpacing(12);
    form->setVerticalSpacing(10);
}

QLabel* FormHint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text,parent);
    label->setWordWrap(true);
    label->setStyleSheet("color: palette(mid);");
    return label;
}
}

Window::Window(bool smokeTest)
{
    m_smokeTest = smokeTest;
    m_client.setParent(this);
    setWindowTitle("Barista");
    resize(760,620);
    setMinimumSize(620,500);

    m_status = new QLabel("Starting Barista…",this);
    m_status->setObjectName("sessionStatus");
    m_status->setContentsMargins(10,3,10,3);
    m_status->setMinimumHeight(m_status->fontMetrics().height() + 8);
    SetTone(m_status,Tone::Warning,true);
    statusBar()->addWidget(m_status,1);

    auto* sessionMenu = menuBar()->addMenu("&Session");
    auto* startAction = sessionMenu->addAction("&Start");
    auto* stopAction = sessionMenu->addAction("S&top");
    sessionMenu->addSeparator();
    auto* pairAction = sessionMenu->addAction("&Pair GamePad…");
    sessionMenu->addSeparator();
    auto* quitAction = sessionMenu->addAction("&Quit Barista",this,&Window::Quit);
    quitAction->setShortcut(QKeySequence::Quit);
    auto* helpMenu = menuBar()->addMenu("&Help");
    helpMenu->addAction("&About Barista",this,[this] {
        QMessageBox::about(this,"About Barista","Barista connects a Wii U GamePad to your computer.");
    });

    auto* root = new QWidget(this);
    auto* layout = new QVBoxLayout(root);
    layout->setContentsMargins(10,10,10,10);
    layout->setSpacing(8);
    m_message = new QLabel(root);
    m_message->setWordWrap(true);
    m_message->setStyleSheet("color: palette(highlight);");
    m_message->hide();
    layout->addWidget(m_message);

    m_tabs = new QTabWidget(root);
    m_tabs->setDocumentMode(true);
    layout->addWidget(m_tabs,1);

    auto* connection = new QWidget(m_tabs);
    auto* connectionLayout = new QVBoxLayout(connection);
    connectionLayout->setContentsMargins(12,12,12,12);
    connectionLayout->setSpacing(8);
    m_hint = new QLabel(connection);
    m_hint->setObjectName("connectionHint");
    m_hint->setWordWrap(true);
    connectionLayout->addWidget(m_hint);
    auto* choices = new QFormLayout;
    ConfigureForm(choices);
    m_interface = new QComboBox(connection);
    m_interface->setEditable(true);
    for (const auto& interface : QNetworkInterface::allInterfaces())
        if (interface.type() == QNetworkInterface::Wifi) m_interface->addItem(interface.name());
    if (!m_interface->count()) m_interface->addItem("wlan0");
    m_mode = new QComboBox(connection);
    m_mode->addItem("Screen + controller","real");
    m_mode->addItem("Controller only","controller");
    if (!smokeTest) {
        QSettings settings;
        m_interface->setCurrentText(settings.value("interface",m_interface->currentText()).toString());
        m_mode->setCurrentIndex(std::max(0,m_mode->findData(settings.value("mode","real"))));
    }
    choices->addRow("&Wi-Fi adapter:",m_interface);
    choices->addRow("&Use GamePad as:",m_mode);
    auto* sessionControls = new QWidget(connection);
    auto* sessionLayout = new QHBoxLayout(sessionControls);
    sessionLayout->setContentsMargins(0,0,0,0);
    sessionLayout->setSpacing(8);
    m_start = new QPushButton("Start",sessionControls);
    m_start->setObjectName("startButton");
    m_stop = new QPushButton("Stop",sessionControls);
    m_stop->setObjectName("stopButton");
    sessionLayout->addWidget(m_start);
    sessionLayout->addWidget(m_stop);
    sessionLayout->addStretch();
    choices->addRow("Session:",sessionControls);
    connectionLayout->addLayout(choices);
    m_description = new QLabel(connection);
    m_description->setWordWrap(true);
    connectionLayout->addWidget(m_description);
    connectionLayout->addStretch();
    m_tabs->addTab(connection,"Connection");

    auto* pairing = new QWidget(m_tabs);
    auto* pairingLayout = new QVBoxLayout(pairing);
    pairingLayout->setContentsMargins(12,12,12,12);
    pairingLayout->setSpacing(12);
    pairingLayout->addWidget(new QLabel("Press SYNC on the back of the GamePad, then select its four symbols below.",pairing));
    auto* pairingForm = new QFormLayout;
    ConfigureForm(pairingForm);
    m_pairSymbols = new QWidget(pairing);
    auto* symbols = new QHBoxLayout(m_pairSymbols);
    symbols->setContentsMargins(0,0,0,0);
    m_code = new QLineEdit("2220",pairing); m_code->hide();
    const QStringList shapes{"♠  Spade","♥  Heart","♦  Diamond","♣  Club"};
    std::array<QComboBox*,4> symbolChoices{};
    for (size_t i=0; i<symbolChoices.size(); ++i) {
        auto* choice = new QComboBox(m_pairSymbols);
        choice->addItems(shapes);
        choice->setCurrentIndex(i == 3 ? 0 : 2);
        choice->setAccessibleName(QString("Pairing symbol %1").arg(i+1));
        symbolChoices[i] = choice;
        symbols->addWidget(choice);
    }
    for (auto* choice : symbolChoices)
        connect(choice,qOverload<int>(&QComboBox::currentIndexChanged),this,[this,symbolChoices] {
            QString code;
            for (auto* symbol : symbolChoices) code += QString::number(symbol->currentIndex());
            m_code->setText(code);
            ApplyStatus(m_lastStatus);
        });
    pairingForm->addRow("Pairing symbols:",m_pairSymbols);
    pairingLayout->addLayout(pairingForm);
    m_pair = new QPushButton("Pair GamePad…",pairing);
    m_pair->setObjectName("pairButton");
    pairingLayout->addWidget(m_pair,0,Qt::AlignLeft);
    m_pairHint = new QLabel(pairing);
    m_pairHint->setWordWrap(true);
    pairingLayout->addWidget(m_pairHint);
    pairingLayout->addStretch();
    m_tabs->addTab(pairing,"Pair GamePad");

    auto* advanced = new QWidget(m_tabs);
    advanced->setObjectName("advancedPanel");
    auto* advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(12,12,12,12);
    advancedLayout->setSpacing(12);
    advancedLayout->addWidget(FormHint("Normal use is automatic. Use these checks only when troubleshooting.",advanced));
    auto* healthForm = new QFormLayout;
    ConfigureForm(healthForm);
    healthForm->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    const QList<QPair<QString,QString>> checks{
        {"service","Barista service:"}, {"networkManagerRunning","NetworkManager:"},
        {"polkitRunning","Authorization service:"}, {"engineInstalled","Radio engine:"},
        {"hostapdInstalled","Wi-Fi helper:"}, {"controllerSupported","Virtual controller:"},
        {"tools","Required tools:"}
    };
    for (const auto& [key,label] : checks) {
        auto* value = new QLabel("Checking…",advanced);
        value->setWordWrap(true);
        value->setMinimumWidth(300);
        value->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_health.insert(key,value);
        healthForm->addRow(label,value);
    }
    advancedLayout->addLayout(healthForm);
    auto* maintenance = new QDialogButtonBox(Qt::Horizontal,advanced);
    auto* refresh = maintenance->addButton("Check again",QDialogButtonBox::ActionRole);
    refresh->setObjectName("refreshButton");
    m_prepare = maintenance->addButton("Prepare system…",QDialogButtonBox::ActionRole);
    m_prepare->setObjectName("prepareButton");
    advancedLayout->addWidget(maintenance);
    connect(refresh,&QPushButton::clicked,this,[this] { m_operationError.clear(); m_message->hide(); m_client.Retry(); });
    connect(m_prepare,&QPushButton::clicked,this,[this] {
        if (QMessageBox::question(this,"Prepare system?",
            "Start NetworkManager if needed and load virtual-controller support? This can affect existing network connections. No packages will be installed and no GamePad session will start.",
            QMessageBox::Ok | QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Ok) return;
        m_message->hide();
        m_client.Prepare();
    });
    m_background = new QCheckBox("Keep running when the window is closed",advanced);
    m_background->setObjectName("backgroundCheck");
    m_background->setChecked(smokeTest || QSettings().value("background",true).toBool());
    advancedLayout->addWidget(m_background);
    connect(m_background,&QCheckBox::toggled,this,[smokeTest](bool enabled) {
        if (!smokeTest) QSettings().setValue("background",enabled);
    });
    auto* connectorForm = new QFormLayout;
    ConfigureForm(connectorForm);
    m_endpoint = new QLineEdit(advanced);
    m_endpoint->setReadOnly(true);
    m_endpoint->setObjectName("mediaEndpoint");
    m_endpoint->setPlaceholderText("Available while Screen + controller is running");
    connectorForm->addRow("App socket:",m_endpoint);
    m_copy = new QPushButton("Copy AppHook launch prefix",advanced);
    connectorForm->addRow(QString(),m_copy);
    advancedLayout->addLayout(connectorForm);
    m_details = new QLabel(advanced);
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    advancedLayout->addWidget(m_details);
    advancedLayout->addStretch();
    m_tabs->addTab(advanced,"Advanced");

    setCentralWidget(root);

    auto describe = [this] {
        m_description->setText(Mode() == "real"
            ? "A compatible AppHook client can use the GamePad screen, audio and input. The Barista logo is shown until an app supplies video."
            : "Buttons and sticks appear as a virtual controller for PC games. The GamePad shows the Barista logo. Touch, motion and rumble are not supported yet.");
    };
    connect(m_mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[describe](int) { describe(); });
    describe();

    connect(startAction,&QAction::triggered,m_start,&QPushButton::click);
    connect(stopAction,&QAction::triggered,m_stop,&QPushButton::click);
    connect(pairAction,&QAction::triggered,this,[this] { m_tabs->setCurrentIndex(1); m_pair->setFocus(); });
    connect(m_start,&QPushButton::clicked,this,[this] {
        ShowWindow();
        if (!ConfirmWifi(false)) return;
        QSettings settings;
        settings.setValue("interface",m_interface->currentText());
        settings.setValue("mode",Mode());
        m_message->hide();
        m_client.Start(m_interface->currentText(),Mode());
    });
    connect(m_stop,&QPushButton::clicked,&m_client,&ControlClient::Stop);
    connect(m_pair,&QPushButton::clicked,this,[this] {
        if (!ConfirmWifi(true)) return;
        m_message->hide();
        m_client.Pair(m_interface->currentText(),m_code->text(),Mode());
    });
    connect(m_copy,&QPushButton::clicked,this,[this] {
        QApplication::clipboard()->setText("env BARISTA_MUG_SOCKET=" + m_endpoint->text() + " ");
        m_message->setText("Copied the AppHook launch prefix. Run the client as your normal user.");
        m_message->show();
    });

    m_tray = new QSystemTrayIcon(QIcon(":/barista/barista-logo.png"),this);
    auto* trayMenu = new QMenu(this);
    trayMenu->addAction("Open Barista",this,&Window::ShowWindow);
    trayMenu->addSeparator();
    m_trayStart = trayMenu->addAction("Start",m_start,&QPushButton::click);
    m_trayStop = trayMenu->addAction("Stop",m_stop,&QPushButton::click);
    trayMenu->addSeparator();
    trayMenu->addAction("Quit Barista",this,&Window::Quit);
    m_tray->setContextMenu(trayMenu);
    m_tray->setToolTip("Barista");
    connect(m_tray,&QSystemTrayIcon::activated,this,[this](QSystemTrayIcon::ActivationReason reason) {
        if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) ShowWindow();
    });
    if (!smokeTest) m_tray->show();

    connect(&m_client,&ControlClient::Status,this,&Window::ApplyStatus);
    connect(&m_client,&ControlClient::Error,this,[this](const QString& error) {
        if (!isVisible()) ShowWindow();
        m_operationError = error;
        m_message->setText("That didn't complete. See Advanced for details.");
        m_message->show();
        ApplyStatus(m_lastStatus);
    });
    connect(&m_client,&ControlClient::Pending,this,[this](bool pending) {
        m_pending = pending;
        if (pending) m_operationError.clear();
        ApplyStatus(m_lastStatus);
    });
    connect(m_mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int) { ApplyStatus(m_lastStatus); });
    ApplyStatus({{"activating",!smokeTest}});
    if (!smokeTest) {
        auto* timer = new QTimer(this);
        connect(timer,&QTimer::timeout,this,[this] {
            m_client.Refresh();
            if (!isVisible() && !QSystemTrayIcon::isSystemTrayAvailable()) showMinimized();
        });
        timer->start(1000);
        m_client.Refresh();
    }
}

void Window::ShowWindow() { showNormal(); raise(); activateWindow(); }
void Window::closeEvent(QCloseEvent* event)
{
    if (m_quitting) { event->accept(); return; }
    event->ignore();
    if (!m_background->isChecked()) { Quit(); return; }
    RunInBackground();
}
void Window::RunInBackground()
{
    if (!m_smokeTest && QSystemTrayIcon::isSystemTrayAvailable()) {
        hide();
        if (!m_backgroundNotice) {
            m_tray->showMessage("Barista is still running","Your session continues. Open Barista from the tray or app launcher; choose Quit to stop it.");
            m_backgroundNotice = true;
        }
    } else {
        showMinimized();
        m_message->setText("Barista is running in the background. Use Quit Barista from the menu to stop and exit."); m_message->show();
    }
}
void Window::Quit()
{
    if ((m_lastStatus.value("running").toBool() && m_lastStatus.value("ownedByCaller").toBool()) || m_pending) {
        ShowWindow();
        if (QMessageBox::question(this,"Quit Barista?","Quit and stop your GamePad session? The Wi-Fi adapter will be released.",
            QMessageBox::Yes | QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
    }
    // The service's owner-disconnect watcher stops the engine even on a crash.
    // Hiding/minimizing keeps that same D-Bus owner alive; quitting does not.
    m_quitting = true;
    QApplication::quit();
}
bool Window::ConfirmWifi(bool pairing)
{
    if (!barista::ValidInterface(m_interface->currentText().toStdString())) {
        m_message->setText("Choose a valid Wi-Fi adapter in General first."); m_message->show(); return false;
    }
    QMessageBox warning(QMessageBox::Warning, pairing ? "Pair your GamePad?" : "Start Barista?",
        QString("Barista will take over Wi-Fi adapter %1 for your GamePad. Internet access through this adapter will be interrupted. Use Ethernet or another Wi-Fi adapter to stay online.")
            .arg(m_interface->currentText()), QMessageBox::NoButton, this);
    warning.setInformativeText(pairing
        ? "This can replace your saved pairing and disconnect the GamePad from its Wii U. Stop releases the adapter; you may need to reconnect to your Wi-Fi network."
        : "Stop releases the adapter; you may need to reconnect to your Wi-Fi network. Barista starts NetworkManager and loads controller support if needed. Your desktop may ask for permission.");
    auto* proceed = warning.addButton(pairing ? "Pair GamePad" : "Start",QMessageBox::AcceptRole);
    auto* cancel = warning.addButton(QMessageBox::Cancel);
    warning.setDefaultButton(cancel);
    warning.setEscapeButton(cancel);
    warning.exec();
    return warning.clickedButton() == proceed;
}
QString Window::Mode() const { return m_mode->currentData().toString(); }
void Window::ApplyStatus(const QVariantMap& status)
{
    m_lastStatus = status;
    const bool available = status.value("available").toBool();
    const bool running = status.value("running").toBool();
    const bool busy = m_pending || status.value("busy").toBool();
    const bool owned = status.value("ownedByCaller").toBool();
    const bool connected = status.value("connected").toBool();
    const auto phase = status.value("phase").toString();
    const bool activating = status.value("activating").toBool();
    Tone sessionTone = Tone::Neutral;
    QString sessionText;
    QString hint;
    if (activating) {
        sessionText = "● Starting service"; sessionTone = Tone::Warning;
        hint = "Starting Barista in the background…";
    } else if (!available) {
        sessionText = "● Service unavailable"; sessionTone = Tone::Bad;
        hint = "Automatic startup did not complete. Open Advanced to check setup or retry.";
    } else if (phase == "stopping") {
        sessionText = "● Stopping"; sessionTone = Tone::Warning;
        hint = "Releasing the Wi-Fi adapter…";
    } else if (busy) {
        sessionText = "● Preparing"; sessionTone = Tone::Warning;
        hint = "Complete any permission prompt, then allow a moment for setup.";
    } else if (running && !owned) {
        sessionText = "● GamePad in use"; sessionTone = Tone::Warning;
        hint = "Another Barista window owns the current session.";
    } else if (connected) {
        sessionText = "● GamePad connected"; sessionTone = Tone::Good;
        hint = "Connected. Start your supported app when you are ready.";
    } else if (running) {
        sessionText = "● GamePad not connected"; sessionTone = Tone::Bad;
        hint = "Turn on your paired GamePad and keep it nearby.";
    } else {
        sessionText = "● Ready";
        hint = "Select Start for a paired GamePad, or open Pair GamePad for a new one.";
    }
    m_status->setText(sessionText);
    SetTone(m_status,sessionTone,true);
    m_hint->setText(hint);
    SetTone(m_hint,sessionTone);
    const bool supported = Mode() != "controller" || status.value("controllerSupported").toBool() ||
        status.value("controllerSetupAvailable").toBool();
    m_start->setEnabled(available && !running && !busy && supported);
    m_pair->setEnabled(available && !running && !busy && supported &&
        barista::ValidPairCode(m_code->text().toStdString()));
    m_stop->setEnabled(available && running && !busy && owned && phase != "stopping");
    m_trayStart->setEnabled(m_start->isEnabled());
    m_trayStop->setEnabled(m_stop->isEnabled());
    m_tray->setToolTip("Barista — " + m_status->text());
    m_prepare->setEnabled(available && !running && !busy && status.value("setupSupported").toBool());
    m_health["service"]->setText(available ? "Ready" : activating ? "Starting…" : "Not available");
    SetTone(m_health["service"],available ? Tone::Good : activating ? Tone::Warning : Tone::Bad);
    for (const auto& key : {"networkManagerRunning","polkitRunning","engineInstalled","hostapdInstalled","controllerSupported"}) {
        QString text = "Not checked";
        Tone tone = Tone::Neutral;
        if (available) {
            const bool ready = status.value(key).toBool();
            if (QString(key) == "networkManagerRunning") { text = ready ? "Running" : "Will start when needed"; tone = ready ? Tone::Good : Tone::Warning; }
            else if (QString(key) == "polkitRunning") { text = ready ? "Running" : "Not available"; tone = ready ? Tone::Good : Tone::Bad; }
            else if (QString(key) == "controllerSupported") {
                text = ready ? "Ready" : status.value("controllerSetupAvailable").toBool() ? "Can prepare" : "Unavailable";
                tone = ready ? Tone::Good : status.value("controllerSetupAvailable").toBool() ? Tone::Warning : Tone::Bad;
            } else { text = ready ? "Installed" : "Not available"; tone = ready ? Tone::Good : Tone::Bad; }
        }
        m_health[key]->setText(text);
        SetTone(m_health[key],tone);
    }
    const auto missing = status.value("missingTools").toStringList();
    m_health["tools"]->setText(!available ? "Not checked" : missing.isEmpty() ? "Ready" : "Missing: " + missing.join(", "));
    SetTone(m_health["tools"],!available ? Tone::Neutral : missing.isEmpty() ? Tone::Good : Tone::Bad);
    m_interface->setEnabled(!running && !busy);
    m_mode->setEnabled(!running && !busy);
    m_pairSymbols->setEnabled(!running && !busy);
    m_pairHint->setText(!available ? "Service unavailable — check Advanced." :
        busy ? "Waiting for setup to finish." :
        running ? "Stop the current session before pairing." :
        !supported ? "Controller support is unavailable; choose Screen + controller or check Advanced." : "");
    m_pairHint->setVisible(!m_pairHint->text().isEmpty());
    SetTone(m_pairHint,!available || !supported ? Tone::Bad : Tone::Warning);
    m_endpoint->setText(status.value("mediaEndpoint").toString());
    m_copy->setEnabled(!m_endpoint->text().isEmpty());
    const auto error = m_operationError.isEmpty() ? status.value("error").toString() : m_operationError;
    if (!error.isEmpty()) m_details->setText(error);
    else if (available && !supported) m_details->setText("Controller support cannot be prepared automatically. Check kernel module tools and uinput support.");
    else if (available) m_details->setText(QString("Service: %1\nSession: %2\nMode: %3")
        .arg(status.value("platform").toString(),phase,status.value("mode").toString()));
    if (available && !error.isEmpty()) {
        m_hint->setText("The last operation reported a problem. See Advanced for details before trying again.");
        SetTone(m_hint,Tone::Bad);
    }
}
