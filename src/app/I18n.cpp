#include "app/I18n.h"

#include <QApplication>
#include <QFile>
#include <QHash>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLibraryInfo>
#include <QLocale>
#include <QRegularExpression>
#include <QSettings>
#include <QTranslator>
#include <algorithm>
#include <memory>
#include <vector>

#include "app/AppSettings.h"
#include "dicom/DicomSeries.h"

namespace vtc {

namespace {

// Placeholders %1..%9 inside a source text.
const QRegularExpression& placeholderRe() {
    static const QRegularExpression re(QStringLiteral("%(\\d)"));
    return re;
}

class TableTranslator : public QTranslator {
public:
    bool loadTable(const QString& resource) {
        QFile f(resource);
        if (!f.open(QIODevice::ReadOnly)) {
            return false;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (!doc.isObject()) {
            return false;
        }
        const QJsonObject obj = doc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            const QString translation = it.value().toString();
            if (it.key().startsWith(QLatin1Char('@')) || translation.isEmpty()) {
                continue;  // metadata / not translated yet
            }
            exact_.insert(it.key(), translation);
            if (it.key().contains(placeholderRe())) {
                addPattern(it.key(), translation);
            }
        }
        // Most specific first: a pattern with more fixed text wins.
        std::stable_sort(patterns_.begin(), patterns_.end(),
                         [](const Pattern& a, const Pattern& b) { return a.fixedLength > b.fixedLength; });
        return !exact_.isEmpty();
    }

    QString translate(const char* /*context*/, const char* sourceText, const char* /*disambiguation*/,
                      int n) const override {
        const auto it = exact_.constFind(QString::fromUtf8(sourceText));
        if (it == exact_.constEnd()) {
            return {};
        }
        return pickPlural(*it, n);
    }

    [[nodiscard]] bool isEmpty() const override { return exact_.isEmpty(); }

    [[nodiscard]] QString message(const QString& text) const {
        if (const auto it = exact_.constFind(text); it != exact_.constEnd()) {
            return *it;
        }
        for (const auto& p : patterns_) {
            const auto m = p.re.match(text);
            if (!m.hasMatch()) {
                continue;
            }
            // Variable parts that are known texts are translated as well
            // ("Arquivo DICOM corrompido (sequência corrompida)."). A part is
            // always shorter than the whole message: the recursion ends.
            QStringList parts;
            for (int i = 0; i < static_cast<int>(p.order.size()); ++i) {
                parts << message(m.captured(i + 1));
            }
            QString out;
            const QString& t = p.translation;
            for (qsizetype i = 0; i < t.size(); ++i) {
                if (t[i] == QLatin1Char('%') && i + 1 < t.size() && t[i + 1].isDigit()) {
                    const int number = t[i + 1].digitValue();
                    const auto it = std::find(p.order.begin(), p.order.end(), number);
                    if (it != p.order.end()) {
                        out += parts[static_cast<int>(it - p.order.begin())];
                        ++i;
                        continue;
                    }
                }
                out += t[i];
            }
            return out;
        }
        return text;
    }

    [[nodiscard]] int size() const { return static_cast<int>(exact_.size()); }

private:
    struct Pattern {
        QRegularExpression re;
        QString translation;
        std::vector<int> order;  // placeholder number of each capture group
        int fixedLength = 0;
    };

    void addPattern(const QString& key, const QString& translation) {
        Pattern p;
        p.translation = translation;
        QString re = QStringLiteral("^");
        qsizetype last = 0;
        auto it = placeholderRe().globalMatch(key);
        while (it.hasNext()) {
            const auto m = it.next();
            const QString fixed = key.mid(last, m.capturedStart() - last);
            re += QRegularExpression::escape(fixed) + QStringLiteral("(.+?)");
            p.fixedLength += static_cast<int>(fixed.size());
            p.order.push_back(m.captured(1).toInt());
            last = m.capturedEnd();
        }
        const QString tail = key.mid(last);
        re += QRegularExpression::escape(tail) + QStringLiteral("$");
        p.fixedLength += static_cast<int>(tail.size());
        p.re = QRegularExpression(re, QRegularExpression::DotMatchesEverythingOption);
        patterns_.push_back(std::move(p));
    }

