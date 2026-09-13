#include "window.h"
#include "cafe_icons.h"
#include "cafe_theme.h"
#include "pairing_pattern.h"
#include "api/diagnostics.h"
#include <QApplication>
#include <QButtonGroup>
#include <QTabBar>
#include <QScrollArea>
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
#include <QGraphicsOpacityEffect>
#include <QPropertyAnimation>
#include <QShowEvent>
#include <QHideEvent>
#include <QDateTime>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QListWidget>
#include <QInputDialog>
#include <QMenu>
#include <QAction>
#include <QMessageBox>
#include <QNetworkInterface>
#include <QPlainTextEdit>
#include <QPixmap>
#include <QPainter>
#include <QImage>
#include <QPushButton>
#include <QRegularExpression>
#include <QRegularExpressionValidator>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QStackedWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QDir>
#include <array>
#include <QFile>
#include <QFileInfo>
#include <QUrl>

namespace {
class PairingProgressLabel final : public QLabel
{
public:
    explicit PairingProgressLabel(QWidget* parent) : QLabel(parent)
    {
        m_opacity = new QGraphicsOpacityEffect(this);
        m_opacity->setOpacity(1.0);
        setGraphicsEffect(m_opacity);
        m_breathing = new QPropertyAnimation(m_opacity,"opacity",this);
        m_breathing->setObjectName("pairingBreathing");
        m_breathing->setDuration(4000);
        m_breathing->setStartValue(1.0);
        m_breathing->setEndValue(0.55);
        // A full, smoothly eased fade out and back in, without disappearing.
        m_breathing->setEasingCurve(QEasingCurve::SineCurve);
        m_breathing->setLoopCount(-1);
    }

    void SetBreathing(bool enabled)
    {
        m_enabled = enabled;
        UpdateAnimation();
    }

protected:
    void showEvent(QShowEvent* event) override
    {
        QLabel::showEvent(event);
        UpdateAnimation();
    }

    void hideEvent(QHideEvent* event) override
    {
        QLabel::hideEvent(event);
        m_breathing->stop();
        m_opacity->setOpacity(1.0);
    }

private:
    void UpdateAnimation()
    {
        if (m_enabled && isVisible())
        {
            if (m_breathing->state() != QAbstractAnimation::Running) m_breathing->start();
        }
        else
        {
            m_breathing->stop();
            m_opacity->setOpacity(1.0);
        }
    }

    QGraphicsOpacityEffect* m_opacity;
    QPropertyAnimation* m_breathing;
    bool m_enabled = false;
};

class PairingDialog final : public QDialog
{
public:
    explicit PairingDialog(QWidget* parent) : QDialog(parent) {}

protected:
    void paintEvent(QPaintEvent*) override
    {
        // Paint the complete client surface, independently of native dialog themes.
        QPainter painter(this);
        QLinearGradient cream(0,0,width(),height());
        cream.setColorAt(0,QColor("#faf3e8"));
        cream.setColorAt(1,QColor("#ecdfcf"));
        painter.fillRect(rect(),cream);
    }
};

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
    else if (barista::api::ValidInterfaceName(interface.toStdString())) {
        combo->addItem(interface + " (saved adapter)",interface);
        combo->setCurrentIndex(combo->count() - 1);
    }
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
    bool light = true;
    for (auto* parent = label->parentWidget(); parent; parent = parent->parentWidget())
        if (parent->objectName() == "sidebar") light = false;
    QString color = light ? "#5c4432" : "#eddfcd";
    if (tone == Tone::Good) color = light ? "#17613e" : "#a4c49a";
    if (tone == Tone::Warning) color = light ? "#865411" : "#e4bc7b";
    if (tone == Tone::Bad) color = light ? "#9b382a" : "#efaaa0";
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
    label->setStyleSheet("color: #78604b; font-size: 11px;");
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
    setWindowIcon(QIcon(":/barista/barista-logo.png"));
    resize(920,680);
    setMinimumSize(780,620);
    ApplyCafeTheme(this);

    auto* quitAction = new QAction("Quit Barista",this);
    quitAction->setObjectName("quitAction");
    quitAction->setShortcut(QKeySequence::Quit);
    addAction(quitAction);
    connect(quitAction,&QAction::triggered,this,&Window::Quit);

