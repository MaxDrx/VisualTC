#pragma once

#include <QIcon>
#include <QString>

namespace vtc {

// Original vector icon set (resources/icons). SVGs use a "#ICON#" color
// token that is replaced by the current theme color, so the same files work
// in dark and light themes and stay sharp on High-DPI/Retina screens.
class Icons {
public:
    static QIcon get(const QString& name);
    static QIcon appIcon();
    static void clearCache();
};

}  // namespace vtc
