#pragma once
#include "api/app_hook.h"
#include <QObject>
#include <QImage>
#include <QTimer>
#include <QElapsedTimer>
#include <QVariant>
#include <QScreen>
#include <QPointer>
#if QT_VERSION >= QT_VERSION_CHECK(6, 6, 0) && !defined(BARISTA_FORCE_LEGACY_CAPTURE)
#define BARISTA_QT_CAPTURE 1
#include <QMediaCaptureSession>
#include <QScreenCapture>
#include <QWindowCapture>
#include <QVideoSink>
#endif
#include <memory>
class PortalCapture;
class X11Capture;

class DesktopStreamer : public QObject
{
    Q_OBJECT
public:
    explicit DesktopStreamer(QObject* parent = nullptr);
    ~DesktopStreamer() override;
    void Start(const QString& endpoint, QScreen* screen, const QVariant& window = {});
    void Stop();
    void ShowKeyboard();
    bool Requested() const
    {
        return m_requested;
    }
    bool KeyboardAvailable() const
    {
        return m_hook && m_hook->connected() && !m_rgb.empty() && !m_keyboardId;
    }
    static QImage Letterbox(const QImage& image);
signals:
    void Status(const QString& text);
    void Input(const QByteArray& report);
    void Text(const QString& text);

private:
    void Receive(const QImage& image);
    void Tick();
    bool m_requested = false;
    uint32_t m_previousButtons = 0, m_keyboardId = 0, m_nextKeyboardId = 1;
    std::unique_ptr<barista::api::AppHook> m_hook;
#ifdef BARISTA_QT_CAPTURE
    QMediaCaptureSession m_session;
    QScreenCapture m_screen;
    QWindowCapture m_window;
    QVideoSink m_sink;
#else
    QPointer<QScreen> m_screen;
#endif
#ifdef BARISTA_X11_CAPTURE
    std::unique_ptr<X11Capture> m_x11;
    QPointer<QScreen> m_x11Screen;
    quint32 m_windowId = 0;
#endif
    QTimer m_timer;
    QElapsedTimer m_captureClock;
    std::vector<uint8_t> m_rgb;
    qint64 m_lastFrame = -1;
#ifdef BARISTA_PORTAL_CAPTURE
    PortalCapture* m_portal = nullptr;
#endif
};
