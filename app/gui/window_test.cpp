#include "window.h"
#include "cafe_icons.h"
#include "pairing_pattern.h"
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTabBar>
#include <QMouseEvent>
#include <QAbstractItemView>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QFileInfo>
#include <QToolBar>
#include <QMenuBar>
#include <QAction>
#include <QDialog>
#include <QSet>
#include <QSvgRenderer>
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv)
{
    QApplication app(argc,argv);
    try {
        Window window(true); window.show();
        QApplication::processEvents();
        auto* start = window.findChild<QPushButton*>("startButton");
        auto* stop = window.findChild<QPushButton*>("stopButton");
        auto* pair = window.findChild<QPushButton*>("pairButton");
        auto* country = window.findChild<QLineEdit*>("regulatoryCountry");
        auto* advanced = window.findChild<QWidget*>("advancedPanel");
        Check(start && stop && pair && country && advanced,"required controls");
        Check(!window.findChild<QToolBar*>("sessionToolbar"),"session controls are not duplicated in a toolbar");
        Check(!window.findChild<QMenuBar*>(),"window has no top menu bar");
        auto* quitButton = window.findChild<QPushButton*>("quitButton");
        auto* quitAction = window.findChild<QAction*>("quitAction");
        Check(quitButton && quitAction && quitAction->shortcut() == QKeySequence(QKeySequence::Quit),"Quit retains its keyboard shortcut");
        Check(!start->isEnabled() && !stop->isEnabled(),"service missing disables actions");
        auto* tabs = window.findChild<QTabWidget*>("mainPages");
        auto* settingsTabs = window.findChild<QTabWidget*>("settingsTabs");
        Check(tabs->count() == 3 && tabs->tabText(2) == "Settings", "sidebar has only Home, GamePads and Settings");
        Check(settingsTabs && settingsTabs->count() == 4 && settingsTabs->tabText(0) == "General" &&
            settingsTabs->tabText(1) == "Session info" && settingsTabs->tabText(2) == "Support" &&
            settingsTabs->tabText(3) == "About", "Settings contains the four requested tabs");
        Check(!window.findChild<QPushButton*>("navConnection") && !window.findChild<QPushButton*>("navAbout"),
            "Connection and About are no longer sidebar pages");
        Check(!window.findChild<QPushButton*>("connectionStartButton") &&
            !window.findChild<QPushButton*>("connectionStopButton") && advanced->findChildren<QPushButton*>().isEmpty(),
            "Session info is read-only without duplicated connection actions");
        auto* preferences = window.findChild<QPushButton*>("navSettings");
        auto* back = window.findChild<QPushButton*>("navHome");
        auto* screenMode = window.findChild<QPushButton*>("screenModeButton");
        auto* controllerMode = window.findChild<QPushButton*>("controllerModeButton");
        Check(preferences && back && screenMode && controllerMode,"sidebar navigation and mode controls");
        const QSize initialSize = window.size();
        Check(window.findChild<QLabel*>("homeLogo"),"Home displays Barista branding");
        Check(!QFileInfo::exists(":/barista/app/branding/gamepad.svg"),"previous standalone icon is no longer bundled");
        for (const QString name : {"home","gamepad-alt","signal-high","cog","info-circle","plus","caret-right",
                "stop","reload","copy","external","save","pencil","trash","exit","computer"}) {
            QSvgRenderer svg(":/barista/icons/kenney/" + name + ".svg");
            Check(svg.isValid(),"every interface icon is a valid bundled Kenney SVG");
        }
        for (int symbol = 0; symbol <= static_cast<int>(CafeSymbol::Computer); ++symbol) {
            const auto icon = CafeIcon(static_cast<CafeSymbol>(symbol));
            for (const int size : {20,24,48}) {
                const QImage pixels = icon.pixmap(size,size).toImage();
                int visible = 0;
                for (int y = 0; y < pixels.height(); ++y)
                    for (int x = 0; x < pixels.width(); ++x)
                        if (pixels.pixelColor(x,y).alpha() > 100) ++visible;
                Check(visible > size * size / 10,"icon has enough visual weight at button, sidebar and high-DPI sizes");
            }
        }
        const QColor iconColor("#927456");
        const auto gamePadIcon = CafeIcon(CafeSymbol::GamePad,iconColor);
        const auto normalIcon = gamePadIcon.pixmap(84,84).toImage();
        const auto disabledIcon = gamePadIcon.pixmap(84,84,QIcon::Disabled).toImage();
        int normalAlpha = 0, disabledAlpha = 0;
        for (int y = 0; y < normalIcon.height(); ++y) {
            for (int x = 0; x < normalIcon.width(); ++x) {
                const auto pixel = normalIcon.pixelColor(x,y);
                normalAlpha += pixel.alpha();
                disabledAlpha += disabledIcon.pixelColor(x,y).alpha();
                if (pixel.alpha() == 255)
                    Check(pixel == iconColor,"GamePad SVG uses the requested theme color");
            }
        }
        Check(normalAlpha > 0 && disabledAlpha > 0 && disabledAlpha < normalAlpha,
            "GamePad SVG renders visible artwork and a dimmed disabled state");
        auto* sidebarStatus = window.findChild<QLabel*>("sessionStatus");
        Check(!window.findChild<QLabel*>("sidebarLogo"),"sidebar logo has been removed");
        Check(sidebarStatus && quitButton->parentWidget()->objectName() == "sidebar" &&
            quitButton->y() > sidebarStatus->geometry().bottom(),"Quit sits below the status in the sidebar footer");
        for (const QString name : {"Home","GamePads","Settings"}) {
            auto* nav = window.findChild<QPushButton*>("nav" + name);
            Check(nav && !nav->icon().pixmap(24,24).isNull(),"sidebar has self-contained icons");
            nav->click();
            Check(nav->isChecked() && tabs->tabText(tabs->currentIndex()) == name,"sidebar navigation selects the correct page");
            QApplication::processEvents();
            const auto pageImage = window.grab().toImage();
            const auto backgroundColor = pageImage.pixelColor(pageImage.width() - 20,pageImage.height() - 20);
            Check(backgroundColor.red() > 220 && backgroundColor.green() > 210 && backgroundColor.blue() > 190,
                "all main pages have a cream content background");
            Check(pageImage.pixelColor(3,3).lightness() < 80,"only the sidebar retains its dark background");
            Check(quitButton->isVisible(),"Quit is available on every page");
        }
        for (int index = 0; index < settingsTabs->count(); ++index) {
            settingsTabs->setCurrentIndex(index);
            QApplication::processEvents();
            const auto settingsImage = settingsTabs->grab().toImage();
            const auto backgroundColor = settingsImage.pixelColor(settingsImage.width() - 20,settingsImage.height() - 20);
            Check(backgroundColor.red() > 220 && backgroundColor.green() > 210 && backgroundColor.blue() > 190,
                "all Settings tabs have cream backgrounds");
        }
        settingsTabs->setCurrentIndex(0);
        QApplication::processEvents();
        auto* settingsBar = settingsTabs->tabBar();
        Check(std::abs(settingsBar->width() - settingsTabs->width()) <= 2 &&
            settingsBar->tabRect(3).right() >= settingsTabs->width() - 2,
            "Settings tabs expand across the full content width");
        window.resize(initialSize.width() + 180,initialSize.height());
        QApplication::processEvents();
        Check(settingsBar->tabRect(3).right() >= settingsTabs->width() - 2,
            "Settings tabs continue to fill the width after resizing");
        window.resize(initialSize);
        back->click();
        window.findChild<QPushButton*>("connectionDetailsButton")->click();
        Check(tabs->currentIndex() == 2 && settingsTabs->currentIndex() == 1,
            "Home session shortcut opens Settings Session info tab");
        back->click();
        window.findChild<QPushButton*>("navSettings")->click();
        Check(quitButton->isVisible(),"Quit is available in the sidebar");
        back->click();
        Check(!advanced->isVisible(),"technical details not on General");
        preferences->click();
        settingsTabs->setCurrentIndex(1);
        Check(advanced->isVisible(),"Session info tab opens session details");
        back->click(); Check(tabs->currentIndex() == 0 && window.size() == initialSize,"navigation preserves window size");
        preferences->click();
        Check(!window.findChild<QWidget*>("supportPanel")->isVisible(),"diagnostics are separate from Connection");
        settingsTabs->setCurrentIndex(2);
        Check(window.findChild<QWidget*>("supportPanel")->isVisible(),"Support tab opens diagnostics directly");
        Check(!window.findChild<QPushButton*>("supportToggle"),"Support no longer needs an expansion toggle");
        Check(window.findChild<QLineEdit*>("mediaEndpoint")->isReadOnly(),"automatic socket is not editable");
        Check(!window.findChild<QPushButton*>("prepareButton")->isEnabled(),"no repair without service");
        auto* viewLog = window.findChild<QPushButton*>("viewLogButton");
        auto* openLogs = window.findChild<QPushButton*>("openLogsButton");
        auto* copyDiagnostics = window.findChild<QPushButton*>("copyDiagnosticsButton");
        auto* saveDiagnostics = window.findChild<QPushButton*>("saveDiagnosticsButton");
        Check(viewLog && openLogs && copyDiagnostics && saveDiagnostics,"support log controls exist");
        Check(!viewLog->isEnabled() && !copyDiagnostics->isEnabled(),"support controls wait for diagnostics");
        auto* client = window.findChild<ControlClient*>();
        Check(client,"control client exists");
        int operations = 0;
        QObject::connect(client,&ControlClient::Pending,[&](bool pending) { if (pending) ++operations; });
        emit client->Diagnostics("safe report","/var/log/barista/support",{"run-test.log","pairing-test.log"},"12345678-abcd");
        Check(viewLog->isEnabled() && openLogs->isEnabled() && copyDiagnostics->isEnabled() &&
            saveDiagnostics->isEnabled(),"support controls enable with diagnostics");
        Check(window.findChild<QComboBox*>("supportLogFiles")->count() == 2,"support logs are listed");
        Check(window.findChild<QLabel*>("supportId")->text().contains("12345678"),"support ID is shown");
        barista::api::SessionStatus status;
        status.available = true;
        status.capabilities.controller = true;
        auto apply = [&] {
            Check(QMetaObject::invokeMethod(&window,"ApplyStatus",Qt::DirectConnection,
                Q_ARG(barista::api::SessionStatus,status)),"apply status");
        };
        apply(); Check(start->isEnabled() && pair->isEnabled() && !stop->isEnabled(),"idle actions");
        auto* pattern = window.findChild<QLineEdit*>("pairingCode");
        Check(!window.findChild<QPushButton*>("newPairingPatternButton"),"pairing has no pattern editing action");
        Check(pattern && pattern->isReadOnly() && barista::api::ParsePairCode(pattern->text().toStdString()).has_value(),"startup generates a valid read-only pairing pattern");
        const QString originalPattern = pattern->text();
        emit client->GamePads({{"00:11:22:33:44:55","Living room GamePad"}});
        Check(window.findChild<QListWidget*>("savedGamePads")->count() == 1,"startup pattern preserves saved GamePads");
        Check(operations == 0,"generating a pattern does not call the service or replace credentials");
        const QStringList shapes{"♠","♥","♦","♣"};
        for (int i = 0; i < 4; ++i) {
            auto* symbol = window.findChild<QLabel*>(QString("pairingSymbol%1").arg(i + 1));
            Check(symbol && symbol->text() == shapes[pattern->text()[i].digitValue()],"display-only symbols match the submitted code");
            Check(!symbol->accessibleName().isEmpty(),"pairing symbols have accessible names");
        }
        window.OpenPairing();
        auto* pairDialog = window.findChild<QDialog*>("pairingDialog");
        Check(pairDialog && pairDialog->isVisible(),"pairing opens in its own dialog");
        Check(pairDialog->testAttribute(Qt::WA_StyledBackground),"pairing explicitly paints its styled background");
        const QImage pairingImage = pairDialog->grab().toImage();
        for (const QPoint point : {QPoint(8,8),QPoint(pairingImage.width() - 8,8),
                QPoint(8,pairingImage.height() - 8),QPoint(pairingImage.width() - 8,pairingImage.height() - 8)}) {
            const QColor pairingBackground = pairingImage.pixelColor(point);
            Check(pairingBackground.red() > 220 && pairingBackground.green() > 210 && pairingBackground.blue() > 190,
                "the entire pairing dialog background renders cream outside the symbol tiles");
        }
        Check(pairDialog->findChildren<QComboBox*>().isEmpty(),"pairing has no adapter or symbol selectors");
        Check(window.findChild<QLabel*>("pairingStatus")->property("lightSurface").toBool(),"pairing status uses cream-surface contrast");
        pairDialog->hide();
        window.OpenPairing();
        Check(pattern->text() == originalPattern,"reopening pairing preserves the startup pattern");
        pairDialog->hide();
        QRandomGenerator seeded(12345);
        QSet<QString> generated;
        for (int i = 0; i < 64; ++i) {
            const auto code = NewPairingPattern(seeded);
            const auto encoded = barista::api::PairCodeName(code);
            Check(barista::api::ParsePairCode(encoded).has_value(),"generated pattern stays within protocol alphabet");
            generated.insert(QString::fromStdString(encoded));
        }
        Check(generated.size() > 1,"generator does not return a fixed pattern");
        back->click();
        Check(start->isVisible() && !stop->isVisible(),"idle shows only connect action");
        controllerMode->click();
        Check(controllerMode->isChecked() && !screenMode->isChecked() &&
            window.findChild<QComboBox*>("modeCombo")->currentData() == "controller","segmented mode updates session mode");
        screenMode->click();
        auto* pairingStatus = window.findChild<QLabel*>("pairingStatus");
        Check(pairingStatus && pairingStatus->text().contains("Ready to start pairing"),"pairing is initially ready");
        status.busy = true; status.phase = barista::api::SessionPhase::Starting; apply();
        const QString frozenPattern = pattern->text();
        Check(pattern->text() == originalPattern,"pairing pattern cannot change during startup");
        Check(pairingStatus->text().contains("Starting pairing") && !pair->isEnabled(),"pairing startup is explained");
        status.busy = false; status.running = true; status.phase = barista::api::SessionPhase::Pairing; apply();
        Check(pattern->text() == frozenPattern && window.findChild<QLabel*>("pairingSymbol1")->isEnabled(),"active pairing keeps symbols unchanged and readable");
        Check(pairingStatus->text().contains("Pair now") && pairingStatus->text().contains("automatically") &&
            pair->text() == "Pairing active","pairing readiness is explicit");
        status.running = false; status.phase = barista::api::SessionPhase::Idle; apply();
        status.batteryPercent = 100; apply();
        Check(window.findChild<QLabel*>("gamepadBattery") && window.findChild<QLabel*>("gamepadBattery")->text() == "100%","GamePad battery is shown");
        status.error = barista::api::Error{.code=barista::api::ErrorCode::Failed,.message="Adapter failed",
            .diagnosticCode="ADAPTER_UNSUPPORTED",.action="Select another adapter."};
        apply();
        auto* supportDetails = window.findChild<QLabel*>("supportDetails");
        Check(supportDetails && supportDetails->text().contains("ADAPTER_UNSUPPORTED") &&
            supportDetails->text().contains("Select another adapter"),"diagnostic guidance is shown");
        status.running = true;
        status.error = barista::api::Error{.code=barista::api::ErrorCode::Failed,.message="Regulatory domain blocked",
            .diagnosticCode="AP_REGULATORY_BLOCKED",.action="Configure the country code."};
        apply();
        Check(pairingStatus->text().contains("regulatory domain") &&
            !pairingStatus->text().contains("another GamePad session"),"regulatory failure replaces misleading running-session text");
        status.running = false;
        status.error.reset(); apply();
        country->setText("U"); apply();
        Check(!start->isEnabled() && !pair->isEnabled(),"invalid regulatory country disables session actions");
        country->setText("US"); apply();
        Check(start->isEnabled() && pair->isEnabled(),"valid regulatory country enables session actions");
        auto* interfaceCombo = window.findChild<QComboBox*>("interfaceCombo");
        const auto selectedName = [](QComboBox* combo) {
            return combo->currentText().contains(' ') ? combo->currentData().toString() : combo->currentText();
        };
        Check(interfaceCombo,"Settings contains the adapter selector");
        Check(!interfaceCombo->isEditable() && !interfaceCombo->lineEdit(),"adapter is selection-only");
        QSvgRenderer dropdownArrow(QStringLiteral(":/barista/icons/dropdown.svg"));
        Check(dropdownArrow.isValid(),"dropdown uses the bundled cafe-colored Kenney caret");
        for (const QString name : {"wlan0","wlan1"})
            if (interfaceCombo->findData(name) < 0) interfaceCombo->addItem(name,name);
        interfaceCombo->setCurrentIndex(interfaceCombo->findData("wlan0"));
        Check(pairingStatus->text().contains("wlan0"),"pairing uses the Settings adapter");
        interfaceCombo->setCurrentIndex(interfaceCombo->findData("wlan1"));
        Check(pairingStatus->text().contains("wlan1") && pattern->text() == originalPattern,"Settings updates pairing adapter without changing symbols");
        preferences->click();
        settingsTabs->setCurrentIndex(0);
        QApplication::processEvents();
        const QPointF fieldPoint(12,interfaceCombo->height() / 2.0);
        QMouseEvent fieldClick(QEvent::MouseButtonPress,fieldPoint,
            interfaceCombo->mapToGlobal(fieldPoint.toPoint()),Qt::LeftButton,Qt::LeftButton,Qt::NoModifier);
        QApplication::sendEvent(interfaceCombo,&fieldClick);
        Check(interfaceCombo->view()->isVisible(),"clicking the adapter label area opens the dropdown");
        interfaceCombo->hidePopup();
        back->click();
        for (auto* action : {start,pair}) {
            bool warned = false, safeDefault = false;
            const auto selectedInterface = selectedName(interfaceCombo);
            QTimer::singleShot(0,[&] {
                if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    warned = dialog->text().contains("Internet access") && dialog->text().contains(selectedInterface) &&
                        dialog->informativeText().contains("US") && dialog->informativeText().contains("system-wide");
                    safeDefault = dialog->defaultButton() == dialog->button(QMessageBox::Cancel);
                    dialog->reject();
                }
            });
            action->click();
            Check(warned && safeDefault,"Wi-Fi warning with Cancel as default");
            Check(operations == 0,"cancel must not call backend");
        }
        status.running = true; status.ownedByCaller = true; apply();
        Check(!start->isEnabled() && !pair->isEnabled() && stop->isEnabled(),"owned running session");
        auto* sessionStatus = window.findChild<QLabel*>("sessionStatus");
        Check(sessionStatus && sessionStatus->styleSheet().contains("#efaaa0"),"waiting GamePad has readable warning color");
        Check(!start->isVisible() && stop->isVisible(),"running shows only disconnect action");
        Check(!screenMode->isEnabled() && !controllerMode->isEnabled(),"running session locks mode controls");
        status.gamePadConnected = true; apply();
        Check(sessionStatus->text().contains("connected") && sessionStatus->styleSheet().contains("#a4c49a"),"connected GamePad is green");
        Check(sessionStatus->isVisible(),"connection status remains visible after moving out of status bar");
        Check(window.findChild<QLabel*>("homeStatus")->styleSheet().contains("#17613e"),"Home uses a readable status color on cream");
        status.application.connected = true;
        status.application.name = "Example app";
        status.application.pid = 123;
        apply();
        Check(window.findChild<QLabel*>("applicationName")->text() == "Example app","application card uses a friendly name without process details");
        Check(window.findChild<QLabel*>("applicationSummary")->text().contains("connected"),"application card explains the connection");
        if (argc == 2) {
            QApplication::processEvents();
            const QString screenshot = QString::fromLocal8Bit(argv[1]);
            auto capture = [](QWidget* widget, const QString& path) {
                QApplication::processEvents();
                Check(!QFileInfo::exists(path) && widget->grab().save(path),"save café UI screenshot");
            };
            capture(&window,screenshot);
            tabs->setCurrentIndex(1); capture(&window,screenshot + ".gamepads.png");
            tabs->setCurrentIndex(2);
            settingsTabs->setCurrentIndex(1); capture(&window,screenshot + ".connection.png");
            settingsTabs->setCurrentIndex(0); capture(&window,screenshot + ".settings.png");
            settingsTabs->setCurrentIndex(2); capture(&window,screenshot + ".support.png");
            settingsTabs->setCurrentIndex(3); capture(&window,screenshot + ".about.png");
            const auto connectedStatus = status;
            status.running = false; status.gamePadConnected = false; status.phase = barista::api::SessionPhase::Idle; apply();
            window.OpenPairing(); capture(pairDialog,screenshot + ".pairing.png"); pairDialog->hide();
            status.running = true; status.phase = barista::api::SessionPhase::Runtime; apply();
            auto* waiting = window.findChild<QDialog*>("waitingDialog");
            waiting->show(); capture(waiting,screenshot + ".waiting.png");
            status = connectedStatus; apply();
            Check(!waiting->isVisible(),"waiting dialog closes on connection");
            back->click();
        }
        status.application = {};
        status.gamePadConnected = false;
        auto* background = window.findChild<QCheckBox*>("backgroundCheck");
        Check(background && background->isChecked(),"background enabled by default");
        window.close();
        Check(window.isMinimized() && operations == 0,"no-tray close minimizes without stopping session");
        window.ShowWindow(); Check(!window.isMinimized() && window.isVisible(),"restore background window");
        background->setChecked(false);
        bool quitPrompt = false;
        QTimer::singleShot(0,[&] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                quitPrompt = dialog->text().contains("stop your GamePad"); dialog->reject();
            }
        });
        window.close();
        Check(quitPrompt && window.isVisible() && operations == 0,"cancel quit keeps session");
        quitPrompt = false;
        QTimer::singleShot(0,[&] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                quitPrompt = dialog->text().contains("stop your GamePad"); dialog->reject();
            }
        });
        quitButton->click();
        Check(quitPrompt && window.isVisible() && operations == 0,"sidebar Quit preserves confirmation and cancellation");
        status.ownedByCaller = false; apply(); Check(!stop->isEnabled(),"cannot stop another session");
        status.ownedByCaller = true; status.phase = barista::api::SessionPhase::Stopping; apply(); Check(!stop->isEnabled(),"no repeated stops");
        status.running = false; status.busy = true; apply(); Check(!start->isEnabled() && !pair->isEnabled(),"pending authorization");
        status.busy = false; status.capabilities.systemPreparation = true; apply();
        auto* prepare = window.findChild<QPushButton*>("prepareButton");
        Check(prepare->isEnabled(),"idle system can prepare");
        bool setupPrompt = false;
        QTimer::singleShot(0,[&] {
            if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                setupPrompt = dialog->text().contains("NetworkManager") && dialog->defaultButton() == dialog->button(QMessageBox::Cancel);
                dialog->reject();
            }
        });
        prepare->click(); Check(setupPrompt && operations == 0,"cancel preparation makes no privileged call");
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
