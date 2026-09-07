#include "window.h"
#include <QApplication>
#include <QMessageBox>
#include <QPushButton>
#include <QTabWidget>
#include <QTimer>
#include <QCheckBox>
#include <QComboBox>
#include <QLineEdit>
#include <QLabel>
#include <QToolBar>
#include <iostream>
#include <stdexcept>

void Check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
int main(int argc, char** argv)
{
    QApplication app(argc,argv);
    try {
        Window window(true); window.show();
        auto* start = window.findChild<QPushButton*>("startButton");
        auto* stop = window.findChild<QPushButton*>("stopButton");
        auto* pair = window.findChild<QPushButton*>("pairButton");
        auto* pairInterface = window.findChild<QComboBox*>("pairInterfaceCombo");
        auto* advanced = window.findChild<QWidget*>("advancedPanel");
        Check(start && stop && pair && pairInterface && advanced,"required controls");
        Check(!window.findChild<QToolBar*>("sessionToolbar"),"session controls are not duplicated in a toolbar");
        Check(!start->isEnabled() && !stop->isEnabled(),"service missing disables actions");
        auto* tabs = window.findChild<QTabWidget*>();
        Check(tabs->count() == 3 && tabs->tabText(2) == "Advanced", "advanced tab exists");
        Check(!advanced->isVisible(),"technical details not on General");
        tabs->setCurrentIndex(2); Check(advanced->isVisible(),"advanced tab opens");
        Check(window.findChild<QLineEdit*>("mediaEndpoint")->isReadOnly(),"automatic socket is not editable");
        Check(!window.findChild<QPushButton*>("prepareButton")->isEnabled(),"no repair without service");
        QVariantMap status{{"available",true},{"running",false},{"controllerSupported",true},{"phase","idle"}};
        auto apply = [&] { Check(QMetaObject::invokeMethod(&window,"ApplyStatus",Qt::DirectConnection,Q_ARG(QVariantMap,status)),"apply status"); };
        apply(); Check(start->isEnabled() && pair->isEnabled() && !stop->isEnabled(),"idle actions");
        auto* pairingStatus = window.findChild<QLabel*>("pairingStatus");
        Check(pairingStatus && pairingStatus->text().contains("Ready to start pairing"),"pairing is initially ready");
        status["busy"] = true; status["phase"] = "starting"; apply();
        Check(pairingStatus->text().contains("Starting pairing") && !pair->isEnabled(),"pairing startup is explained");
        status["busy"] = false; status["running"] = true; status["phase"] = "pairing"; apply();
        Check(pairingStatus->text().contains("Pair now") && pairingStatus->text().contains("automatically") &&
            pair->text() == "Pairing active","pairing readiness is explicit");
        status["running"] = false; status["phase"] = "idle"; apply();
        status["batteryAvailable"] = true; status["battery"] = 100; apply();
        Check(window.findChild<QLabel*>("gamepadBattery") && window.findChild<QLabel*>("gamepadBattery")->text() == "100%","GamePad battery is shown");
        int operations = 0;
        QObject::connect(window.findChild<ControlClient*>(),&ControlClient::Pending,[&](bool pending) { if (pending) ++operations; });
        pairInterface->setCurrentText("wlan1");
        auto* interfaceCombo = window.findChild<QComboBox*>("interfaceCombo");
        const auto selectedName = [](QComboBox* combo) {
            return combo->currentText().contains(' ') ? combo->currentData().toString() : combo->currentText();
        };
        Check(interfaceCombo && selectedName(interfaceCombo) == "wlan1","pairing adapter updates session adapter");
        interfaceCombo->setCurrentText("wlan0");
        Check(selectedName(pairInterface) == "wlan0","session adapter updates pairing adapter");
        pairInterface->setCurrentText("wlan1");
        for (auto* action : {start,pair}) {
            bool warned = false, safeDefault = false;
            auto* selectedCombo = action == pair ? pairInterface : interfaceCombo;
            const auto selectedInterface = selectedName(selectedCombo);
            QTimer::singleShot(0,[&] {
                if (auto* dialog = qobject_cast<QMessageBox*>(QApplication::activeModalWidget())) {
                    warned = dialog->text().contains("Internet access") && dialog->text().contains(selectedInterface);
                    safeDefault = dialog->defaultButton() == dialog->button(QMessageBox::Cancel);
                    dialog->reject();
                }
            });
            action->click();
            Check(warned && safeDefault,"Wi-Fi warning with Cancel as default");
            Check(operations == 0,"cancel must not call backend");
        }
        status["running"] = true; status["ownedByCaller"] = true; apply();
        Check(!start->isEnabled() && !pair->isEnabled() && stop->isEnabled(),"owned running session");
        auto* sessionStatus = window.findChild<QLabel*>("sessionStatus");
        Check(sessionStatus && sessionStatus->styleSheet().contains("#b3261e"),"waiting GamePad is red");
        status["connected"] = true; apply();
        Check(sessionStatus->text().contains("connected") && sessionStatus->styleSheet().contains("#207a3b"),"connected GamePad is green");
        status["connected"] = false;
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
        status["ownedByCaller"] = false; apply(); Check(!stop->isEnabled(),"cannot stop another session");
        status["ownedByCaller"] = true; status["phase"] = "stopping"; apply(); Check(!stop->isEnabled(),"no repeated stops");
        status["running"] = false; status["busy"] = true; apply(); Check(!start->isEnabled() && !pair->isEnabled(),"pending authorization");
        status["busy"] = false; status["setupSupported"] = true; apply();
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
