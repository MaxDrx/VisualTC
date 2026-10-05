#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

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