    auto* root = new QWidget(this);
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0,0,0,0);
    rootLayout->setSpacing(0);
    auto* sidebar = new QWidget(root);
    sidebar->setObjectName("sidebar");
    sidebar->setFixedWidth(184);
    auto* navigation = new QVBoxLayout(sidebar);
    navigation->setContentsMargins(10,18,10,16);
    navigation->setSpacing(6);
    m_tabs = new QTabWidget(root);
    m_tabs->setObjectName("mainPages");
    m_tabs->setDocumentMode(true);
    m_tabs->tabBar()->hide();
    auto* navGroup = new QButtonGroup(this);
    const QStringList pages{"Home","GamePads","Settings"};
    const std::array icons{CafeSymbol::Home,CafeSymbol::GamePad,CafeSymbol::Settings};
    for (int i = 0; i < pages.size(); ++i) {
        auto* button = new QPushButton(CafeIcon(icons[i],QColor("#d8c0a9")),pages[i],sidebar);
        button->setObjectName("nav" + pages[i]);
        button->setProperty("nav",true);
        button->setCheckable(true);
        button->setIconSize(QSize(24,24));
        navGroup->addButton(button,i);
        navigation->addWidget(button);
    }
    navGroup->button(0)->setChecked(true);
    connect(navGroup,&QButtonGroup::idClicked,m_tabs,&QTabWidget::setCurrentIndex);
    connect(m_tabs,&QTabWidget::currentChanged,this,[navGroup](int index) {
        if (auto* button = navGroup->button(index)) button->setChecked(true);
    });
    navigation->addStretch();
    m_status = new QLabel(sidebar);
    m_status->setObjectName("sessionStatus");
    m_status->setWordWrap(true);
    navigation->addWidget(m_status);
    auto* quitButton = new QPushButton(CafeIcon(CafeSymbol::Quit,QColor("#d8c0a9")),"Quit Barista",sidebar);
    quitButton->setObjectName("quitButton");
    quitButton->setProperty("nav",true);
    connect(quitButton,&QPushButton::clicked,this,&Window::Quit);
    navigation->addWidget(quitButton);
    rootLayout->addWidget(sidebar);
    auto* content = new QWidget(root);
    auto* contentLayout = new QVBoxLayout(content);
    contentLayout->setContentsMargins(0,0,0,0);
    contentLayout->setSpacing(0);
    m_message = new QLabel(content);
    m_message->setWordWrap(true);
    m_message->setContentsMargins(20,10,20,10);
    m_message->setStyleSheet("background: #ead5bd; color: #583b24;");
    m_message->hide();
    contentLayout->addWidget(m_message);
    contentLayout->addWidget(m_tabs,1);
    rootLayout->addWidget(content,1);

    auto* home = new QWidget(m_tabs);
    home->setObjectName("homePage");
    auto* homeLayout = new QVBoxLayout(home);
    homeLayout->setContentsMargins(30,24,30,22);
    homeLayout->setSpacing(20);
    auto* welcome = new QHBoxLayout;
    auto* greeting = new QVBoxLayout;
    auto* headline = new QLabel("Good to see you!",home);
    headline->setObjectName("homeHeading");
    greeting->addStretch();
    greeting->addWidget(headline);
    auto* introduction = new QLabel("Your GamePad, right at home on your PC.",home);
    introduction->setWordWrap(true);
    greeting->addWidget(introduction);
    greeting->addStretch();
    welcome->addLayout(greeting,1);
    auto* logo = new QLabel(home);
    logo->setObjectName("homeLogo");
    logo->setPixmap(QPixmap(":/barista/barista-logo.png").scaled(155,170,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    logo->setFixedSize(160,175);
    logo->setAlignment(Qt::AlignCenter);
    welcome->addWidget(logo);
    homeLayout->addLayout(welcome);

    auto* ready = new QFrame(home);
    ready->setObjectName("readyCard");
    auto* readyLayout = new QVBoxLayout(ready);
    readyLayout->setContentsMargins(20,18,20,18);
    readyLayout->setSpacing(12);
    m_homeStatus = new QLabel("Starting…",ready);
    m_homeStatus->setObjectName("homeStatus");
    readyLayout->addWidget(m_homeStatus);
    m_hint = new QLabel(ready);
    m_hint->setObjectName("homeHint");
    m_hint->setWordWrap(true);
    readyLayout->addWidget(m_hint);
    auto* deviceRow = new QHBoxLayout;
    auto* deviceIcon = new QLabel(ready);
    deviceIcon->setPixmap(CafeIcon(CafeSymbol::GamePad,QColor("#8b705a")).pixmap(32,32));
    deviceRow->addWidget(deviceIcon);
    auto* deviceText = new QVBoxLayout;
    m_deviceTitle = new QLabel("Wii U GamePad",ready);
    m_deviceTitle->setObjectName("deviceTitle");
    m_deviceTitle->setTextFormat(Qt::PlainText);
    m_deviceTitle->setWordWrap(true);
    m_homeDeviceDetail = new QLabel("Pair a GamePad to get started.",ready);
    m_homeDeviceDetail->setObjectName("homeDeviceDetail");
    m_homeDeviceDetail->setWordWrap(true);
    deviceText->addWidget(m_deviceTitle);
    deviceText->addWidget(m_homeDeviceDetail);
    deviceRow->addLayout(deviceText,1);
    m_gamepadBattery = new QLabel(ready);
    m_gamepadBattery->setObjectName("gamepadBattery");
    m_gamepadBattery->setAccessibleName("GamePad battery");
    deviceRow->addWidget(m_gamepadBattery);
    readyLayout->addLayout(deviceRow);
    m_start = new QPushButton(CafeIcon(CafeSymbol::Play),"Connect GamePad",ready);
    m_start->setObjectName("startButton");
    m_start->setProperty("primary",true);
    m_stop = new QPushButton(CafeIcon(CafeSymbol::Stop),"Disconnect GamePad",ready);
    m_stop->setObjectName("stopButton");
    m_stop->setProperty("primary",true);
    readyLayout->addWidget(m_start);
    readyLayout->addWidget(m_stop);
    homeLayout->addWidget(ready);

    auto* appCard = new QFrame(home);
    appCard->setObjectName("applicationCard");
    auto* appLayout = new QHBoxLayout(appCard);
    appLayout->setContentsMargins(0,0,0,0);
    m_appLogo = new QLabel(appCard);
    m_appLogo->setObjectName("connectedAppLogo");
    m_appLogo->setFixedSize(40,40);
    m_appLogo->setAlignment(Qt::AlignCenter);
    m_appLogo->setPixmap(CafeIcon(CafeSymbol::GamePad,QColor("#8b705a")).pixmap(30,30));
    appLayout->addWidget(m_appLogo);
    auto* appText = new QVBoxLayout;
    m_appName = new QLabel(appCard);
    m_appName->setObjectName("applicationName");
    m_appName->setTextFormat(Qt::PlainText);
    m_appName->setWordWrap(true);
    m_appSummary = new QLabel(appCard);
    m_appSummary->setObjectName("applicationSummary");
    m_appSummary->setWordWrap(true);
    appText->addWidget(m_appName);
    appText->addWidget(m_appSummary);
    appLayout->addLayout(appText,1);
    homeLayout->addWidget(appCard);
    homeLayout->addStretch();
    auto* homeFooter = new QHBoxLayout;
    auto* footerHint = new QLabel("Screen, sound, and controls. Together.",home);
    footerHint->setStyleSheet("color: #94775d; font-size: 11px;");
    homeFooter->addWidget(footerHint,1);
    auto* connectionLink = new QPushButton("Session info →",home);
    connectionLink->setObjectName("connectionDetailsButton");
    connectionLink->setProperty("link",true);
    connect(connectionLink,&QPushButton::clicked,this,[this] {
        m_settingsTabs->setCurrentIndex(1);
        m_tabs->setCurrentIndex(2);
    });
    homeFooter->addWidget(connectionLink);
    homeLayout->addLayout(homeFooter);
    for (auto* label : home->findChildren<QLabel*>()) label->setProperty("lightSurface",true);
    m_tabs->addTab(home,"Home");

    auto* gamepads = new QWidget(m_tabs);
    gamepads->setObjectName("gamepadsPage");
    gamepads->setProperty("cafePage",true);
    auto* gamepadsLayout = new QVBoxLayout(gamepads);
    gamepadsLayout->setContentsMargins(26,26,26,24);
    gamepadsLayout->setSpacing(16);
    auto* gamepadsHeader = new QHBoxLayout;
    auto* gamepadsTitle = new QLabel("GamePads",gamepads);
    gamepadsTitle->setProperty("heading",true);
    gamepadsHeader->addWidget(gamepadsTitle,1);
    auto* refreshPads = new QPushButton(CafeIcon(CafeSymbol::Refresh),"Refresh",gamepads);
    refreshPads->setObjectName("refreshGamePadsButton");
    connect(refreshPads,&QPushButton::clicked,this,&Window::RefreshSavedGamePads);
    gamepadsHeader->addWidget(refreshPads);
    gamepadsLayout->addLayout(gamepadsHeader);
    gamepadsLayout->addWidget(FormHint("Saved Wii U GamePads. Reconnect using their existing pairing.",gamepads));
    m_savedGamePads = new QListWidget(gamepads);
    m_savedGamePads->setObjectName("savedGamePads");
    m_savedGamePads->setIconSize(QSize(36,36));
    m_savedGamePads->setSpacing(6);
    gamepadsLayout->addWidget(m_savedGamePads);
    m_noGamePads = FormHint("No saved GamePads yet. Pair your GamePad to get started.",gamepads);
    m_noGamePads->setObjectName("emptyGamePadsHint");
    gamepadsLayout->addWidget(m_noGamePads);
    auto* savedButtons = new QHBoxLayout;
    savedButtons->addStretch();
    m_renamePair = new QPushButton(CafeIcon(CafeSymbol::Edit),"Rename",gamepads);
    m_removePair = new QPushButton(CafeIcon(CafeSymbol::Remove),"Remove pairing…",gamepads);
    m_renamePair->setObjectName("renameGamePadButton");
    m_removePair->setObjectName("removeGamePadButton");
    savedButtons->addWidget(m_renamePair);
    savedButtons->addWidget(m_removePair);
    gamepadsLayout->addLayout(savedButtons);
    auto* pairShortcut = new QPushButton(CafeIcon(CafeSymbol::Plus),"Pair a GamePad",gamepads);
    pairShortcut->setObjectName("addGamePadButton");
    pairShortcut->setIconSize(QSize(25,25));
    connect(pairShortcut,&QPushButton::clicked,this,&Window::OpenPairing);
    gamepadsLayout->addWidget(pairShortcut);
    gamepadsLayout->addWidget(FormHint("One GamePad session at a time. Pairing uses your dedicated Wi-Fi adapter.",gamepads));
    gamepadsLayout->addStretch();
    m_tabs->addTab(gamepads,"GamePads");

    // Settings own the session configuration; the pairing dialog shares the adapter selection.
    auto* settingsPage = new QWidget(m_tabs);
    settingsPage->setObjectName("settingsPage");
    settingsPage->setProperty("cafePage",true);
    auto* settingsLayout = new QVBoxLayout(settingsPage);
    settingsLayout->setContentsMargins(26,26,26,24);
    settingsLayout->setSpacing(16);
    auto* settingsTitle = new QLabel("Settings",settingsPage);
    settingsTitle->setProperty("heading",true);
    settingsLayout->addWidget(settingsTitle);
    auto* settingsForm = new QFormLayout;
    ConfigureForm(settingsForm);
    m_interface = new QComboBox(settingsPage);
    m_interface->setObjectName("interfaceCombo");
    m_interface->setEditable(false);
    m_interface->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_interface->setMinimumContentsLength(12);
    m_interface->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Fixed);
    for (const auto& interface : QNetworkInterface::allInterfaces())
        if (interface.type() == QNetworkInterface::Wifi)
            m_interface->addItem(AdapterLabel(interface.name()),interface.name());
    if (!m_interface->count()) m_interface->addItem("wlan0","wlan0");
    settingsForm->addRow("GamePad &adapter:",m_interface);
    m_country = new QLineEdit(settingsPage);
    m_country->setObjectName("regulatoryCountry");
    m_country->setMaxLength(2);
    m_country->setValidator(new QRegularExpressionValidator(QRegularExpression("[A-Za-z]{0,2}"),m_country));
    m_country->setPlaceholderText("System setting");
    settingsForm->addRow("&Country:",m_country);
    settingsLayout->addLayout(settingsForm);
    settingsLayout->addWidget(FormHint("Use your physical country's two-letter code. Temporary radio settings are restored when the session ends.",settingsPage));
    connect(m_country,&QLineEdit::textEdited,this,[this](const QString& value) {
        m_country->setText(value.toUpper());
        ApplyStatus(m_lastStatus);
    });
    auto* modeTitle = new QLabel("GamePad mode",settingsPage);
    modeTitle->setProperty("subheading",true);
    settingsLayout->addWidget(modeTitle);
    m_mode = new QComboBox(settingsPage);
    m_mode->setObjectName("modeCombo");
    m_mode->addItem("Screen + controller","real");
    m_mode->addItem("Controller only","controller");
    m_mode->hide();
    auto* modeButtons = new QButtonGroup(this);
    auto* modes = new QHBoxLayout;
    m_screenMode = new QPushButton(CafeIcon(CafeSymbol::GamePad),"Screen + controller",settingsPage);
    m_controllerMode = new QPushButton(CafeIcon(CafeSymbol::GamePad),"Controller only",settingsPage);
    m_screenMode->setObjectName("screenModeButton");
    m_controllerMode->setObjectName("controllerModeButton");
    for (auto* button : {m_screenMode,m_controllerMode}) {
        button->setCheckable(true);
        modes->addWidget(button);
    }
    modeButtons->addButton(m_screenMode,0);
    modeButtons->addButton(m_controllerMode,1);
    connect(modeButtons,&QButtonGroup::idClicked,m_mode,&QComboBox::setCurrentIndex);
    connect(m_mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[modeButtons](int index) {
        if (auto* button = modeButtons->button(index)) button->setChecked(true);
    });
    if (!smokeTest) {
        QSettings settings;
        SelectInterface(m_interface,settings.value("interface",InterfaceName(m_interface)).toString());
        m_mode->setCurrentIndex(std::max(0,m_mode->findData(settings.value("mode","real"))));
        m_country->setText(settings.value("regulatoryCountry",SuggestedRegulatoryCountry()).toString().toUpper());
    }
    modeButtons->button(m_mode->currentIndex())->setChecked(true);
    settingsLayout->addLayout(modes);
    m_description = FormHint({},settingsPage);
    m_description->setObjectName("modeDescription");
    settingsLayout->addWidget(m_description);
    auto* desktopTitle = new QLabel("Desktop",settingsPage);
    desktopTitle->setProperty("subheading",true);
    settingsLayout->addWidget(desktopTitle);
    m_background = new QCheckBox("Keep running when the window closes",settingsPage);
    m_background->setObjectName("backgroundCheck");
    m_background->setChecked(smokeTest || QSettings().value("background",true).toBool());
    settingsLayout->addWidget(m_background);
    settingsLayout->addWidget(FormHint("Your session continues in the system tray.",settingsPage));
    connect(m_background,&QCheckBox::toggled,this,[smokeTest](bool enabled) {
        if (!smokeTest) QSettings().setValue("background",enabled);
    });
    settingsLayout->addStretch();

    m_pairDialog = new PairingDialog(this);
    m_pairDialog->setObjectName("pairingDialog");
    m_pairDialog->setAttribute(Qt::WA_StyledBackground,true);
    auto pairingPalette = m_pairDialog->palette();
    pairingPalette.setColor(QPalette::Window,QColor("#f5ecdf"));
    pairingPalette.setColor(QPalette::WindowText,QColor("#38261a"));
    m_pairDialog->setPalette(pairingPalette);
    m_pairDialog->setAutoFillBackground(true);
    m_pairDialog->setWindowTitle("Barista — Pair a GamePad");
    m_pairDialog->resize(590,450);
    auto* pairingLayout = new QVBoxLayout(m_pairDialog);
    pairingLayout->setContentsMargins(24,24,24,24);
    pairingLayout->setSpacing(16);
    m_pairTitle = new QLabel("Pair a GamePad",m_pairDialog);
    m_pairTitle->setObjectName("pairingTitle");
    m_pairTitle->setProperty("heading",true);
    pairingLayout->addWidget(m_pairTitle);
    m_pairInstructions = new QLabel(m_pairDialog);
    m_pairInstructions->setObjectName("pairingInstructions");
    m_pairInstructions->setWordWrap(true);
    pairingLayout->addWidget(m_pairInstructions);
    m_pairContent = new QStackedWidget(m_pairDialog);
    m_pairContent->setObjectName("pairingContent");
    m_pairContent->setMinimumHeight(110);
    m_pairStage = new PairingProgressLabel(m_pairContent);
    m_pairStage->setObjectName("pairingStage");
    m_pairStage->setAlignment(Qt::AlignCenter);
    m_pairStage->setWordWrap(true);
    m_pairContent->addWidget(m_pairStage);
    m_pairSymbols = new QWidget(m_pairDialog);
    m_pairSymbols->setObjectName("pairingSymbols");
    auto* symbols = new QHBoxLayout(m_pairSymbols);
    symbols->setContentsMargins(0,0,0,0);
    m_code = new QLineEdit(m_pairDialog);
    m_code->setObjectName("pairingCode");
    m_code->setReadOnly(true);
    m_code->hide();
    for (size_t i = 0; i < m_pairSymbolLabels.size(); ++i) {
        auto* symbol = new QLabel(m_pairSymbols);
        symbol->setObjectName(QString("pairingSymbol%1").arg(i + 1));
        symbol->setProperty("pairSymbol",true);
        symbol->setAlignment(Qt::AlignCenter);
        symbol->setMinimumHeight(110);
        m_pairSymbolLabels[i] = symbol;
        symbols->addWidget(symbol,1);
    }
    InitializePairingPattern();
    m_pairContent->addWidget(m_pairSymbols);
    pairingLayout->addWidget(m_pairContent);
    auto* pairingHint = new QLabel("Already paired? Use Connect GamePad on Home instead.",m_pairDialog);
    pairingHint->setObjectName("pairingHint");
    pairingHint->setWordWrap(true);
    pairingLayout->addWidget(pairingHint);
    pairingLayout->addStretch();
    auto* pairButtons = new QHBoxLayout;
    auto* closePair = new QPushButton("Cancel",m_pairDialog);
    closePair->setObjectName("cancelPairButton");
    connect(closePair,&QPushButton::clicked,m_pairDialog,&QDialog::reject);
    connect(m_pairDialog,&QDialog::rejected,this,&Window::CancelPairing);
    connect(this,&Window::PairingStopRequested,&m_client,&ControlClient::Stop);
    pairButtons->addStretch();
    pairButtons->addWidget(closePair);
    m_pair = new QPushButton(CafeIcon(CafeSymbol::Plus),"Pair",m_pairDialog);
    m_pair->setObjectName("pairButton");
    m_pair->setProperty("primary",true);
    pairButtons->addWidget(m_pair);
    pairingLayout->addLayout(pairButtons);

    m_waitingDialog = new PairingDialog(this);
    m_waitingDialog->setObjectName("waitingDialog");
    m_waitingDialog->setWindowTitle("Barista — Connecting");
    m_waitingDialog->resize(440,430);
    auto* waitingLayout = new QVBoxLayout(m_waitingDialog);
    waitingLayout->setContentsMargins(28,24,28,24);
    waitingLayout->setSpacing(16);
    m_waitingStatus = new QLabel("Waiting for your GamePad",m_waitingDialog);
    m_waitingStatus->setProperty("heading",true);
    m_waitingStatus->setAlignment(Qt::AlignCenter);
    m_waitingStatus->setWordWrap(true);
    waitingLayout->addWidget(m_waitingStatus);
    auto* waitingHint = FormHint("Turn on your paired GamePad and keep it nearby.",m_waitingDialog);
    waitingHint->setAlignment(Qt::AlignCenter);
    waitingLayout->addWidget(waitingHint);
    auto* waitingLogo = new QLabel(m_waitingDialog);
    waitingLogo->setPixmap(QPixmap(":/barista/barista-logo.png").scaled(145,158,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    waitingLogo->setAlignment(Qt::AlignCenter);
    waitingLayout->addWidget(waitingLogo);
    auto* waitingAdapterCard = new QFrame(m_waitingDialog);
    waitingAdapterCard->setObjectName("waitingAdapterCard");
    auto* waitingAdapterLayout = new QHBoxLayout(waitingAdapterCard);
    waitingAdapterLayout->setContentsMargins(16,12,16,12);
    auto* waitingWifi = new QLabel(waitingAdapterCard);
    waitingWifi->setPixmap(CafeIcon(CafeSymbol::Wifi).pixmap(26,26));
    waitingAdapterLayout->addWidget(waitingWifi);
    m_waitingAdapter = new QLabel(waitingAdapterCard);
    m_waitingAdapter->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_waitingAdapter->setWordWrap(true);
    waitingAdapterLayout->addWidget(m_waitingAdapter,1);
    waitingLayout->addWidget(waitingAdapterCard);
    auto* waitButtons = new QHBoxLayout;
    auto* hideWaiting = new QPushButton("Hide",m_waitingDialog);
    hideWaiting->setToolTip("Keep the session running in the background");
    connect(hideWaiting,&QPushButton::clicked,m_waitingDialog,&QDialog::hide);
    m_waitingStop = new QPushButton(CafeIcon(CafeSymbol::Stop),"End session",m_waitingDialog);
    m_waitingStop->setObjectName("waitingStopButton");
    connect(m_waitingStop,&QPushButton::clicked,m_stop,&QPushButton::click);
    waitButtons->addWidget(hideWaiting);
    waitButtons->addWidget(m_waitingStop);
    waitingLayout->addLayout(waitButtons);


    connect(m_savedGamePads,&QListWidget::currentRowChanged,this,[this] { ApplyStatus(m_lastStatus); });
    connect(m_renamePair,&QPushButton::clicked,this,[this] {
        auto* item = m_savedGamePads->currentItem(); if (!item) return;
        const QString mac = item->data(Qt::UserRole).toString();
        bool ok = false; const QString name = QInputDialog::getText(this,"Rename GamePad","Name:",QLineEdit::Normal,item->data(Qt::UserRole + 1).toString(),&ok);
        if (!ok) return;
        m_client.RenameGamePad({mac.toStdString(), name.toStdString()});
        auto records = LoadSavedGamePadsCache(); records.insert(mac,name); SaveSavedGamePadsCache(records);
        ApplyGamePads({});
    });
    connect(m_removePair,&QPushButton::clicked,this,[this] {
        auto* item = m_savedGamePads->currentItem(); if (!item) return;
        const QString mac = item->data(Qt::UserRole).toString();
        if (QMessageBox::question(this,"Remove saved GamePad?",QString("Remove %1? It will need to be paired again.").arg(item->text())) != QMessageBox::Yes) return;
        m_client.RemoveGamePad({mac.toStdString()});
        auto records = LoadSavedGamePadsCache(); records.remove(mac); SaveSavedGamePadsCache(records);
        ApplyGamePads({});
    });

    QTimer::singleShot(0,this,&Window::RefreshSavedGamePads);
    auto* connectionPage = new QWidget(m_tabs);
    connectionPage->setObjectName("advancedPanel");
    connectionPage->setProperty("cafePage",true);
    auto* connectionLayout = new QVBoxLayout(connectionPage);
    connectionLayout->setContentsMargins(26,26,26,24);
    connectionLayout->setSpacing(18);
    auto* connectionTitle = new QLabel("Session info",connectionPage);
    connectionTitle->setProperty("heading",true);
    connectionLayout->addWidget(connectionTitle);
    connectionLayout->addWidget(FormHint("Current GamePad session details. Connect or disconnect from Home.",connectionPage));
    auto* sessionDetails = new QGroupBox("Session",connectionPage);
    auto* detailsForm = new QFormLayout(sessionDetails);
    ConfigureForm(detailsForm);
    m_gamepadState = new QLabel(sessionDetails);
    m_gamepadPhase = new QLabel(sessionDetails);
    m_gamepadMode = new QLabel(sessionDetails);
    m_gamepadIface = new QLabel(sessionDetails);
    for (auto* label : {m_gamepadState,m_gamepadPhase,m_gamepadMode,m_gamepadIface}) {
        label->setWordWrap(true);
        label->setSizePolicy(QSizePolicy::Expanding,QSizePolicy::Preferred);
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
    }
    detailsForm->addRow("GamePad:",m_gamepadState);
    detailsForm->addRow("Session:",m_gamepadPhase);
    detailsForm->addRow("Mode:",m_gamepadMode);
    detailsForm->addRow("Wi-Fi adapter:",m_gamepadIface);
    connectionLayout->addWidget(sessionDetails);
    auto* advanced = new QWidget;
    advanced->setObjectName("supportPanel");
    advanced->setProperty("cafePage",true);
    auto* advancedLayout = new QVBoxLayout(advanced);
    advancedLayout->setContentsMargins(26,26,26,24);
    advancedLayout->setSpacing(12);
    auto* supportTitle = new QLabel("Support & diagnostics",advanced);
    supportTitle->setProperty("heading",true);
    advancedLayout->addWidget(supportTitle);
    m_appLock = new QLabel(advanced);
    m_appIdleLogo = new QLabel(advanced);
    m_appLastSeen = new QLabel(advanced);
    m_appSocket = new QLabel(advanced);
    for (auto* label : {m_appLock,m_appIdleLogo,m_appLastSeen,m_appSocket}) {
        label->setTextInteractionFlags(Qt::TextSelectableByMouse);
        label->setWordWrap(true);
    }
    auto* appForm = new QFormLayout;
    ConfigureForm(appForm);
    appForm->addRow("Application access:",m_appLock);
    appForm->addRow("Idle logo:",m_appIdleLogo);
    appForm->addRow("Last activity:",m_appLastSeen);
    appForm->addRow("Media socket:",m_appSocket);
    advancedLayout->addLayout(appForm);
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
        value->setMinimumWidth(120);
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
    m_logFiles->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_logFiles->setMinimumContentsLength(20);
    supportLayout->addWidget(m_logFiles);
    auto* supportButtons = new QDialogButtonBox(Qt::Vertical,support);
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

    m_copy->setIcon(CafeIcon(CafeSymbol::Copy));
    m_viewLog->setIcon(CafeIcon(CafeSymbol::Info));
    m_openLogs->setIcon(CafeIcon(CafeSymbol::Folder));
    m_copyDiagnostics->setIcon(CafeIcon(CafeSymbol::Copy));
    m_saveDiagnostics->setIcon(CafeIcon(CafeSymbol::Save));
    refresh->setIcon(CafeIcon(CafeSymbol::Refresh));
    refreshLogs->setIcon(CafeIcon(CafeSymbol::Refresh));
    advancedLayout->addStretch();
    connectionLayout->addStretch();
    m_settingsTabs = new QTabWidget(m_tabs);
    m_settingsTabs->setObjectName("settingsTabs");
    m_settingsTabs->setDocumentMode(true);
    m_settingsTabs->tabBar()->setExpanding(true);
    auto* connectionScroll = new QScrollArea(m_settingsTabs);
    connectionScroll->setWidgetResizable(true);
    connectionScroll->setWidget(connectionPage);
    auto* settingsScroll = new QScrollArea(m_settingsTabs);
    settingsScroll->setWidgetResizable(true);
    settingsScroll->setWidget(settingsPage);
    m_settingsTabs->addTab(settingsScroll,"General");
    m_settingsTabs->addTab(connectionScroll,"Session info");
    auto* supportScroll = new QScrollArea(m_settingsTabs);
    supportScroll->setWidgetResizable(true);
    supportScroll->setWidget(advanced);
    m_settingsTabs->addTab(supportScroll,"Support");
    m_tabs->addTab(m_settingsTabs,"Settings");

    auto* about = new QWidget(m_tabs);
    about->setObjectName("aboutPage");
    about->setProperty("cafePage",true);
    auto* aboutLayout = new QVBoxLayout(about);
    aboutLayout->setContentsMargins(45,35,45,35);
    aboutLayout->addStretch();
    auto* aboutLogo = new QLabel(about);
    aboutLogo->setPixmap(QPixmap(":/barista/barista-logo.png").scaled(140,155,Qt::KeepAspectRatio,Qt::SmoothTransformation));
    aboutLogo->setAlignment(Qt::AlignCenter);
    aboutLayout->addWidget(aboutLogo);
    auto* aboutTitle = new QLabel("Barista",about);
    aboutTitle->setProperty("heading",true);
    aboutTitle->setAlignment(Qt::AlignCenter);
    aboutLayout->addWidget(aboutTitle);
    auto* aboutVersion = FormHint("Version " + qApp->applicationVersion(),about);
    aboutVersion->setAlignment(Qt::AlignCenter);
    aboutLayout->addWidget(aboutVersion);
    auto* aboutCopy = FormHint("A new home for your Wii U GamePad.\n\nConnect a real GamePad to your Linux desktop for screen, audio, and controller input. Barista handles pairing and the dedicated Wi-Fi connection.\n\nNot affiliated with Nintendo.",about);
    aboutCopy->setAlignment(Qt::AlignCenter);
    aboutLayout->addWidget(aboutCopy);
    aboutLayout->addStretch();
    m_settingsTabs->addTab(about,"About");
    setCentralWidget(root);

    auto describe = [this] {
        m_description->setText(Mode() == barista::api::SessionMode::Real
            ? "Video, audio, and controls for supported apps."
            : "PC controller input. Touch, motion, and rumble are unavailable.");
    };
    connect(m_mode,qOverload<int>(&QComboBox::currentIndexChanged),this,[describe](int) { describe(); });
    connect(m_interface,&QComboBox::currentTextChanged,this,[this] {
        ApplyStatus(m_lastStatus);
    });
    describe();

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
        m_waitingDialog->show();
        m_client.Start({interface.toStdString(), Mode(), country.toStdString()});
    });
    connect(m_stop,&QPushButton::clicked,&m_client,&ControlClient::Stop);
    connect(m_pair,&QPushButton::clicked,this,[this] {
        const QString interface = InterfaceName(m_interface);
        if (!ConfirmWifi(true,interface)) return;
        QSettings settings;
        settings.setValue("interface",interface);
        settings.setValue("pairInterface",interface);
        const QString country = m_country->text().trimmed().toUpper();
        settings.setValue("regulatoryCountry",country);
        m_message->hide();
        const auto code = barista::api::ParsePairCode(m_code->text().toStdString());
        if (!code) return;
        m_pairingRequested = true;
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
        m_message->setText("That didn't complete. Open Settings → Support for details.");
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
        if (!m_smokeTest && index == 2 && m_settingsTabs->currentIndex() == 2) RefreshDiagnostics();
    });
    connect(m_settingsTabs,&QTabWidget::currentChanged,this,[this](int index) {
        if (!m_smokeTest && index == 2) RefreshDiagnostics();
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
void Window::OpenPairing()
{
    ShowWindow();
    m_tabs->setCurrentIndex(1);
    m_pairDialog->show();
    m_pairDialog->raise();
    m_pair->setFocus();
}

void Window::CancelPairing()
{
    const auto phase = m_lastStatus.phase;
    const bool ownedPairing = m_lastStatus.ownedByCaller &&
        (phase == barista::api::SessionPhase::Pairing ||
         ((phase == barista::api::SessionPhase::Starting || phase == barista::api::SessionPhase::Preparing) &&
          m_lastStatus.pairingStep != barista::api::PairingStep::None));
    const bool shouldStop = m_pairingRequested || ownedPairing;
    m_pairingRequested = false;
    if (shouldStop && phase != barista::api::SessionPhase::Stopping)
    {
        // Stop queues behind an outstanding Pair/authorization request, so
        // cancelling early cannot leave a session starting in the background.
        emit PairingStopRequested();
    }
}

void Window::InitializePairingPattern()
{
    const auto pattern = NewPairingPattern(*QRandomGenerator::global());
    const QStringList shapes{"♠","♥","♦","♣"};
    const QStringList names{"Spade","Heart","Diamond","Club"};
    QString code;
    for (size_t i = 0; i < pattern.size(); ++i) {
        m_pairSymbolLabels[i]->setText(shapes[pattern[i]]);
        m_pairSymbolLabels[i]->setAccessibleName(QString("Pairing symbol %1: %2").arg(i + 1).arg(names[pattern[i]]));
        code += QString::number(pattern[i]);
    }
    m_code->setText(code);
}
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
        m_message->setText("Barista is running in the background. Use Quit Barista in the sidebar or tray to stop and exit."); m_message->show();
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
    if (!m_smokeTest) m_client.RefreshGamePads();
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
    const QString selected = m_savedGamePads->currentItem()
        ? m_savedGamePads->currentItem()->data(Qt::UserRole).toString() : QString();
    auto records = m_smokeTest ? QMap<QString,QString>{} : LoadSavedGamePadsCache();
    for (const auto& gamePad : gamePads)
        records.insert(QString::fromStdString(gamePad.mac), QString::fromStdString(gamePad.name));
    if (!m_smokeTest) SaveSavedGamePadsCache(records);
    m_savedGamePads->clear();
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        const QString name = it.value().isEmpty() ? "Wii U GamePad" : it.value();
        auto* item = new QListWidgetItem(CafeIcon(CafeSymbol::GamePad),name + "\nWii U GamePad · Saved pairing",m_savedGamePads);
        item->setSizeHint(QSize(0,78));
        item->setData(Qt::UserRole,it.key());
        item->setData(Qt::UserRole + 1,name);
        if (it.key() == selected) m_savedGamePads->setCurrentItem(item);
    }
    if (!m_savedGamePads->currentItem() && m_savedGamePads->count()) m_savedGamePads->setCurrentRow(0);
    m_savedGamePads->setFixedHeight(std::clamp(m_savedGamePads->count() * 90 + 12,100,290));
    m_savedGamePads->setVisible(m_savedGamePads->count() > 0);
    m_noGamePads->setVisible(m_savedGamePads->count() == 0);
    UpdateHomeDevice();
}

void Window::UpdateHomeDevice()
{
    const int count = m_savedGamePads->count();
    m_deviceTitle->setText(count == 1 ? m_savedGamePads->item(0)->data(Qt::UserRole + 1).toString() : "Wii U GamePad");
    m_homeDeviceDetail->setText(m_lastStatus.gamePadConnected ? "Wii U GamePad · Connected" :
        m_lastStatus.running ? "Turn on your paired GamePad and keep it nearby." :
        count == 0 ? "No saved GamePads listed. Add one from the GamePads page." :
        QString("%1 saved pairing%2 · Ready to reconnect").arg(count).arg(count == 1 ? "" : "s"));
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
    if (!busy && (!available || status.error || phase == barista::api::SessionPhase::Failed ||
        phase == barista::api::SessionPhase::Stopping ||
        (phase == barista::api::SessionPhase::Idle && previousPhase != barista::api::SessionPhase::Idle)))
        m_pairingRequested = false;
    UpdateHomeDevice();
    const QString phaseText = QString::fromLatin1(barista::api::SessionPhaseName(phase));
    if (previousRunning && !running) RefreshDiagnostics();
    if (m_pairingRequested && phase == barista::api::SessionPhase::Runtime &&
        previousPhase != barista::api::SessionPhase::Runtime) {
        RefreshSavedGamePads();
        m_pairingRequested = false;
        m_pairDialog->hide();
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
        hint = "Automatic startup did not complete. Open Settings → Support to retry.";
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
        hint = "Start a session to connect your paired GamePad. For a new device, open GamePads.";
    }
    m_status->setText(sessionText);
    SetTone(m_status,sessionTone,true);
    m_homeStatus->setText(sessionText);
    SetTone(m_homeStatus,sessionTone,true);
    m_hint->setText(hint);
    SetTone(m_hint,sessionTone);
    m_hint->show();
    m_waitingStatus->setText(busy ? "Preparing your connection…" : "Waiting for your GamePad");
    m_waitingAdapter->setText("GamePad Wi-Fi adapter\n" + InterfaceName(m_interface));
    if (connected || (!running && !busy) || phase == barista::api::SessionPhase::Failed)
        m_waitingDialog->hide();

    const auto sessionMode = status.mode.value_or(barista::api::SessionMode::Real);
    const auto ifaceName = QString::fromStdString(status.interfaceName);
    m_gamepadPhase->setText(running ? phaseText : "Idle");
    m_gamepadMode->setText(sessionMode == barista::api::SessionMode::Controller ? "Controller only" : "Screen + controller");
    m_gamepadIface->setText(ifaceName.isEmpty() ? InterfaceName(m_interface) : ifaceName);
    if (status.batteryPercent)
        m_gamepadBattery->setText(QString("%1%").arg(*status.batteryPercent));
    else
        m_gamepadBattery->setText("—");
    m_gamepadBattery->setVisible(status.batteryPercent.has_value());
    m_gamepadBattery->setAccessibleName("GamePad battery");
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
            m_appLogo->setText({});
            m_appLogo->setPixmap(CafeIcon(CafeSymbol::GamePad,QColor("#8b705a")).pixmap(30,30));
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
        m_appName->setText("PC controller ready");
        SetTone(m_appName, Tone::Good, false);
        m_appLock->setText("Not applicable in controller mode");
        SetTone(m_appLock, Tone::Neutral, false);
        if (m_appIdleLogo) m_appIdleLogo->setText("Default (Barista)");
        m_appLastSeen->setText("Active");
        m_appSocket->setText("Internal controller bridge");
    } else if (appConnected) {
        m_appName->setText(appName.isEmpty() ? "Connected application" : appName);
        m_appName->setToolTip(QString("Process ID: %1").arg(appPid));
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
        m_appName->setText("No application connected");
        SetTone(m_appName, Tone::Warning, false);
        m_appLock->setText("Unlocked");
        SetTone(m_appLock, Tone::Neutral, false);
        if (m_appIdleLogo) {
            m_appIdleLogo->setText("Default (Barista)");
        }
        m_appLastSeen->setText("—");
        m_appSocket->setText(mediaEndpoint.isEmpty() ? "Listening" : mediaEndpoint);
    }
    m_appSummary->setText(!running ? "Connect your GamePad to get started." :
        sessionMode == barista::api::SessionMode::Controller ? "Ready for PC games. Barista stays on the screen." :
        appConnected ? "Screen, audio, and controls connected." : "Open a supported app on your computer.");
    if (!appConnected) m_appName->setToolTip({});
    m_start->setVisible(!running);
    m_stop->setVisible(running);
    const bool supported = Mode() != barista::api::SessionMode::Controller || status.capabilities.controller ||
        status.capabilities.controllerSetup;
    const std::string country = m_country->text().trimmed().toUpper().toStdString();
    const bool validCountry = country.empty() || barista::api::ValidRegulatoryCountry(country);
    m_start->setEnabled(available && !running && !busy && supported && validCountry);
    m_pair->setEnabled(available && !running && !busy && supported &&
        barista::api::ParsePairCode(m_code->text().toStdString()).has_value() &&
        validCountry);
    m_stop->setEnabled(available && running && !busy && owned && phase != barista::api::SessionPhase::Stopping);
    m_waitingStop->setEnabled(m_stop->isEnabled());
    const bool canEditPair = available && !running && !busy && m_savedGamePads->currentItem();
    m_renamePair->setEnabled(canEditPair);
    m_removePair->setEnabled(canEditPair);
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
    m_screenMode->setEnabled(!running && !busy);
    m_controllerMode->setEnabled(!running && !busy);
    m_country->setEnabled(!running && !busy);
    const bool pairingError = status.error.has_value() || !m_operationError.isEmpty() ||
        phase == barista::api::SessionPhase::Failed;
    const bool preparing = available && (busy || (!pairingError &&
        (phase == barista::api::SessionPhase::Starting || phase == barista::api::SessionPhase::Preparing ||
        (m_pairingRequested && phase == barista::api::SessionPhase::Idle))));
    const bool pairNow = available && running && owned && !preparing && !pairingError &&
        phase == barista::api::SessionPhase::Pairing;
    m_pairTitle->setText(pairNow ? "Pair now" : "Pair a GamePad");
    m_pairInstructions->setText(pairNow
        ? "Press the SYNC button on your GamePad.\nEnter these symbols from left to right."
        : preparing ? "Keep your GamePad nearby.\nWait for the symbols to appear."
        : "1. Turn on your GamePad and keep it nearby.\n2. Select Pair below to get started.");
    QString stage;
    Tone stageTone = Tone::Neutral;
    if (!available) {
        stage = "Barista isn't ready.\nCheck Settings → Support, then try again.";
        stageTone = Tone::Bad;
    } else if (preparing) {
        switch (status.pairingStep)
        {
        case barista::api::PairingStep::CheckingAdapter: stage = "Checking adapter…"; break;
        case barista::api::PairingStep::SettingUpAdapter: stage = "Setting up adapter…"; break;
        case barista::api::PairingStep::CreatingNetwork: stage = "Creating network…"; break;
        case barista::api::PairingStep::None:
            stage = phase == barista::api::SessionPhase::Idle
                ? "Waiting for permission…" : "Starting pairing…";
            break;
        }
    } else if (pairingError) {
        stage = status.error && status.error->diagnosticCode == "AP_REGULATORY_BLOCKED"
            ? "Check your Wi-Fi country in Settings → General.\nSee Settings → Support if pairing still won't start."
            : "Pairing couldn't start. Try again.\nSee Settings → Support for help.";
        stageTone = Tone::Bad;
    } else if (running && !pairNow) {
        stage = "End the current session on Home before pairing.";
        stageTone = Tone::Warning;
    } else if (!supported) {
        stage = "Select Screen + controller in Settings → General.";
        stageTone = Tone::Warning;
    } else if (!validCountry) {
        stage = "Check your country code in Settings → General.";
        stageTone = Tone::Warning;
    }
    m_pairStage->setText(stage);
    SetTone(m_pairStage,stageTone,true);
    m_pairContent->setCurrentWidget(pairNow ? m_pairSymbols : m_pairStage);
    m_pairContent->setVisible(pairNow || !stage.isEmpty());
    static_cast<PairingProgressLabel*>(m_pairStage)->SetBreathing(preparing);
    m_pair->setEnabled(m_pair->isEnabled() && !preparing);
    m_pair->setText(preparing ? "Starting…" : pairNow ? "Pairing…" : "Pair");
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
        m_hint->setText("The last operation reported a problem. Open Settings → Support before trying again.");
        m_hint->show();
        SetTone(m_hint,Tone::Bad);
    }
}