    // "singular||plural" for texts with %n.
    static QString pickPlural(const QString& t, int n) {
        const qsizetype bar = t.indexOf(QStringLiteral("||"));
        if (bar < 0) {
            return t;
        }
        return (n == 1) ? t.left(bar) : t.mid(bar + 2);
    }

    QHash<QString, QString> exact_;
    std::vector<Pattern> patterns_;
};

Language g_language = Language::Portuguese;
TableTranslator* g_table = nullptr;  // owned by the application (QObject parent)

}  // namespace

QString languageKey(Language l) {
    switch (l) {
        case Language::Portuguese: return QStringLiteral("pt");
        case Language::Spanish: return QStringLiteral("es");
        case Language::English: return QStringLiteral("en");
    }
    return QStringLiteral("pt");
}

Language languageFromKey(const QString& key) {
    for (Language l : kLanguages) {
        if (languageKey(l) == key) {
            return l;
        }
    }
    return Language::Portuguese;
}

QString languageNativeName(Language l) {
    // Each language in its own name: whoever cannot read the current one
    // still finds theirs.
    switch (l) {
        case Language::Portuguese: return QStringLiteral("Português (Brasil)");
        case Language::Spanish: return QStringLiteral("Español");
        case Language::English: return QStringLiteral("English");
    }
    return {};
}

Language startupLanguage() {
    auto& s = AppSettings::instance();
    const QString saved = s.language();
    if (!saved.isEmpty()) {
        return languageFromKey(saved);
    }
    Language l = Language::Portuguese;
    // Someone updating from a version without this option keeps Portuguese.
    if (!s.hasPreviousUse()) {
        switch (QLocale::system().language()) {
            case QLocale::Portuguese: l = Language::Portuguese; break;
            case QLocale::Spanish: l = Language::Spanish; break;
            default: l = Language::English; break;
        }
    }
    s.setLanguage(languageKey(l));
    return l;
}

void installLanguage(QCoreApplication& app, Language l) {
    g_language = l;
    switch (l) {
        case Language::Portuguese: QLocale::setDefault(QLocale(QLocale::Portuguese, QLocale::Brazil)); break;
        case Language::Spanish: QLocale::setDefault(QLocale(QLocale::Spanish, QLocale::Spain)); break;
        case Language::English: QLocale::setDefault(QLocale(QLocale::English, QLocale::UnitedStates)); break;
    }
    // Qt's own texts (standard buttons, file dialogs) when deployed.
    if (l != Language::English) {
        auto* qt = new QTranslator(&app);
        const QLocale loc;
        if (qt->load(loc, QStringLiteral("qtbase"), QStringLiteral("_"), QStringLiteral(":/qt-translations")) ||
            qt->load(loc, QStringLiteral("qtbase"), QStringLiteral("_"),
                     QLibraryInfo::path(QLibraryInfo::TranslationsPath)) ||
            qt->load(loc, QStringLiteral("qtbase"), QStringLiteral("_"),
                     QCoreApplication::applicationDirPath() + QStringLiteral("/translations"))) {
            QCoreApplication::installTranslator(qt);
        } else {
            delete qt;
        }
    }
    if (l == Language::Portuguese) {
        return;  // the source language
    }
    auto* table = new TableTranslator;
    table->setParent(&app);
    if (table->loadTable(QStringLiteral(":/i18n/%1.json").arg(languageKey(l)))) {
        QCoreApplication::installTranslator(table);
        g_table = table;
    } else {
        delete table;
    }
}

Language currentLanguage() { return g_language; }

QString trCore(const QString& message) { return g_table != nullptr ? g_table->message(message) : message; }

QString trCore(const std::string& message) { return trCore(QString::fromStdString(message)); }

QString seriesDescription(const Series& series) {
    if (series.frames.empty()) {
        return {};
    }
    std::string base = series.firstInstance().seriesDescription;
    if (base.empty()) {
        base = series.firstInstance().protocolName;
    }
    QString out = QString::fromStdString(base);
    if (!series.subLabel.empty()) {
        QStringList parts = QString::fromStdString(series.subLabel).split(QStringLiteral(" · "));
        for (auto& part : parts) {
            part = trCore(part);
        }
        const QString sub = parts.join(QStringLiteral(" · "));
        out = out.isEmpty() ? sub : out + QStringLiteral(" · ") + sub;
    }
    return out;
}

int translationSize() { return g_table != nullptr ? g_table->size() : 0; }

}  // namespace vtc
