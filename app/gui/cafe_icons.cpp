#include "cafe_icons.h"
#include <QIconEngine>
#include <QPainter>
#include <QSvgRenderer>
#include <algorithm>
#include <array>

namespace
{
QString IconResource(CafeSymbol symbol)
{
    static constexpr std::array names{
        "home", "gamepad-alt", "signal-high", "cog", "info-circle", "plus",
        "caret-right", "stop", "reload", "copy", "external", "save", "pencil", "trash", "exit", "computer"
    };
    return QString(":/barista/icons/kenney/%1.svg").arg(names.at(static_cast<size_t>(symbol)));
}

class CafeIconEngine final : public QIconEngine
{
public:
    CafeIconEngine(CafeSymbol symbol, QColor color)
        : m_symbol(symbol), m_color(color), m_renderer(IconResource(symbol))
    {
    }

    QIconEngine* clone() const override { return new CafeIconEngine(m_symbol,m_color); }

    QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
    {
        QPixmap image(size);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        paint(&painter,QRect(QPoint(),size),mode,state);
        return image;
    }

    void paint(QPainter* painter, const QRect& rect, QIcon::Mode mode, QIcon::State) override
    {
        if (rect.isEmpty()) return;
        const qreal ratio = painter->device()->devicePixelRatioF();
        QImage image(rect.size() * ratio,QImage::Format_ARGB32_Premultiplied);
        image.setDevicePixelRatio(ratio);
        image.fill(Qt::transparent);
        QPainter svgPainter(&image);
        // Every glyph uses the same square grid, with a small shared optical inset.
        const qreal side = std::min(rect.width(),rect.height()) * 0.9;
        const QRectF bounds((rect.width() - side) / 2,(rect.height() - side) / 2,side,side);
        m_renderer.render(&svgPainter,bounds);
        svgPainter.setCompositionMode(QPainter::CompositionMode_SourceIn);
        QColor color = m_color;
        if (mode == QIcon::Disabled) color.setAlpha(100);
        svgPainter.fillRect(QRect(QPoint(),rect.size()),color);
        svgPainter.end();
        painter->drawImage(rect.topLeft(),image);
    }

private:
    CafeSymbol m_symbol;
    QColor m_color;
    QSvgRenderer m_renderer;
};
}

QIcon CafeIcon(CafeSymbol symbol, const QColor& color)
{
    return QIcon(new CafeIconEngine(symbol,color));
}
