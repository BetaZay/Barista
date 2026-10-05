#pragma once
#include <QObject>
#include <QImage>
#include <QVariantMap>
#include <QTimer>
#include <atomic>
struct pw_thread_loop;
struct pw_context;
struct pw_core;
struct pw_stream;
struct spa_hook;
struct spa_pod;

// Unprivileged ScreenCast portal client. The compositor chooses the source.
class PortalCapture : public QObject
{
    Q_OBJECT
  public:
    explicit PortalCapture(QObject *parent = nullptr);
    ~PortalCapture() override;
    void Start();
    void Stop();
  signals:
    void Frame(const QImage &image);
    void Error(const QString &error);
  private slots:
    void Response(uint code, const QVariantMap &result);
    void Closed();

  private:
    void Request(const QString &method, QVariantList arguments, QVariantMap options);
    void OpenRemote(uint node);
    bool ConnectPipeWire(int fd, uint node);
    static void Format(void *data, uint id, const spa_pod *parameter);
    static void Process(void *data);

    QString m_session, m_request;
    int m_stage = 0;
    std::atomic_uint m_generation{0};
    QTimer m_timeout;
    pw_thread_loop *m_loop = nullptr;
    pw_context *m_context = nullptr;
    pw_core *m_core = nullptr;
    pw_stream *m_stream = nullptr;
    spa_hook *m_listener = nullptr;
    uint m_width = 0, m_height = 0, m_format = 0;
    std::atomic_bool m_framePending{false};
};
