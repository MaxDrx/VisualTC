#pragma once

#include <QString>
#include <string>

class QCoreApplication;

namespace vtc {

struct Series;

// Interface language. The texts in the source code are Brazilian
// Portuguese; Spanish and English come from resources/i18n/<key>.json
// (source text -> translation), loaded by our own QTranslator, so building
// needs no Qt Linguist tools.
enum class Language { Portuguese = 0, Spanish, English };
inline constexpr Language kLanguages[] = {Language::Portuguese, Language::Spanish, Language::English};

QString languageKey(Language l);         // "pt", "es", "en" (settings)
Language languageFromKey(const QString& key);
QString languageNativeName(Language l);  // "Português (Brasil)", "Español", "English"

// Language for this run: the user's choice; on the first run, the system
// language (Portuguese for anyone who already used an earlier version).
Language startupLanguage();
// Installs the translation of the interface and of Qt's standard dialogs,
// and the number format of that language. Call once, before any window.
void installLanguage(QCoreApplication& app, Language l);
Language currentLanguage();

// Messages produced by the core library (always Portuguese, sometimes with
// variable parts such as a file name) in the interface language.
QString trCore(const std::string& message);
QString trCore(const QString& message);

// Series description with the parts VisualTC adds when it splits a series
// ("Eco 2", "Clipe 3", "Outras") in the interface language.
QString seriesDescription(const Series& series);

// Number of entries of the loaded translation (0 for Portuguese); tests.
int translationSize();

}  // namespace vtc
