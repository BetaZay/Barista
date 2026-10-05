#include "desktop_streamer.h"
#include "x11_capture.h"
#include <QApplication>
#include <QWidget>
#include <QScreen>
#include <QTemporaryDir>
#include <QEventLoop>
#include <QTimer>
#include <QCursor>
#include <xcb/xcb.h>
#include <cstdlib>
#include <iostream>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    if (QGuiApplication::platformName() != "xcb")
        return 1;
    QWidget window;
    window.setWindowTitle("Barista X11 capture fixture");
    window.setStyleSheet("background-color: rgb(200, 40, 90)");
    window.setGeometry(50, 50, 640, 360);
    window.show();
    QEventLoop exposed;
    QTimer::singleShot(200, &exposed, &QEventLoop::quit);
    exposed.exec();
    QGuiApplication::sync();
    QCursor::setPos(5, 5);
    // Provide the EWMH application list normally owned by an X11 window manager.
    auto* connection = xcb_connect(nullptr, nullptr);
    const auto root = xcb_setup_roots_iterator(xcb_get_setup(connection)).data->root;
    auto* atom = xcb_intern_atom_reply(
        connection, xcb_intern_atom(connection, false, 16, "_NET_CLIENT_LIST"), nullptr);
    const quint32 id = window.winId();
    xcb_change_property(connection, XCB_PROP_MODE_REPLACE, root, atom->atom, XCB_ATOM_WINDOW, 32, 1,
                        &id);
    std::free(atom);
    // Complete the property update before another connection reads the list.
    std::free(xcb_get_input_focus_reply(connection, xcb_get_input_focus(connection), nullptr));
    xcb_disconnect(connection);
    X11Capture native;
    bool listed = false;
    for (const auto& source : native.Windows())
        listed |= source.id == window.winId() && source.title == window.windowTitle();
    if (!listed)
    {
        std::cerr << "X11 application was not listed\n";
        return 2;
    }
    window.setCursor(Qt::CrossCursor);
    QCursor::setPos(220, 140);
    QApplication::processEvents();
    QString cursorError;
    const auto cursorFrame =
        native.Grab(QGuiApplication::primaryScreen(), quint32(window.winId()), cursorError);
    bool cursorVisible = false;
    if (!cursorFrame.isNull())
        for (int y = 80; y < 105; ++y)
            for (int x = 160; x < 185; ++x)
                cursorVisible |= cursorFrame.pixelColor(x, y) != QColor(200, 40, 90);
    if (!cursorError.isEmpty() || !cursorVisible)
    {
        std::cerr << "X11 cursor missing\n";
        return 8;
    }
    QCursor::setPos(5, 5);

    QTemporaryDir temporary;
    barista::api::AppHook server(true);
    std::string error;
    const auto endpoint = temporary.path() + "/media.sock";
    if (!server.start(endpoint.toStdString(), error))
    {
        std::cerr << error;
        return 3;
    }
    DesktopStreamer streamer;
    bool inputReceived = false, released = false;
    QObject::connect(&streamer, &DesktopStreamer::Input,
                     [&](const QByteArray& report)
                     {
                         if (report.size() == 128 && static_cast<unsigned char>(report[2]) == 0x80)
                             inputReceived = true;
                         if (report.isEmpty() && inputReceived)
                             released = true;
                     });
    QObject::connect(&streamer, &DesktopStreamer::Status,
                     [](const QString& status) { std::cout << status.toStdString() << '\n'; });
    QVariant selected;
    selected = QVariant::fromValue(quint32(window.winId()));
    if (!selected.isValid())
        return 4;
    // Test the actual desktop streamer -> AppHook RGB -> I420 path for both sources.
    for (bool application : {false, true})
    {
        bool passed = false, keyboardRequested = false, keyboardTextReceived = false;
        int lastCenter = -1, lastLeft = -1;
        QEventLoop loop;
        QTimer poll;
        QObject::connect(&streamer, &DesktopStreamer::Text, &poll, [&](const QString& text) {
            keyboardTextReceived = text == "Hello!";
        });
        std::vector<uint8_t> frame(barista::api::FrameBytes);
        std::array<uint8_t, 128> input{};
        input[2] = 0x80;
        QObject::connect(&poll, &QTimer::timeout,
                         [&]
                         {
                             server.submit_input(input);
                             bool active = false;
                             if (!server.read_video(frame, active) || !active)
                                 return;
                             // Rec.601 luma for the fixture color is approximately 96.
                             const int center = frame[240 * 864 + 427];
                             const int left = frame[240 * 864 + 20];
                             lastCenter = center;
                             lastLeft = left;
                             if (center < 93 || center > 99 || !inputReceived)
                                 return;
                             if (application ? (left < 93 || left > 99) : left > 20)
                                 return;
                             if (!keyboardRequested)
                             {
                                 keyboardRequested = true;
                                 if (application) input[80] = 0x40; // R3 shortcut.
                                 else streamer.ShowKeyboard();    // GUI button path.
                             }
                             barista::api::KeyboardCommand command;
                             if (server.read_keyboard_command(command))
                             {
                                 if (command.cancel || command.request.title != "Type into desktop" ||
                                     command.request.maxCharacters != 1024)
                                     return;
                                 server.submit_keyboard_result({command.request.id,
                                     barista::api::KeyboardOutcome::Submitted, "Hello!"}, command.connectionRevision);
                             }
                             if (!keyboardTextReceived) return;
                             passed = true;
                             loop.quit();
                         });
        QTimer::singleShot(8000, &loop, &QEventLoop::quit);
        streamer.Start(endpoint, QGuiApplication::primaryScreen(),
                       application ? selected : QVariant{});
        poll.start(10);
        loop.exec();
        streamer.Stop();
        if (!passed)
        {
            std::cerr << (application ? "Window" : "Monitor")
                      << " stream failed: center=" << lastCenter << " left=" << lastLeft
                      << " input=" << inputReceived << '\n';
            return 5;
        }
    }
    if (!released)
    {
        std::cerr << "Desktop input was not released on stop\n";
        return 6;
    }
    QString captureError;
    window.hide();
    QApplication::processEvents();
    // The capture connection is independent of Qt's connection. Wait until
    // the X server has processed Qt's unmap before checking window state.
    QGuiApplication::sync();
    if (!native.Grab(QGuiApplication::primaryScreen(), quint32(window.winId()), captureError)
             .isNull() ||
        captureError.isEmpty())
    {
        std::cerr << "Hidden X11 window was captured or returned no error\n";
        return 7;
    }
    std::cout << "X11 monitor/window streams, input bridge, release and unavailable-window checks "
                 "passed\n";
    return 0;
}
