#include "host/AttentionMonitor.hpp"

#include <catch2/catch_test_macros.hpp>

using beamit::host::AttentionTracker;

namespace {
const HWND teams = reinterpret_cast<HWND>(0x1000);
const HWND other = reinterpret_cast<HWND>(0x2000);
} // namespace

TEST_CASE("Flashing sets attention once", "[attention]") {
    AttentionTracker tracker;
    const auto set = tracker.flashed(teams, 42, false);
    REQUIRE(set);
    CHECK(set->requested);
    CHECK(set->pid == 42);
    CHECK(set->window == teams);
    // Repeated flash notifications while the window keeps flashing are not new requests.
    CHECK_FALSE(tracker.flashed(teams, 42, false));
    CHECK(tracker.active().size() == 1);
}

TEST_CASE("Activation and destruction clear attention", "[attention]") {
    AttentionTracker tracker;
    REQUIRE(tracker.flashed(teams, 42, false));
    REQUIRE(tracker.flashed(other, 7, false));
    const auto activated = tracker.activated(teams);
    REQUIRE(activated);
    CHECK_FALSE(activated->requested);
    CHECK(activated->pid == 42);
    CHECK_FALSE(tracker.activated(teams));
    const auto destroyed = tracker.destroyed(other);
    REQUIRE(destroyed);
    CHECK(destroyed->pid == 7);
    CHECK(tracker.active().empty());
    // A new flash after clearing is a new request.
    CHECK(tracker.flashed(teams, 42, false));
}

TEST_CASE("Flashes of the foreground window are ignored", "[attention]") {
    AttentionTracker tracker;
    CHECK_FALSE(tracker.flashed(teams, 42, true));
    CHECK(tracker.active().empty());
}
