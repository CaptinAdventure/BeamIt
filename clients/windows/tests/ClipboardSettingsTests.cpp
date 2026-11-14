#include "../app/AppUtil.hpp"

#include <catch2/catch_test_macros.hpp>
#include <vector>

using beamit::client::Settings;

TEST_CASE("Windows clipboard settings validate policy without changing user configuration",
          "[clipboard-settings]") {
    std::vector<std::string> diagnostics;
    const auto parse = [&](std::string_view input) {
        diagnostics.clear();
        return Settings::parse(input, [&](std::string_view message) { diagnostics.emplace_back(message); });
    };
    auto settings = parse("{}");
    CHECK(settings.clipboard_bulk_byte_threshold == 500'000'000);
    CHECK(settings.clipboard_bulk_entry_threshold == 5);
    CHECK_FALSE(settings.clipboard_sync);
    CHECK(diagnostics.empty());
    settings = parse(R"({"clipboard_sync":true,"clipboard_bulk_byte_threshold":0,"clipboard_bulk_entry_threshold":0})");
    CHECK(settings.clipboard_sync);
    CHECK(settings.clipboard_bulk_byte_threshold == 0);
    CHECK(settings.clipboard_bulk_entry_threshold == 0);
    CHECK(diagnostics.empty());
    settings = parse(R"({"clipboard_bulk_byte_threshold":2000000000,"clipboard_bulk_entry_threshold":64})");
    CHECK(settings.clipboard_bulk_byte_threshold == 2'000'000'000);
    CHECK(settings.clipboard_bulk_entry_threshold == 64);
    CHECK(diagnostics.empty());
    for (const auto& value : {"-1", "2000000001", "true", R"("123")", "0.5", "null", "18446744073709551615"}) {
        settings = parse(std::string(R"({"clipboard_bulk_byte_threshold":)") + value + "}");
        CHECK(settings.clipboard_bulk_byte_threshold == 500'000'000);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0] == "BeamIt: invalid clipboard_bulk_byte_threshold; using the documented default.");
    }
    for (const auto& value : {"-1", "65", "false", R"("5")", "1.5", "null"}) {
        settings = parse(std::string(R"({"clipboard_bulk_entry_threshold":)") + value + "}");
        CHECK(settings.clipboard_bulk_entry_threshold == 5);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0] == "BeamIt: invalid clipboard_bulk_entry_threshold; using the documented default.");
    }
    for (const auto& input : {"[", "[]", "false"}) {
        settings = parse(input);
        CHECK(settings.clipboard_bulk_byte_threshold == 500'000'000);
        CHECK(settings.clipboard_bulk_entry_threshold == 5);
        REQUIRE(diagnostics.size() == 1);
        CHECK(diagnostics[0] == "BeamIt: invalid client settings; using documented defaults, including clipboard thresholds.");
    }
}
