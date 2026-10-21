#pragma once
#include <QImage>
#include <QList>
#include <QString>
class QScreen;
struct xcb_connection_t;

struct X11WindowSource
{
    quint32 id;
    QString title;
};

// Native X11 window selection and visible cursor capture, including Qt 6.4.
class X11Capture
{
public:
    X11Capture();
    ~X11Capture();
    X11Capture(const X11Capture&) = delete;
    X11Capture& operator=(const X11Capture&) = delete;
    QList<X11WindowSource> Windows() const;
    QImage Grab(QScreen* screen, quint32 window, QString& error) const;

private:
    quint32 Atom(const char* name) const;
    QByteArray Property(quint32 window, quint32 atom, quint32 type) const;
    xcb_connection_t* m_connection = nullptr;
    bool m_cursor = false;
};
