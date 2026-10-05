#include "x11_capture.h"
#include <QGuiApplication>
#include <QScreen>
#include <QPixmap>
#include <QPainter>
#include <xcb/xcb.h>
#include <xcb/xfixes.h>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace
{
template <class T> using Reply = std::unique_ptr<T, decltype(&std::free)>;
}

X11Capture::X11Capture()
{
    m_connection = xcb_connect(nullptr, nullptr);
    if (xcb_connection_has_error(m_connection))
    {
        xcb_disconnect(m_connection);
        m_connection = nullptr;
        return;
    }
    const auto* extension = xcb_get_extension_data(m_connection, &xcb_xfixes_id);
    if (extension && extension->present)
    {
        Reply<xcb_xfixes_query_version_reply_t> version(
            xcb_xfixes_query_version_reply(m_connection,
                                           xcb_xfixes_query_version(m_connection, 4, 0), nullptr),
            &std::free);
        m_cursor = bool(version);
    }
}

X11Capture::~X11Capture()
{
    if (m_connection)
        xcb_disconnect(m_connection);
}

quint32 X11Capture::Atom(const char* name) const
{
    Reply<xcb_intern_atom_reply_t> atom(
        xcb_intern_atom_reply(
            m_connection, xcb_intern_atom(m_connection, true, std::strlen(name), name), nullptr),
        &std::free);
    return atom ? atom->atom : quint32(XCB_ATOM_NONE);
}

QByteArray X11Capture::Property(quint32 window, quint32 atom, quint32 type) const
{
    if (!atom)
        return {};
    Reply<xcb_get_property_reply_t> value(
        xcb_get_property_reply(m_connection,
                               xcb_get_property(m_connection, false, window, atom, type, 0, 65536),
                               nullptr),
        &std::free);
    if (!value || !value->format)
        return {};
    return QByteArray(static_cast<const char*>(xcb_get_property_value(value.get())),
                      xcb_get_property_value_length(value.get()));
}

QList<X11WindowSource> X11Capture::Windows() const
{
    QList<X11WindowSource> result;
    if (!m_connection)
        return result;
    auto roots = xcb_setup_roots_iterator(xcb_get_setup(m_connection));
    for (; roots.rem; xcb_screen_next(&roots))
    {
        auto windows =
            Property(roots.data->root, Atom("_NET_CLIENT_LIST_STACKING"), XCB_ATOM_WINDOW);
        if (windows.isEmpty())
            windows = Property(roots.data->root, Atom("_NET_CLIENT_LIST"), XCB_ATOM_WINDOW);
        if (windows.isEmpty())
        {
            // Minimal window managers and the isolated test server need no EWMH list.
            Reply<xcb_query_tree_reply_t> tree(
                xcb_query_tree_reply(m_connection, xcb_query_tree(m_connection, roots.data->root),
                                     nullptr),
                &std::free);
            if (tree)
                windows =
                    QByteArray(reinterpret_cast<const char*>(xcb_query_tree_children(tree.get())),
                               xcb_query_tree_children_length(tree.get()) * sizeof(quint32));
        }
        for (qsizetype offset = 0; offset + qsizetype(sizeof(quint32)) <= windows.size();
             offset += sizeof(quint32))
        {
            quint32 window;
            std::memcpy(&window, windows.constData() + offset, sizeof(window));
            Reply<xcb_get_window_attributes_reply_t> attributes(
                xcb_get_window_attributes_reply(
                    m_connection, xcb_get_window_attributes(m_connection, window), nullptr),
                &std::free);
            if (!attributes || attributes->map_state != XCB_MAP_STATE_VIEWABLE ||
                attributes->override_redirect)
                continue;
            auto title = Property(window, Atom("_NET_WM_NAME"), Atom("UTF8_STRING"));
            if (title.isEmpty())
                title = Property(window, XCB_ATOM_WM_NAME, XCB_GET_PROPERTY_TYPE_ANY);
            if (!title.isEmpty())
                result.append({window, QString::fromUtf8(title)});
        }
    }
    return result;
}

QImage X11Capture::Grab(QScreen* screen, quint32 window, QString& error) const
{
    error.clear();
    if (!m_connection)
    {
        error = "Cannot connect to the X11 display.";
        return {};
    }
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
    {
        error = "The selected monitor is no longer available.";
        return {};
    }
    QPoint origin = screen->geometry().topLeft();
    if (window)
    {
        Reply<xcb_get_window_attributes_reply_t> attributes(
            xcb_get_window_attributes_reply(
                m_connection, xcb_get_window_attributes(m_connection, window), nullptr),
            &std::free);
        Reply<xcb_get_geometry_reply_t> geometry(
            xcb_get_geometry_reply(m_connection, xcb_get_geometry(m_connection, window), nullptr),
            &std::free);
        if (!attributes || !geometry || attributes->map_state != XCB_MAP_STATE_VIEWABLE)
        {
            error = "The selected window was closed or minimized. Choose a source again.";
            return {};
        }
        Reply<xcb_translate_coordinates_reply_t> position(
            xcb_translate_coordinates_reply(
                m_connection, xcb_translate_coordinates(m_connection, window, geometry->root, 0, 0),
                nullptr),
            &std::free);
        if (!position)
        {
            error = "Cannot locate the selected window.";
            return {};
        }
        origin = QPoint(position->dst_x, position->dst_y);
    }
    auto image = screen->grabWindow(window).toImage();
    if (image.isNull())
    {
        error = "X11 could not capture the selected source.";
        return {};
    }
    if (m_cursor)
    {
        Reply<xcb_xfixes_get_cursor_image_reply_t> cursor(
            xcb_xfixes_get_cursor_image_reply(m_connection,
                                              xcb_xfixes_get_cursor_image(m_connection), nullptr),
            &std::free);
        if (cursor && cursor->width && cursor->height)
        {
            QImage cursorImage(reinterpret_cast<const uchar*>(
                                   xcb_xfixes_get_cursor_image_cursor_image(cursor.get())),
                               cursor->width, cursor->height, QImage::Format_ARGB32_Premultiplied);
            QPainter painter(&image);
            const auto dpr = image.devicePixelRatio();
            cursorImage.setDevicePixelRatio(dpr);
            painter.drawImage(QPointF((cursor->x - origin.x() - cursor->xhot) / dpr,
                                      (cursor->y - origin.y() - cursor->yhot) / dpr),
                              cursorImage);
        }
    }
    return image;
}
