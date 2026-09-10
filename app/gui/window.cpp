#include "window.h"
#include "api/diagnostics.h"
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QColor>
#include <QDialogButtonBox>
#include <QDialog>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListWidget>
#include <QInputDialog>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QImage>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QDir>
#include <array>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

namespace {
enum class Tone { Neutral, Good, Warning, Bad };

QString InterfaceName(const QComboBox* combo)
{
    if (combo->isEditable() && barista::api::ValidInterfaceName(combo->currentText().toStdString()))
        return combo->currentText();
    if (combo->isEditable() && combo->currentIndex() >= 0 &&
        combo->currentText() != combo->itemText(combo->currentIndex()))
        return combo->currentText();
    const QString data = combo->currentData().toString();
    return data.isEmpty() ? combo->currentText() : data;
}

void SelectInterface(QComboBox* combo, const QString& interface)
{
    const int index = combo->findData(interface);
    if (index >= 0) combo->setCurrentIndex(index);
    else combo->setCurrentText(interface);
}

QString AdapterLabel(const QString& interface)
{
    const QString device = QFileInfo("/sys/class/net/" + interface + "/device").canonicalFilePath();
    const QString driver = QFileInfo("/sys/class/net/" + interface + "/device/driver").canonicalFilePath();
    const bool usb = device.contains("/usb",Qt::CaseInsensitive) || driver.contains("8821au",Qt::CaseInsensitive);
    const QString model = driver.contains("8852be",Qt::CaseInsensitive) ? "Realtek RTL8852BE" :
        driver.contains("8821au",Qt::CaseInsensitive) || driver.contains("8821a",Qt::CaseInsensitive)
            ? "Realtek RTL8821AU" : "Wi-Fi adapter";
    return QString("%1 · %2 (%3)").arg(usb ? "USB" : "Internal",model,interface);
}

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

QString SavedGamePadsFile()
{
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/barista-wiiu/gamepads.ini";
}

QMap<QString,QString> LoadSavedGamePadsCache()
{
    QMap<QString,QString> records;
    QSettings settings(SavedGamePadsFile(),QSettings::IniFormat);
    const int count = settings.beginReadArray("gamepads");
    for (int i = 0; i < count; ++i) {
        settings.setArrayIndex(i);
        const QString mac = settings.value("mac").toString();
        if (!mac.isEmpty()) records.insert(mac,settings.value("name").toString());
    }
    settings.endArray();
    return records;
}

void SaveSavedGamePadsCache(const QMap<QString,QString>& records)
{
    const QFileInfo file(SavedGamePadsFile());
    QDir().mkpath(file.absolutePath());
    QSettings settings(file.absoluteFilePath(),QSettings::IniFormat);
    settings.remove("gamepads");
    settings.beginWriteArray("gamepads");
    int index = 0;
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        settings.setArrayIndex(index++);
        settings.setValue("mac",it.key());
        settings.setValue("name",it.value());
    }
    settings.endArray();
    settings.sync();
}

QLabel* FormHint(const QString& text, QWidget* parent)
{
    auto* label = new QLabel(text,parent);
    label->setWordWrap(true);
    label->setStyleSheet("color: palette(mid);");
    return label;
}

QPixmap AppLogoPreview(const QString& path)
{
    if (!path.endsWith(".i420")) return QPixmap(path);
    constexpr int width = 864, height = 480;
    constexpr qsizetype bytes = width * height * 3 / 2;
    QFile input(path);
    if (!input.open(QIODevice::ReadOnly) || input.size() != bytes) return {};
    const auto i420 = input.readAll();
    if (i420.size() != bytes) return {};
    const auto* data = reinterpret_cast<const uchar*>(i420.constData());
    const auto* u = data + width * height;
    const auto* v = u + width * height / 4;
    QImage image(width,height,QImage::Format_RGB32);
    for (int y = 0; y < height; ++y) {
        auto* output = reinterpret_cast<QRgb*>(image.scanLine(y));
        for (int x = 0; x < width; ++x) {
            const int c = std::max(0,int(data[y * width + x]) - 16);
            const int d = int(u[(y / 2) * (width / 2) + x / 2]) - 128;
            const int e = int(v[(y / 2) * (width / 2) + x / 2]) - 128;
            output[x] = qRgb(std::clamp((298 * c + 409 * e + 128) >> 8,0,255),
                std::clamp((298 * c - 100 * d - 208 * e + 128) >> 8,0,255),
                std::clamp((298 * c + 516 * d + 128) >> 8,0,255));
        }
    }
    return QPixmap::fromImage(image);
}

