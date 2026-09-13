#pragma once

#include <QColor>
#include <QIcon>

enum class CafeSymbol { Home, GamePad, Wifi, Settings, Info, Plus, Play, Stop, Refresh, Copy, Folder, Save, Edit, Remove, Quit, Computer };

// Bundled CC0 Kenney SVGs, rendered and tinted without a desktop icon theme.
QIcon CafeIcon(CafeSymbol symbol, const QColor& color = QColor("#70513a"));
