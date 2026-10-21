#include "desktop_streamer.h"
#include "desktop_backend.h"
#include "api/controller.h"
#include <QGuiApplication>
#include <QPainter>
#ifdef BARISTA_QT_CAPTURE
#include <QVideoFrame>
#endif
#include <cstring>
#ifdef BARISTA_PORTAL_CAPTURE
#include "portal_capture.h"
#endif
#ifdef BARISTA_X11_CAPTURE
#include "x11_capture.h"
#endif
DesktopStreamer::DesktopStreamer(QObject* parent) : QObject(parent)
{
#ifdef BARISTA_QT_CAPTURE
    m_session.setScreenCapture(&m_screen);
    m_session.setWindowCapture(&m_window);
    m_session.setVideoSink(&m_sink);
    connect(&m_sink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame& frame) { Receive(frame.toImage()); });
    auto failed = [this](const QString& text)
    {
        Stop();
        emit Status("Sharing stopped: " + text);
    };
    connect(&m_screen, &QScreenCapture::errorOccurred, this,
            [failed](auto, const QString& text) { failed(text); });
    connect(&m_window, &QWindowCapture::errorOccurred, this,
            [failed](auto, const QString& text) { failed(text); });
#else
    auto failed = [this](const QString& text)
    {
        Stop();
        emit Status("Sharing stopped: " + text);
    };
#endif
#ifdef BARISTA_PORTAL_CAPTURE
    m_portal = new PortalCapture(this);
    connect(m_portal, &PortalCapture::Frame, this, &DesktopStreamer::Receive);
    connect(m_portal, &PortalCapture::Error, this, failed);
#endif
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setInterval(16);
    connect(&m_timer, &QTimer::timeout, this, &DesktopStreamer::Tick);
}
DesktopStreamer::~DesktopStreamer()
{
    Stop();
}
QImage DesktopStreamer::Letterbox(const QImage& image)
{
    if (image.isNull())
        return {};
    QImage output(864, 480, QImage::Format_RGB888);
    output.fill(Qt::black);
    const auto scaled = image.scaled(854, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    QPainter painter(&output);
    painter.drawImage((854 - scaled.width()) / 2, (480 - scaled.height()) / 2, scaled);
    return output;
}
void DesktopStreamer::Start(const QString& endpoint, QScreen* screen, const QVariant& window)
{
    Stop();
    if (endpoint.isEmpty())
    {
        emit Status("Start a desktop session before sharing.");
        return;
    }
    m_hook = std::make_unique<barista::api::AppHook>(false);
    std::string error;
    if (!m_hook->start(endpoint.toStdString(), error))
    {
        m_hook.reset();
        emit Status(QString::fromStdString(error));
        return;
    }
    m_requested = true;
    m_captureClock.start();
    m_timer.start();
    emit Status("Choose a monitor or application to share.");
#ifdef BARISTA_PORTAL_CAPTURE
    if (DesktopUsesPortal(QGuiApplication::platformName(),
                          qEnvironmentVariable("XDG_SESSION_TYPE")))
    {
        m_portal->Start();
        return;
    }
#endif
#ifdef BARISTA_X11_CAPTURE
    if (QGuiApplication::platformName() == "xcb")
    {
        m_x11Screen = screen ? screen : QGuiApplication::primaryScreen();
        m_windowId = window.toUInt();
        m_x11 = std::make_unique<X11Capture>();
        return;
    }
#endif
#ifdef BARISTA_QT_CAPTURE
    const auto captureWindow = window.value<QCapturableWindow>();
    if (captureWindow.isValid())
    {
        m_window.setWindow(captureWindow);
        m_window.start();
    }
    else
    {
        m_screen.setScreen(screen);
        m_screen.start();
    }
#else
    Q_UNUSED(window);
    m_screen = screen ? screen : QGuiApplication::primaryScreen();
#endif
}
void DesktopStreamer::Receive(const QImage& image)
{
    if (!m_requested || image.isNull())
        return;
    // Bound expensive scaling/conversion even on high-refresh-rate displays.
    if (m_lastFrame >= 0 && m_captureClock.elapsed() - m_lastFrame < 15)
        return;
    auto frame = Letterbox(image);
    m_rgb.resize(864 * 480 * 3);
    for (int y = 0; y < 480; ++y)
        std::memcpy(m_rgb.data() + y * 864 * 3, frame.constScanLine(y), 864 * 3);
    if (m_rgb.size() && m_lastFrame < 0)
        emit Status("Desktop sharing active.");
    m_lastFrame = m_captureClock.elapsed();
}
void DesktopStreamer::ShowKeyboard()
{
    if (!m_hook || !m_hook->connected() || m_rgb.empty() || m_keyboardId) return;
    const barista::api::KeyboardRequest request{m_nextKeyboardId++, "Type into desktop", {}, 1024, false};
    if (m_nextKeyboardId == 0) m_nextKeyboardId = 1;
    if (m_hook->request_keyboard(request)) m_keyboardId = request.id;
}
void DesktopStreamer::Tick()
{
    if (!m_hook)
        return;
#ifdef BARISTA_X11_CAPTURE
    if (m_x11)
    {
        if (!m_x11Screen)
        {
            Stop();
            emit Status("The selected monitor is no longer available.");
            return;
        }
        QString error;
        auto frame = m_x11->Grab(m_x11Screen, m_windowId, error);
        if (!error.isEmpty())
        {
            Stop();
            emit Status(error);
            return;
        }
        Receive(frame);
    }
#endif
#if !defined(BARISTA_QT_CAPTURE) && !defined(BARISTA_X11_CAPTURE)
    if (m_screen)
        Receive(m_screen->grabWindow(0).toImage());
#endif
    const bool active = m_hook->connected() && !m_rgb.empty();
    m_hook->set_active(active);
    if (active)
        m_hook->submit_rgb(m_rgb, 864, 480);
    std::array<uint8_t, 128> report{};
    if (active && m_hook->read_input(report))
    {
        const auto buttons = barista::DecodeInput(report).buttons;
        if ((buttons & ~m_previousButtons) & 0x400000) ShowKeyboard(); // Right stick click.
        m_previousButtons = buttons;
        emit Input(QByteArray(reinterpret_cast<const char*>(report.data()), report.size()));
    }
    else
    {
        m_previousButtons = 0;
        emit Input({}); // Release held mouse/keyboard state when pad/bridge disconnects.
    }
    barista::api::KeyboardResult result;
    while (m_hook->read_keyboard_result(result))
        if (result.id == m_keyboardId)
        {
            m_keyboardId = 0;
            if (result.outcome == barista::api::KeyboardOutcome::Submitted)
                emit Text(QString::fromStdString(result.text));
        }
    if (!m_hook->connected()) m_keyboardId = 0;
}
void DesktopStreamer::Stop()
{
    m_requested = false;
    m_previousButtons = m_keyboardId = 0;
    m_timer.stop();
#ifdef BARISTA_QT_CAPTURE
    m_screen.stop();
    m_window.stop();
#else
    m_screen.clear();
#endif
#ifdef BARISTA_X11_CAPTURE
    m_x11.reset();
    m_x11Screen.clear();
    m_windowId = 0;
#endif
#ifdef BARISTA_PORTAL_CAPTURE
    if (m_portal)
        m_portal->Stop();
#endif
    if (m_hook)
    {
        m_hook->set_active(false);
        m_hook->stop();
        m_hook.reset();
    }
    m_rgb.clear();
    m_lastFrame = -1;
    emit Input({});
}
