#include "portal_capture.h"
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusArgument>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QUuid>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/format-utils.h>
#include <unistd.h>
#include <cstring>
#include <limits>

namespace
{
constexpr auto Portal = "org.freedesktop.portal.Desktop";
constexpr auto Path = "/org/freedesktop/portal/desktop";
constexpr auto ScreenCast = "org.freedesktop.portal.ScreenCast";
QString Token()
{
    return "barista" + QUuid::createUuid().toString(QUuid::Id128);
}
} // namespace
PortalCapture::PortalCapture(QObject *parent) : QObject(parent)
{
    m_timeout.setSingleShot(true);
    connect(&m_timeout, &QTimer::timeout, this,
            [this]
            {
                Stop();
                emit Error("Screen sharing timed out. Choose a source again.");
            });
}
PortalCapture::~PortalCapture()
{
    Stop();
}
void PortalCapture::Start()
{
    Stop();
    m_stage = 1;
    Request("CreateSession", {}, {{"session_handle_token", Token()}});
}
void PortalCapture::Request(const QString &method, QVariantList arguments, QVariantMap options)
{
    auto bus = QDBusConnection::sessionBus();
    QString sender = bus.baseService().mid(1);
    sender.replace('.', '_');
    const auto token = Token();
    options.insert("handle_token", token);
    m_request = "/org/freedesktop/portal/desktop/request/" + sender + '/' + token;
    if (!bus.connect(Portal, m_request, "org.freedesktop.portal.Request", "Response", this,
                     SLOT(Response(uint, QVariantMap))))
    {
        Stop();
        emit Error("Cannot listen for the desktop sharing request.");
        return;
    }
    auto message = QDBusMessage::createMethodCall(Portal, Path, ScreenCast, method);
    arguments.append(options);
    message.setArguments(arguments);
    const uint generation = m_generation;
    auto *watcher = new QDBusPendingCallWatcher(bus.asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, generation](auto *finished)
            {
                const QDBusPendingReply<QDBusObjectPath> reply = *finished;
                if (generation == m_generation && reply.isError())
                {
                    const auto error = reply.error().message();
                    Stop();
                    emit Error("Desktop sharing: " + error);
                }
                finished->deleteLater();
            });
    m_timeout.start(120000);
}
void PortalCapture::Response(uint code, const QVariantMap &result)
{
    QDBusConnection::sessionBus().disconnect(Portal, m_request, "org.freedesktop.portal.Request",
                                             "Response", this, SLOT(Response(uint, QVariantMap)));
    m_request.clear();
    m_timeout.stop();
    if (code != 0)
    {
        Stop();
        emit Error("Screen sharing was cancelled or denied. Choose a source to try again.");
        return;
    }
    if (m_stage == 1)
    {
        m_session = result.value("session_handle").value<QDBusObjectPath>().path();
        if (m_session.isEmpty())
            m_session = result.value("session_handle").toString();
        if (!m_session.startsWith("/org/freedesktop/portal/desktop/session/"))
        {
            Stop();
            emit Error("The desktop returned an invalid sharing session.");
            return;
        }
        QDBusConnection::sessionBus().connect(Portal, m_session, "org.freedesktop.portal.Session",
                                              "Closed", this, SLOT(Closed()));
        m_stage = 2;
        Request("SelectSources", {QVariant::fromValue(QDBusObjectPath(m_session))},
                {{"types", uint(3)}, {"multiple", false}, {"cursor_mode", uint(2)}});
    }
    else if (m_stage == 2)
    {
        m_stage = 3;
        Request("Start", {QVariant::fromValue(QDBusObjectPath(m_session)), QString()}, {});
    }
    else if (m_stage == 3)
    {
        const auto argument = qvariant_cast<QDBusArgument>(result.value("streams"));
        uint node = 0;
        argument.beginArray();
        if (!argument.atEnd())
        {
            QVariantMap properties;
            argument.beginStructure();
            argument >> node >> properties;
            argument.endStructure();
        }
        argument.endArray();
        if (!node)
        {
            Stop();
            emit Error("No monitor or window was selected.");
            return;
        }
        m_stage = 4;
        OpenRemote(node);
    }
}
void PortalCapture::OpenRemote(uint node)
{
    auto message = QDBusMessage::createMethodCall(Portal, Path, ScreenCast, "OpenPipeWireRemote");
    message.setArguments({QVariant::fromValue(QDBusObjectPath(m_session)), QVariantMap{}});
    const uint generation = m_generation;
    auto *watcher =
        new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, node, generation](auto *finished)
            {
                const QDBusPendingReply<QDBusUnixFileDescriptor> reply = *finished;
                if (generation == m_generation)
                {
                    if (reply.isError() ||
                        !ConnectPipeWire(dup(reply.value().fileDescriptor()), node))
                    {
                        Stop();
                        emit Error("Could not open the selected desktop video stream.");
                    }
                }
                finished->deleteLater();
            });
}
bool PortalCapture::ConnectPipeWire(int fd, uint node)
{
    if (fd < 0)
        return false;
    pw_init(nullptr, nullptr);
    m_loop = pw_thread_loop_new("barista-desktop", nullptr);
    if (!m_loop)
    {
        close(fd);
        return false;
    }
    m_context = pw_context_new(pw_thread_loop_get_loop(m_loop), nullptr, 0);
    if (!m_context)
    {
        close(fd);
        return false;
    }
    m_core = pw_context_connect_fd(m_context, fd, nullptr, 0); // Takes ownership of fd.
    if (!m_core)
        return false;
    m_stream = pw_stream_new(m_core, "Barista desktop mirror",
                             pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY,
                                               "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr));
    if (!m_stream)
        return false;
    static const pw_stream_events events = []
    {
        pw_stream_events value{};
        value.version = PW_VERSION_STREAM_EVENTS;
        value.param_changed = &PortalCapture::Format;
        value.process = &PortalCapture::Process;
        value.state_changed =
            [](void *data, pw_stream_state, pw_stream_state state, const char *error)
        {
            if (state != PW_STREAM_STATE_ERROR)
                return;
            auto *self = static_cast<PortalCapture *>(data);
            QMetaObject::invokeMethod(
                self,
                [self, generation = self->m_generation.load(),
                 text = QString::fromUtf8(error ? error : "Video stream failed")]
                {
                    if (generation != self->m_generation)
                        return;
                    self->Stop();
                    emit self->Error(text);
                },
                Qt::QueuedConnection);
        };
        return value;
    }();
    m_listener = new spa_hook{};
    pw_stream_add_listener(m_stream, m_listener, &events, this);
    uint8_t buffer[1024];
    auto builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const spa_pod *format = static_cast<const spa_pod *>(spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat, SPA_FORMAT_mediaType,
        SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
        SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(4, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_RGBx,
                               SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBA)));
    if (pw_stream_connect(
            m_stream, PW_DIRECTION_INPUT, node,
            static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
            &format, 1) < 0)
        return false;
    return pw_thread_loop_start(m_loop) >= 0;
}
void PortalCapture::Format(void *data, uint id, const spa_pod *parameter)
{
    if (id != SPA_PARAM_Format || !parameter)
        return;
    auto *self = static_cast<PortalCapture *>(data);
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(parameter, &info) >= 0)
    {
        self->m_width = info.size.width;
        self->m_height = info.size.height;
        self->m_format = info.format;
        uint8_t buffer[256];
        auto builder = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const auto *buffers = static_cast<const spa_pod *>(spa_pod_builder_add_object(
            &builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
            SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd))));
        pw_stream_update_params(self->m_stream, &buffers, 1);
    }
}
void PortalCapture::Process(void *data)
{
    auto *self = static_cast<PortalCapture *>(data);
    pw_buffer *latest = nullptr;
    while (auto *next = pw_stream_dequeue_buffer(self->m_stream))
    {
        if (latest)
            pw_stream_queue_buffer(self->m_stream, latest);
        latest = next;
    }
    if (!latest)
        return;
    auto *b = latest->buffer;
    if (b->n_datas && self->m_width && self->m_height && !self->m_framePending.load())
    {
        auto &d = b->datas[0];
        const auto stride = d.chunk ? d.chunk->stride : 0;
        const auto offset = d.chunk ? d.chunk->offset : 0;
        const uint64_t required = uint64_t(stride > 0 ? stride : 0) * self->m_height;
        if (d.data && d.chunk && d.chunk->size >= required &&
            stride >= int64_t(self->m_width) * 4 && required <= d.maxsize &&
            offset <= d.maxsize - required && self->m_width <= 16384 && self->m_height <= 16384)
        {
            auto format = self->m_format == SPA_VIDEO_FORMAT_BGRx   ? QImage::Format_RGB32
                          : self->m_format == SPA_VIDEO_FORMAT_BGRA ? QImage::Format_ARGB32
                          : self->m_format == SPA_VIDEO_FORMAT_RGBA ? QImage::Format_RGBA8888
                                                                    : QImage::Format_RGBX8888;
            QImage image(static_cast<const uchar *>(d.data) + offset, self->m_width, self->m_height,
                         stride, format);
            self->m_framePending = true;
            QMetaObject::invokeMethod(
                self,
                [self, copy = image.copy(), generation = self->m_generation.load()]
                {
                    self->m_framePending = false;
                    if (generation == self->m_generation && self->m_stage == 4)
                        emit self->Frame(copy);
                },
                Qt::QueuedConnection);
        }
    }
    pw_stream_queue_buffer(self->m_stream, latest);
}
void PortalCapture::Stop()
{
    ++m_generation;
    m_stage = 0;
    m_timeout.stop();
    auto bus = QDBusConnection::sessionBus();
    if (!m_request.isEmpty())
    {
        bus.disconnect(Portal, m_request, "org.freedesktop.portal.Request", "Response", this,
                       SLOT(Response(uint, QVariantMap)));
        bus.asyncCall(QDBusMessage::createMethodCall(Portal, m_request,
                                                     "org.freedesktop.portal.Request", "Close"));
        m_request.clear();
    }
    if (!m_session.isEmpty())
    {
        bus.disconnect(Portal, m_session, "org.freedesktop.portal.Session", "Closed", this,
                       SLOT(Closed()));
        bus.asyncCall(QDBusMessage::createMethodCall(Portal, m_session,
                                                     "org.freedesktop.portal.Session", "Close"));
        m_session.clear();
    }
    if (m_loop)
        pw_thread_loop_stop(m_loop);
    if (m_stream)
    {
        pw_stream_destroy(m_stream);
        m_stream = nullptr;
    }
    delete m_listener;
    m_listener = nullptr;
    if (m_core)
    {
        pw_core_disconnect(m_core);
        m_core = nullptr;
    }
    if (m_context)
    {
        pw_context_destroy(m_context);
        m_context = nullptr;
    }
    if (m_loop)
    {
        pw_thread_loop_destroy(m_loop);
        m_loop = nullptr;
    }
    m_width = m_height = m_format = 0;
}

void PortalCapture::Closed()
{
    Stop();
    emit Error("Desktop sharing ended. Choose a source to resume.");
}
