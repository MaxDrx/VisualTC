#include "ui/Icons.h"

#include <QFile>
#include <QHash>
#include <QPainter>
#include <QPixmap>
#include <QSvgRenderer>

#include "app/Theme.h"

namespace vtc {

namespace {
QHash<QString, QIcon>& cache() {
    static QHash<QString, QIcon> c;
    return c;
}

QPixmap render(const QByteArray& svg, int size) {
    QSvgRenderer renderer(svg);
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    renderer.render(&p);
    return pm;
}
}  // namespace

QIcon Icons::get(const QString& name) {
    auto& c = cache();
    if (auto it = c.find(name); it != c.end()) {
        return it.value();
    }
    QFile f(":/icons/" + name + ".svg");
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    QByteArray svg = f.readAll();
    const QByteArray normal = QByteArray(svg).replace("#ICON#", Theme::colors().icon.name().toLatin1());
    const QByteArray active = QByteArray(svg).replace("#ICON#", Theme::colors().accent.name().toLatin1());
    const QByteArray disabled =
        QByteArray(svg).replace("#ICON#", Theme::colors().textSecondary.darker(150).name().toLatin1());
    QIcon icon;
    for (int size : {16, 20, 24, 32, 48, 64}) {
        icon.addPixmap(render(normal, size), QIcon::Normal, QIcon::Off);
        icon.addPixmap(render(active, size), QIcon::Normal, QIcon::On);
        icon.addPixmap(render(disabled, size), QIcon::Disabled, QIcon::Off);
    }
    c.insert(name, icon);
    return icon;
}

QIcon Icons::appIcon() {
    QFile f(":/icons/app.svg");
    if (!f.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QByteArray svg = f.readAll();
    QIcon icon;
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        icon.addPixmap(render(svg, size));
    }
    return icon;
}

void Icons::clearCache() { cache().clear(); }

}  // namespace vtc