QString SuggestedRegulatoryCountry()
{
    return QLocale::territoryToCode(QLocale::system().territory()).toUpper();
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
    statusBar()->hide();

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
        QMessageBox::about(this,"About Barista",QString("Barista %1\n\nBarista connects a Wii U GamePad to your computer.").arg(qApp->applicationVersion()));
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
    connectionLayout->setSpacing(10);
    m_hint = new QLabel(connection);
    m_hint->setObjectName("connectionHint");
    m_hint->setWordWrap(true);
    connectionLayout->addWidget(m_hint);

    auto* gamepadHeader = new QLabel("<b>GamePad Status</b>", connection);
    connectionLayout->addWidget(gamepadHeader);
    auto* gamepadForm = new QFormLayout;
    ConfigureForm(gamepadForm);
    m_gamepadState = new QLabel("Session not started", connection);
    m_gamepadPhase = new QLabel("Idle", connection);
    m_gamepadMode = new QLabel("Screen + controller", connection);
    m_gamepadIface = new QLabel("—", connection);
    m_gamepadBattery = new QLabel("—", connection);
    m_gamepadBattery->setObjectName("gamepadBattery");
    gamepadForm->addRow("GamePad:", m_gamepadState);
    gamepadForm->addRow("Phase:", m_gamepadPhase);
    gamepadForm->addRow("Mode:", m_gamepadMode);
    gamepadForm->addRow("Interface:", m_gamepadIface);
    gamepadForm->addRow("Battery:", m_gamepadBattery);
    connectionLayout->addLayout(gamepadForm);

    auto* div1 = new QFrame(connection);
    div1->setFrameShape(QFrame::HLine);
    div1->setFrameShadow(QFrame::Sunken);
    connectionLayout->addWidget(div1);

    auto* appHeader = new QLabel("<b>Connected Application</b>", connection);
    connectionLayout->addWidget(appHeader);
    auto* appForm = new QFormLayout;
    ConfigureForm(appForm);
    m_appName = new QLabel("No application connected", connection);
    m_appLock = new QLabel("—", connection);
    m_appIdleLogo = new QLabel("—", connection);
    m_appLastSeen = new QLabel("—", connection);
    m_appSocket = new QLabel("—", connection);
    m_appSocket->setTextInteractionFlags(Qt::TextSelectableByMouse);
    appForm->addRow("Application:", m_appName);
    appForm->addRow("Access lock:", m_appLock);
    appForm->addRow("Idle logo:", m_appIdleLogo);
    appForm->addRow("Last activity:", m_appLastSeen);
    appForm->addRow("Media socket:", m_appSocket);
    auto* appDetails = new QWidget(connection);
    auto* appDetailsLayout = new QHBoxLayout(appDetails);
    appDetailsLayout->setContentsMargins(0,0,0,0);
    appDetailsLayout->setSpacing(16);
    appDetailsLayout->addLayout(appForm,1);
    m_appLogo = new QLabel(appDetails);
    m_appLogo->setObjectName("connectedAppLogo");
    m_appLogo->setFixedSize(76,76);
    m_appLogo->setAlignment(Qt::AlignCenter);
    m_appLogo->setStyleSheet("color: palette(mid); border: 1px solid palette(midlight); border-radius: 6px;");
    m_appLogo->setText("No app\nicon");
    appDetailsLayout->addWidget(m_appLogo,0,Qt::AlignTop);
    connectionLayout->addWidget(appDetails);

    auto* div2 = new QFrame(connection);
    div2->setFrameShape(QFrame::HLine);
    div2->setFrameShadow(QFrame::Sunken);
    connectionLayout->addWidget(div2);

    auto* sessionHeader = new QLabel("<b>Session Controls</b>", connection);
    connectionLayout->addWidget(sessionHeader);
    auto* choices = new QFormLayout;
    ConfigureForm(choices);
    m_interface = new QComboBox(connection);
    m_interface->setObjectName("interfaceCombo");
    m_interface->setEditable(true);
    for (const auto& interface : QNetworkInterface::allInterfaces())
        if (interface.type() == QNetworkInterface::Wifi)
            m_interface->addItem(AdapterLabel(interface.name()),interface.name());
    if (!m_interface->count()) m_interface->addItem("wlan0");
    m_mode = new QComboBox(connection);
    m_mode->addItem("Screen + controller","real");
    m_mode->addItem("Controller only","controller");
    if (!smokeTest) {
        QSettings settings;
        SelectInterface(m_interface,settings.value("interface",InterfaceName(m_interface)).toString());
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
    m_description = new QLabel(connection);
    m_description->setWordWrap(true);
    choices->addRow(QString(), m_description);
    connectionLayout->addLayout(choices);
    connectionLayout->addStretch();
    m_tabs->addTab(connection,"General");

    auto* pairing = new QWidget(m_tabs);
    auto* pairingLayout = new QVBoxLayout(pairing);
    pairingLayout->setContentsMargins(12,12,12,12);
    pairingLayout->setSpacing(12);
    pairingLayout->addWidget(new QLabel("Pair a GamePad to a dedicated Wi-Fi adapter. This can be different from the adapter used by General.",pairing));
    m_pairStatus = new QLabel(pairing);
    m_pairStatus->setObjectName("pairingStatus");
    m_pairStatus->setWordWrap(true);
    pairingLayout->addWidget(m_pairStatus);
    auto* pairingForm = new QFormLayout;
    ConfigureForm(pairingForm);
    m_pairInterface = new QComboBox(pairing);
    m_pairInterface->setObjectName("pairInterfaceCombo");
    m_pairInterface->setEditable(true);
    for (int i = 0; i < m_interface->count(); ++i)
        m_pairInterface->addItem(m_interface->itemText(i),m_interface->itemData(i));
    SelectInterface(m_pairInterface,InterfaceName(m_interface));
    pairingForm->addRow("&Wi-Fi adapter:",m_pairInterface);
    m_country = new QLineEdit(pairing);
    m_country->setObjectName("regulatoryCountry");
    m_country->setMaxLength(2);
    m_country->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z]{0,2}"),m_country));
    m_country->setPlaceholderText("System setting");
    if (!smokeTest)
        m_country->setText(QSettings().value("regulatoryCountry",SuggestedRegulatoryCountry()).toString().toUpper());
    connect(m_country,&QLineEdit::textEdited,this,[this](const QString& value) {
        const QString upper = value.toUpper();
        if (upper != value) m_country->setText(upper);
        ApplyStatus(m_lastStatus);
    });
    pairingForm->addRow("Regulatory &country:",m_country);
    auto* countryHelp = new QLabel("Barista suggests the two-letter country from your desktop locale. Confirm it matches your physical location; a temporary radio setting is restored when the session stops.",pairing);
    countryHelp->setWordWrap(true);
    pairingLayout->addWidget(countryHelp);
    pairingLayout->addWidget(new QLabel("Choose four symbols below. Once Barista says “Pair now,” press SYNC on the GamePad and enter the same symbols there. The GamePad submits automatically after the fourth symbol.",pairing));
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
    m_pair = new QPushButton("Start pairing",pairing);
    m_pair->setObjectName("pairButton");
    pairingLayout->addWidget(m_pair,0,Qt::AlignLeft);
    auto* savedPairs = new QGroupBox("Saved GamePads",pairing);
    auto* savedLayout = new QVBoxLayout(savedPairs);
    m_savedGamePads = new QListWidget(savedPairs);
    m_savedGamePads->setObjectName("savedGamePads");
    savedLayout->addWidget(m_savedGamePads);
    auto* savedButtons = new QHBoxLayout;
    auto* renamePair = new QPushButton("Rename",savedPairs);
    auto* removePair = new QPushButton("Remove",savedPairs);
    savedButtons->addWidget(renamePair); savedButtons->addWidget(removePair); savedButtons->addStretch();
    savedLayout->addLayout(savedButtons);
    connect(renamePair,&QPushButton::clicked,this,[this] {
        auto* item = m_savedGamePads->currentItem(); if (!item) return;
        const QString mac = item->data(Qt::UserRole).toString();
        bool ok = false; const QString name = QInputDialog::getText(this,"Rename GamePad","Name:",QLineEdit::Normal,item->text().section(" — ",0,0),&ok);
        if (!ok) return;
        m_client.RenameGamePad({mac.toStdString(), name.toStdString()});
        auto records = LoadSavedGamePadsCache(); records.insert(mac,name); SaveSavedGamePadsCache(records);
        ApplyGamePads({});
    });
    connect(removePair,&QPushButton::clicked,this,[this] {
        auto* item = m_savedGamePads->currentItem(); if (!item) return;
        const QString mac = item->data(Qt::UserRole).toString();
        if (QMessageBox::question(this,"Remove saved GamePad?",QString("Remove %1? It will need to be paired again.").arg(item->text())) != QMessageBox::Yes) return;
        m_client.RemoveGamePad({mac.toStdString()});
        auto records = LoadSavedGamePadsCache(); records.remove(mac); SaveSavedGamePadsCache(records);
        ApplyGamePads({});
    });
    QTimer::singleShot(0,this,&Window::RefreshSavedGamePads);
    pairingLayout->addWidget(savedPairs);
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
    connect(refresh,&QPushButton::clicked,this,[this] {
        m_operationError.clear(); m_message->hide(); m_client.Retry(); RefreshDiagnostics();
    });
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

    auto* support = new QGroupBox("Support logs",advanced);
    auto* supportLayout = new QVBoxLayout(support);
    supportLayout->addWidget(FormHint(
        "These logs contain coded session events and system details that are safe to share. Network addresses, pairing codes, credentials and raw Wi-Fi-helper output are excluded.", support));
    m_supportId = new QLabel("Support ID: not available",support);
    m_supportId->setObjectName("supportId");
    m_supportId->setTextInteractionFlags(Qt::TextSelectableByMouse);
    supportLayout->addWidget(m_supportId);
    m_logFiles = new QComboBox(support);
    m_logFiles->setObjectName("supportLogFiles");
    m_logFiles->setSizeAdjustPolicy(QComboBox::AdjustToContents);
    supportLayout->addWidget(m_logFiles);
    auto* supportButtons = new QDialogButtonBox(Qt::Horizontal,support);
    m_viewLog = supportButtons->addButton("View log",QDialogButtonBox::ActionRole);
    m_viewLog->setObjectName("viewLogButton");
    m_openLogs = supportButtons->addButton("Open log folder",QDialogButtonBox::ActionRole);
    m_openLogs->setObjectName("openLogsButton");
    m_copyDiagnostics = supportButtons->addButton("Copy support report",QDialogButtonBox::ActionRole);
    m_copyDiagnostics->setObjectName("copyDiagnosticsButton");
    m_saveDiagnostics = supportButtons->addButton("Save support report…",QDialogButtonBox::ActionRole);
    m_saveDiagnostics->setObjectName("saveDiagnosticsButton");
    auto* refreshLogs = supportButtons->addButton("Refresh",QDialogButtonBox::ActionRole);
    refreshLogs->setObjectName("refreshLogsButton");
    supportLayout->addWidget(supportButtons);
    advancedLayout->addWidget(support);
    m_viewLog->setEnabled(false);
    m_openLogs->setEnabled(false);
    m_copyDiagnostics->setEnabled(false);
    m_saveDiagnostics->setEnabled(false);
    connect(m_viewLog,&QPushButton::clicked,this,&Window::ViewSelectedLog);
    connect(m_openLogs,&QPushButton::clicked,this,[this] {
        if (!m_logDirectory.isEmpty()) QDesktopServices::openUrl(QUrl::fromLocalFile(m_logDirectory));
    });
    connect(m_copyDiagnostics,&QPushButton::clicked,this,[this] {
        QApplication::clipboard()->setText(m_supportReport);
        m_message->setText("Support report copied. You can paste it into a bug report.");
        m_message->show();
    });
    connect(m_saveDiagnostics,&QPushButton::clicked,this,[this] {
        const QString suggested = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
            "/barista-support-" + QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss") + ".txt";
        const QString path = QFileDialog::getSaveFileName(this,"Save Barista support report",suggested,"Text files (*.txt)");
        if (path.isEmpty()) return;
        QFile output(path);
        if (!output.open(QIODevice::WriteOnly | QIODevice::Text) || output.write(m_supportReport.toUtf8()) < 0) {
            QMessageBox::warning(this,"Could not save report",output.errorString());
            return;
        }
        m_message->setText("Support report saved to " + path); m_message->show();
    });
    connect(refreshLogs,&QPushButton::clicked,this,&Window::RefreshDiagnostics);
    m_details = new QLabel(advanced);
    m_details->setObjectName("supportDetails");
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::PlainText);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    advancedLayout->addWidget(m_details);
    advancedLayout->addStretch();
    m_tabs->addTab(advanced,"Advanced");

    setCentralWidget(root);

    auto describe = [this] {
        m_description->setText(Mode() == barista::api::SessionMode::Real
            ? "A compatible AppHook client can use the GamePad screen, audio and input. The Barista logo is shown until an app supplies video."
            : "Buttons and sticks appear as a virtual controller for PC games. The GamePad shows the Barista logo. Touch, motion and rumble are not supported yet.");
    };
    connect(m_mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[describe](int) { describe(); });
    const auto synchronizeInterface = [this](QComboBox* source, QComboBox* destination) {
        const QString interface = InterfaceName(source);
        if (interface.isEmpty() || InterfaceName(destination) == interface)
        {
            ApplyStatus(m_lastStatus);
            return;
        }
        SelectInterface(destination,interface);
        ApplyStatus(m_lastStatus);
    };
    connect(m_interface,&QComboBox::currentTextChanged,this,[this,synchronizeInterface] {
        synchronizeInterface(m_interface,m_pairInterface);
    });
    connect(m_pairInterface,&QComboBox::currentTextChanged,this,[this,synchronizeInterface] {
        synchronizeInterface(m_pairInterface,m_interface);
    });
    describe();

    connect(startAction,&QAction::triggered,m_start,&QPushButton::click);
    connect(stopAction,&QAction::triggered,m_stop,&QPushButton::click);
    connect(pairAction,&QAction::triggered,this,[this] { m_tabs->setCurrentIndex(1); m_pair->setFocus(); });
    connect(m_start,&QPushButton::clicked,this,[this] {
        ShowWindow();
        const QString interface = InterfaceName(m_interface);
        if (!ConfirmWifi(false,interface)) return;
        QSettings settings;
        settings.setValue("interface",interface);
        settings.setValue("pairInterface",interface);
        settings.setValue("mode",QString::fromLatin1(barista::api::SessionModeName(Mode())));
        const QString country = m_country->text().trimmed().toUpper();
        settings.setValue("regulatoryCountry",country);
        m_message->hide();
        m_client.Start({interface.toStdString(), Mode(), country.toStdString()});
    });
    connect(m_stop,&QPushButton::clicked,&m_client,&ControlClient::Stop);
    connect(m_pair,&QPushButton::clicked,this,[this] {
        const QString interface = InterfaceName(m_pairInterface);
        if (!ConfirmWifi(true,interface)) return;
        QSettings settings;
        settings.setValue("interface",interface);
        settings.setValue("pairInterface",interface);
        const QString country = m_country->text().trimmed().toUpper();
        settings.setValue("regulatoryCountry",country);
        m_message->hide();
        m_pairingRequested = true;
        const auto code = barista::api::ParsePairCode(m_code->text().toStdString());
        if (!code) return;
        m_client.Pair({{interface.toStdString(), Mode(), country.toStdString()}, *code});
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
    connect(&m_client,&ControlClient::GamePads,this,&Window::ApplyGamePads);
    connect(&m_client,&ControlClient::Diagnostics,this,
        [this](const QString& report,const QString& directory,const QStringList& files,const QString& sessionId) {
            const QString selected = m_logFiles->currentText();
            m_supportReport = report; m_logDirectory = directory;
            m_logFiles->clear(); m_logFiles->addItems(files);
            const int previous = m_logFiles->findText(selected);
            if (previous >= 0) m_logFiles->setCurrentIndex(previous);
            m_viewLog->setEnabled(!files.isEmpty());
            m_openLogs->setEnabled(!directory.isEmpty());
            m_copyDiagnostics->setEnabled(!report.isEmpty());
            m_saveDiagnostics->setEnabled(!report.isEmpty());
            m_supportId->setText(sessionId.isEmpty() ? "Support ID: not available" : "Support ID: " + sessionId.left(8));
        });
    connect(&m_client,&ControlClient::Error,this,[this](const QString& error) {
        m_pairingRequested = false;
        if (!isVisible()) ShowWindow();
        const QString code = QString::fromLatin1(barista::api::ClassifyDiagnosticMessage(error.toStdString()));
        const auto advice = barista::api::AdviceForDiagnostic(code.toStdString());
        m_operationError = error + "\n\nCode: " + code + "\nTry this: " +
            QString::fromUtf8(advice.action.data(),static_cast<qsizetype>(advice.action.size()));
        m_message->setText("That didn't complete. See Advanced for details.");
        m_message->show();
        RefreshDiagnostics();
        ApplyStatus(m_lastStatus);
    });
    connect(&m_client,&ControlClient::Pending,this,[this](bool pending) {
        m_pending = pending;
        if (pending) m_operationError.clear();
        ApplyStatus(m_lastStatus);
    });
    connect(&m_client,&ControlClient::Stopped,this,[this](bool) {
        if (m_quitting) QApplication::quit();
    });
    connect(m_mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int) { ApplyStatus(m_lastStatus); });
    connect(m_tabs,&QTabWidget::currentChanged,this,[this](int index) {
        if (!m_smokeTest && m_tabs->widget(index)->objectName() == "advancedPanel") RefreshDiagnostics();
    });
    barista::api::SessionStatus initialStatus;
    initialStatus.activating = !smokeTest;
    ApplyStatus(initialStatus);
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
    if ((m_lastStatus.running && m_lastStatus.ownedByCaller) || m_pending) {
        ShowWindow();
        if (QMessageBox::question(this,"Quit Barista?","Quit and stop your GamePad session? The Wi-Fi adapter will be released.",
            QMessageBox::Yes | QMessageBox::Cancel,QMessageBox::Cancel) != QMessageBox::Yes) return;
    }
    m_quitting = true;
    if (m_lastStatus.running || m_pending) {
        m_client.Stop();
        return;
    }
    QApplication::quit();
}
bool Window::ConfirmWifi(bool pairing, const QString& interface)
{
    if (!barista::api::ValidInterfaceName(interface.toStdString())) {
        m_message->setText("Choose a valid Wi-Fi adapter first."); m_message->show(); return false;
    }
    QMessageBox warning(QMessageBox::Warning, pairing ? "Pair your GamePad?" : "Start Barista?",
        QString("Barista will take over Wi-Fi adapter %1 for your GamePad. Internet access through this adapter will be interrupted. Use Ethernet or another Wi-Fi adapter to stay online.")
            .arg(interface), QMessageBox::NoButton, this);
    QString information = pairing
        ? "This can replace your saved pairing and disconnect the GamePad from its Wii U. Stop releases the adapter; you may need to reconnect to your Wi-Fi network."
        : "Stop releases the adapter; you may need to reconnect to your Wi-Fi network. Barista starts NetworkManager and loads controller support if needed. Your desktop may ask for permission.";
    const QString country = m_country->text().trimmed().toUpper();
    if (!country.isEmpty())
        information += QString(" If the system is using the world regulatory domain, Barista will temporarily apply %1 system-wide and restore the prior setting when this session stops. Confirm %1 matches your physical location.").arg(country);
    warning.setInformativeText(information);
    auto* proceed = warning.addButton(pairing ? "Pair GamePad" : "Start",QMessageBox::AcceptRole);
    auto* cancel = warning.addButton(QMessageBox::Cancel);
    warning.setDefaultButton(cancel);
    warning.setEscapeButton(cancel);
    warning.exec();
    return warning.clickedButton() == proceed;
}
barista::api::SessionMode Window::Mode() const
{
    return barista::api::ParseSessionMode(m_mode->currentData().toString().toStdString())
        .value_or(barista::api::SessionMode::Real);
}
void Window::RefreshSavedGamePads()
{
    ApplyGamePads({});
    m_client.RefreshGamePads();
}
void Window::RefreshDiagnostics()
{
    if (!m_smokeTest) m_client.RefreshDiagnostics();
}

