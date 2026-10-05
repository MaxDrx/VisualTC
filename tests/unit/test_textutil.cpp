#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <clocale>
#include <string>

#include "dicom/TextUtil.h"

using namespace vtc;

TEST_CASE("DS/IS parsing is strict and robust", "[text]") {
    REQUIRE(parseDouble(" 0.488281 ").value() == Catch::Approx(0.488281));
    REQUIRE(parseDouble("-1024").value() == -1024.0);
    REQUIRE(parseDouble("1e-3").value() == Catch::Approx(0.001));
    REQUIRE_FALSE(parseDouble("").has_value());
    REQUIRE_FALSE(parseDouble("abc").has_value());
    REQUIRE_FALSE(parseDouble("1.5x").has_value());
    REQUIRE_FALSE(parseDouble("nan").has_value());
    REQUIRE_FALSE(parseDouble("inf").has_value());
    REQUIRE(parseInt("12").value() == 12);
    REQUIRE(parseInt("+7").value() == 7);
    REQUIRE(parseInt("12.0").value() == 12);
    REQUIRE_FALSE(parseInt("12.5").has_value());
}

// Qt calls setlocale(LC_ALL, "") at start-up: on a Brazilian (or German...)
// desktop the C library then expects a decimal COMMA. DICOM DS values always
// use a dot, so parsing must not depend on the process locale.
TEST_CASE("DS parsing does not depend on the process locale", "[text][locale]") {
    const char* previous = std::setlocale(LC_NUMERIC, nullptr);
    const std::string saved = previous != nullptr ? previous : "C";
    const char* candidates[] = {"pt_BR.UTF-8", "pt_BR.utf8", "pt-BR", "de_DE.UTF-8", "de_DE.utf8", "de-DE", "fr_FR.UTF-8"};
    const char* active = nullptr;
    for (const char* c : candidates) {
        if (std::setlocale(LC_NUMERIC, c) != nullptr) {
            active = c;
            break;
        }
    }
    if (active == nullptr) {
        SKIP("Nenhuma localidade com vírgula decimal instalada neste sistema");
    }
    CAPTURE(active);
    const auto spacing = parseDoubles("0.488281\\0.488281");
    const auto intercept = parseDouble("-1024.0");
    const auto exponent = parseDouble("1.5e-3");
    const auto plus = parseDouble("+2.25");
    std::setlocale(LC_NUMERIC, saved.c_str());
    REQUIRE(spacing.size() == 2);
    REQUIRE(spacing[0] == Catch::Approx(0.488281));
    REQUIRE(intercept.value() == -1024.0);
    REQUIRE(exponent.value() == Catch::Approx(0.0015));
    REQUIRE(plus.value() == Catch::Approx(2.25));
}

TEST_CASE("Multi-valued strings are all-or-nothing", "[text]") {
    const auto v = parseDoubles("0.5\\0.75");
    REQUIRE(v.size() == 2);
    REQUIRE(v[1] == Catch::Approx(0.75));
    REQUIRE(parseDoubles("1\\x\\3").empty());
    REQUIRE(splitBackslash("A\\B\\").size() == 3);
}

TEST_CASE("Person names are formatted for display", "[text]") {
    REQUIRE(formatPersonName("SILVA^JOAO^^DR") == "SILVA JOAO DR");
    REQUIRE(formatPersonName("SOUZA^MARIA=ideographic") == "SOUZA MARIA");
    REQUIRE(formatPersonName("") == "");
}

TEST_CASE("Character sets convert to UTF-8", "[text]") {
    const std::string latin1 = "JO\xC3O";  // JOÃO in ISO 8859-1
    REQUIRE(dicomToUtf8(latin1, "ISO_IR 100") == "JO\xC3\x83O");
    REQUIRE(isValidUtf8(dicomToUtf8(latin1, "")));
    const std::string utf8 = "JO\xC3\x83O";
    REQUIRE(dicomToUtf8(utf8, "ISO_IR 192") == utf8);
    REQUIRE(dicomToUtf8("PLAIN", "") == "PLAIN");
}

TEST_CASE("Dates and times are formatted", "[text]") {
    REQUIRE(formatDicomDate("20240131") == "31/01/2024");
    REQUIRE(formatDicomDate("2024") == "2024");
    REQUIRE(formatDicomTime("143015.123") == "14:30:15");
}