void Window::ViewSelectedLog()
{
    const QString name = m_logFiles->currentText();
    if (m_logDirectory.isEmpty() || name.isEmpty() || QFileInfo(name).fileName() != name) return;
    QFile input(QDir(m_logDirectory).filePath(name));
    if (!input.open(QIODevice::ReadOnly | QIODevice::Text)) {
        QMessageBox::warning(this,"Could not open log",input.errorString());
        return;
    }
    QDialog viewer(this);
    viewer.setWindowTitle(name);
    auto* layout = new QVBoxLayout(&viewer);
    auto* text = new QPlainTextEdit(QString::fromUtf8(input.readAll()),&viewer);
    text->setReadOnly(true);
    text->setLineWrapMode(QPlainTextEdit::NoWrap);
    layout->addWidget(text);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close,&viewer);
    connect(buttons,&QDialogButtonBox::rejected,&viewer,&QDialog::reject);
    layout->addWidget(buttons);
    viewer.resize(820,560);
    viewer.exec();
}
void Window::ApplyGamePads(const std::vector<barista::api::GamePad>& gamePads)
{
    auto records = LoadSavedGamePadsCache();
    for (const auto& gamePad : gamePads)
        records.insert(QString::fromStdString(gamePad.mac), QString::fromStdString(gamePad.name));
    SaveSavedGamePadsCache(records);
    m_savedGamePads->clear();
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        const QString name = it.value();
        auto* item = new QListWidgetItem(name.isEmpty() ? it.key() : name + " — " + it.key(),m_savedGamePads);
        item->setData(Qt::UserRole,it.key());
    }
}
void Window::ApplyStatus(const barista::api::SessionStatus& status)
{
    const auto previousPhase = m_lastStatus.phase;
    const bool previousRunning = m_lastStatus.running;
    m_lastStatus = status;
    const bool available = status.available;
    const bool running = status.running;
    const bool busy = m_pending || status.busy;
    const bool owned = status.ownedByCaller;
    const bool connected = status.gamePadConnected;
    const auto phase = status.phase;
    const QString phaseText = QString::fromLatin1(barista::api::SessionPhaseName(phase));
    if (previousRunning && !running) RefreshDiagnostics();
    if (m_pairingRequested && phase == barista::api::SessionPhase::Runtime &&
        previousPhase != barista::api::SessionPhase::Runtime) {
        RefreshSavedGamePads();
        m_pairingRequested = false;
        m_tabs->setCurrentIndex(0);
        m_message->setText("GamePad paired successfully. A session is now running.");
        m_message->show();
        QTimer::singleShot(6000,this,[this] {
            if (m_message->text() == "GamePad paired successfully. A session is now running.")
                m_message->hide();
        });
    }
    const bool activating = status.activating;
    Tone sessionTone = Tone::Neutral;
    QString sessionText;
    QString hint;
    if (activating) {
        sessionText = "● Starting service"; sessionTone = Tone::Warning;
        hint = "Starting Barista in the background…";
    } else if (!available) {
        sessionText = "● Service unavailable"; sessionTone = Tone::Bad;
        hint = "Automatic startup did not complete. Open Advanced to check setup or retry.";
    } else if (phase == barista::api::SessionPhase::Stopping) {
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

    const auto sessionMode = status.mode.value_or(barista::api::SessionMode::Real);
    const auto ifaceName = QString::fromStdString(status.interfaceName);
    m_gamepadPhase->setText(running ? phaseText : "Idle");
    m_gamepadMode->setText(sessionMode == barista::api::SessionMode::Controller ? "Controller only" : "Screen + controller");
    m_gamepadIface->setText(ifaceName.isEmpty() ? InterfaceName(m_interface) : ifaceName);
    if (status.batteryPercent)
        m_gamepadBattery->setText(QString("%1%").arg(*status.batteryPercent));
    else
        m_gamepadBattery->setText("—");
    if (connected) {
        m_gamepadState->setText("● Connected (5 GHz GamePad Wi-Fi)");
        SetTone(m_gamepadState, Tone::Good, true);
    } else if (running) {
        m_gamepadState->setText("● Waiting for GamePad (searching…)");
        SetTone(m_gamepadState, Tone::Bad, true);
    } else if (phase == barista::api::SessionPhase::Pairing) {
        m_gamepadState->setText("● Pairing mode active");
        SetTone(m_gamepadState, Tone::Warning, true);
    } else {
        m_gamepadState->setText("● Session not started");
        SetTone(m_gamepadState, Tone::Neutral, false);
    }

    bool appConnected = status.application.connected;
    QString appName = QString::fromStdString(status.application.name);
    qint64 appPid = status.application.pid;
    qint64 appLastSeen = static_cast<qint64>(status.application.lastSeen);
    QString appIdleLogo = QString::fromStdString(status.application.idleLogo);
    const QString mediaEndpoint = QString::fromStdString(status.mediaEndpoint);

    // Fallback: check lock file directly if mediaEndpoint is known
    if (!appConnected && !mediaEndpoint.isEmpty())
    {
        QFile file(mediaEndpoint + ".lock");
        if (file.open(QIODevice::ReadOnly | QIODevice::Text))
        {
            while (!file.atEnd())
            {
                const QString line = QString::fromUtf8(file.readLine()).trimmed();
                const int eq = line.indexOf('=');
                if (eq > 0)
                {
                    const QString k = line.left(eq), v = line.mid(eq + 1);
                    if (k == "app") appName = v;
                    else if (k == "pid") appPid = v.toLongLong();
                    else if (k == "last_seen") appLastSeen = v.toLongLong();
                    else if (k == "idle_logo" || k == "logo") appIdleLogo = v;
                }
            }
            if (appPid > 0) appConnected = true;
        }
    }

    if (appIdleLogo.isEmpty() && !mediaEndpoint.isEmpty() && QFile::exists(mediaEndpoint + ".idle.i420"))
    {
        appIdleLogo = mediaEndpoint + ".idle.i420";
    }

    auto updateAppLogo = [this,appConnected,appIdleLogo] {
        const QString source = appConnected ? appIdleLogo : QString();
        if (source == m_appLogoSource) return;
        m_appLogoSource = source;
        const QPixmap logo = source.isEmpty() ? QPixmap{} : AppLogoPreview(source);
        if (logo.isNull()) {
            m_appLogo->setPixmap({});
            m_appLogo->setText(appConnected ? "No app\nicon" : "No app\nconnected");
            return;
        }
        m_appLogo->setText({});
        m_appLogo->setPixmap(logo.scaled(m_appLogo->size(),Qt::KeepAspectRatio,Qt::SmoothTransformation));
    };
    updateAppLogo();

    if (!running) {
        m_appName->setText("No session running");
        SetTone(m_appName, Tone::Neutral, false);
        m_appLock->setText("—");
        SetTone(m_appLock, Tone::Neutral, false);
        if (m_appIdleLogo) m_appIdleLogo->setText("—");
        m_appLastSeen->setText("—");
        m_appSocket->setText("—");
    } else if (sessionMode == barista::api::SessionMode::Controller) {
        m_appName->setText("Virtual PC Controller (uinput)");
        SetTone(m_appName, Tone::Good, false);
        m_appLock->setText("Not applicable in controller mode");
        SetTone(m_appLock, Tone::Neutral, false);
        if (m_appIdleLogo) m_appIdleLogo->setText("Default (Barista)");
        m_appLastSeen->setText("Active");
        m_appSocket->setText("Internal controller bridge");
    } else if (appConnected) {
        m_appName->setText(QString("%1 (PID %2)").arg(appName.isEmpty() ? "Connected App" : appName).arg(appPid));
        SetTone(m_appName, Tone::Good, true);
        m_appLock->setText(QString("Locked by %1").arg(appName.isEmpty() ? "active app" : appName));
        SetTone(m_appLock, Tone::Good, false);
        if (m_appIdleLogo) {
            m_appIdleLogo->setText(appIdleLogo.isEmpty() ? "Default (Barista)" : QFileInfo(appIdleLogo).fileName());
        }
        qint64 now = QDateTime::currentSecsSinceEpoch();
        qint64 diff = (appLastSeen > 0 && now >= appLastSeen) ? (now - appLastSeen) : 0;
        m_appLastSeen->setText(diff == 0 ? "Active just now" : QString("%1s ago").arg(diff));
        m_appSocket->setText(mediaEndpoint.isEmpty() ? "Active" : mediaEndpoint);
    } else {
        m_appName->setText("Waiting for application (e.g. Cemu)");
        SetTone(m_appName, Tone::Warning, false);
        m_appLock->setText("Unlocked");
        SetTone(m_appLock, Tone::Neutral, false);
        if (m_appIdleLogo) {
            m_appIdleLogo->setText("Default (Barista)");
        }
        m_appLastSeen->setText("—");
        m_appSocket->setText(mediaEndpoint.isEmpty() ? "Listening" : mediaEndpoint);
    }
    const bool supported = Mode() != barista::api::SessionMode::Controller || status.capabilities.controller ||
        status.capabilities.controllerSetup;
    const std::string country = m_country->text().trimmed().toUpper().toStdString();
    const bool validCountry = country.empty() || barista::api::ValidRegulatoryCountry(country);
    m_start->setEnabled(available && !running && !busy && supported && validCountry);
    m_pair->setEnabled(available && !running && !busy && supported &&
        barista::api::ParsePairCode(m_code->text().toStdString()).has_value() &&
        validCountry);
    m_stop->setEnabled(available && running && !busy && owned && phase != barista::api::SessionPhase::Stopping);
    m_trayStart->setEnabled(m_start->isEnabled());
    m_trayStop->setEnabled(m_stop->isEnabled());
    m_tray->setToolTip("Barista — " + m_status->text());
    m_prepare->setEnabled(available && !running && !busy && status.capabilities.systemPreparation);
    m_health["service"]->setText(available ? "Ready" : activating ? "Starting…" : "Not available");
    SetTone(m_health["service"],available ? Tone::Good : activating ? Tone::Warning : Tone::Bad);
    const std::array healthChecks{
        std::pair{"networkManagerRunning", status.health.networkManagerRunning},
        std::pair{"polkitRunning", status.health.authorizationRunning},
        std::pair{"engineInstalled", status.health.engineInstalled},
        std::pair{"hostapdInstalled", status.health.hostapdInstalled},
        std::pair{"controllerSupported", status.capabilities.controller},
    };
    for (const auto& [key,ready] : healthChecks) {
        QString text = "Not checked";
        Tone tone = Tone::Neutral;
        if (available) {
            if (QString(key) == "networkManagerRunning") { text = ready ? "Running" : "Will start when needed"; tone = ready ? Tone::Good : Tone::Warning; }
            else if (QString(key) == "polkitRunning") { text = ready ? "Running" : "Not available"; tone = ready ? Tone::Good : Tone::Bad; }
            else if (QString(key) == "controllerSupported") {
                text = ready ? "Ready" : status.capabilities.controllerSetup ? "Can prepare" : "Unavailable";
                tone = ready ? Tone::Good : status.capabilities.controllerSetup ? Tone::Warning : Tone::Bad;
            } else { text = ready ? "Installed" : "Not available"; tone = ready ? Tone::Good : Tone::Bad; }
        }
        m_health[key]->setText(text);
        SetTone(m_health[key],tone);
    }
    QStringList missing;
    for (const auto& tool : status.health.missingTools)
        missing.push_back(QString::fromStdString(tool));
    m_health["tools"]->setText(!available ? "Not checked" : missing.isEmpty() ? "Ready" : "Missing: " + missing.join(", "));
    SetTone(m_health["tools"],!available ? Tone::Neutral : missing.isEmpty() ? Tone::Good : Tone::Bad);
    m_interface->setEnabled(!running && !busy);
    m_mode->setEnabled(!running && !busy);
    m_pairSymbols->setEnabled(!running && !busy);
    const QString pairInterface = InterfaceName(m_pairInterface);
    QString pairStatus;
    Tone pairStatusTone = Tone::Neutral;
    if (!available) {
        pairStatus = "Pairing unavailable — check the service status in Advanced.";
        pairStatusTone = Tone::Bad;
    } else if (busy || phase == barista::api::SessionPhase::Starting) {
        pairStatus = QString("Starting pairing on %1… Preparing the Wi-Fi adapter. Wait for “Pair now” before using SYNC.")
            .arg(pairInterface);
        pairStatusTone = Tone::Warning;
    } else if (phase == barista::api::SessionPhase::Pairing) {
        pairStatus = QString("Pair now — the pairing network is ready on %1. Press SYNC on the GamePad and enter the four symbols selected above. The GamePad submits automatically after the fourth symbol.")
            .arg(pairInterface);
        pairStatusTone = Tone::Good;
    } else if (status.error && status.error->diagnosticCode == "AP_REGULATORY_BLOCKED") {
        pairStatus = "Pairing cannot start because the system blocks 5 GHz access-point channels. Configure the Wi-Fi regulatory domain for your actual country, then reconnect the adapter and try again.";
        pairStatusTone = Tone::Bad;
    } else if (running) {
        pairStatus = "Pairing is unavailable while another GamePad session is running.";
        pairStatusTone = Tone::Warning;
    } else if (!supported) {
        pairStatus = "Pairing cannot start in the selected mode. Check Advanced or select Screen + controller.";
        pairStatusTone = Tone::Bad;
    } else {
        pairStatus = QString("Ready to start pairing on %1. Choose four symbols, then select Start pairing.")
            .arg(pairInterface);
    }
    m_pairStatus->setText(pairStatus);
    SetTone(m_pairStatus,pairStatusTone,true);
    m_pair->setText(busy || phase == barista::api::SessionPhase::Starting ? "Starting pairing…" :
        phase == barista::api::SessionPhase::Pairing ? "Pairing active" : "Start pairing");
    m_pairInterface->setEnabled(!running && !busy);
    m_endpoint->setText(mediaEndpoint);
    m_copy->setEnabled(!m_endpoint->text().isEmpty());
    const auto error = m_operationError.isEmpty()
        ? status.error ? QString::fromStdString(status.error->message) : QString()
        : m_operationError;
    if (!error.isEmpty()) {
        QString details = error;
        if (status.error && !status.error->diagnosticCode.empty())
            details += "\n\nCode: " + QString::fromStdString(status.error->diagnosticCode);
        if (status.error && !status.error->action.empty())
            details += "\nTry this: " + QString::fromStdString(status.error->action);
        m_details->setText(details);
    }
    else if (available && !supported) m_details->setText("Controller support cannot be prepared automatically. Check kernel module tools and uinput support.");
    else if (available) m_details->setText(QString("Service: %1\nSession: %2\nMode: %3")
        .arg(QString::fromStdString(status.platform),phaseText,
            QString::fromLatin1(barista::api::SessionModeName(sessionMode))));
    if (available && !error.isEmpty()) {
        m_hint->setText("The last operation reported a problem. See Advanced for details before trying again.");
        SetTone(m_hint,Tone::Bad);
    }
}
