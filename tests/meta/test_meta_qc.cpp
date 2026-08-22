#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <optional>

#include "ac3/meta/qc.hpp"

// ac3::meta::qc.hpp's own surface: the three named delivery-gate presets
// (roadmap C2) and the pure gate-evaluation math ac3cli qc and
// examples/qc_report.cpp both call. The presets' numeric values are copied
// here as CHECKs against the primary sources cited in qc.hpp's own
// qc_preset() comment (EBU R 128 s2 + EBU R 128, ATSC A/85:2013 §6, Netflix
// Sound Mix Specifications & Best Practices v1.6) - a wrong number here is
// caught the same way a wrong DRC profile edge would be in test_drc.cpp.

using ac3::meta::evaluate_qc_gate;
using ac3::meta::parse_qc_preset;
using ac3::meta::qc_preset;
using ac3::meta::qc_preset_name;
using ac3::meta::QcPresetId;
using Catch::Approx;

TEST_CASE("qc preset numbers match their cited primary sources", "[qc]") {
    SECTION("EBU R 128 s2 (Nov 2023 v3) + EBU R 128 (Nov 2023 v5) recommendations (h)/(m)") {
        const auto p = qc_preset(QcPresetId::kEbuR128S2);
        CHECK(p.target_lkfs == Approx(-23.0));
        CHECK(p.tolerance_lu == Approx(1.0));
        CHECK(p.max_true_peak_dbtp == Approx(-1.0));
        CHECK(qc_preset_name(QcPresetId::kEbuR128S2) == "ebu-r128-s2");
    }
    SECTION("ATSC A/85:2013 (Corrigendum No. 1) Sec.6") {
        const auto p = qc_preset(QcPresetId::kAtscA85);
        CHECK(p.target_lkfs == Approx(-24.0));
        CHECK(p.tolerance_lu == Approx(2.0));
        CHECK(p.max_true_peak_dbtp == Approx(-2.0));
        CHECK(qc_preset_name(QcPresetId::kAtscA85) == "atsc-a85");
    }
    SECTION("Netflix Sound Mix Specifications & Best Practices v1.6") {
        const auto p = qc_preset(QcPresetId::kNetflix);
        CHECK(p.target_lkfs == Approx(-27.0));
        CHECK(p.tolerance_lu == Approx(2.0));
        CHECK(p.max_true_peak_dbtp == Approx(-2.0));
        CHECK(qc_preset_name(QcPresetId::kNetflix) == "netflix");
    }
}

TEST_CASE("parse_qc_preset round-trips every name qc_preset_name emits, and rejects garbage",
          "[qc]") {
    for (const auto id : ac3::meta::kQcPresetIds) {
        QcPresetId parsed{};
        REQUIRE(parse_qc_preset(qc_preset_name(id), parsed));
        CHECK(parsed == id);
    }
    QcPresetId unused{};
    CHECK_FALSE(parse_qc_preset("", unused));
    CHECK_FALSE(parse_qc_preset("ebu-r128", unused));       // near miss, not exact
    CHECK_FALSE(parse_qc_preset("ATSC-A85", unused));       // case-sensitive
    CHECK_FALSE(parse_qc_preset("all", unused));            // "all" is the CLI's own keyword,
                                                              // never a real preset name
}

TEST_CASE("evaluate_qc_gate: loudness gates on |measured - target| <= tolerance, inclusive",
          "[qc]") {
    const ac3::meta::QcPreset preset{
        .target_lkfs = -23.0, .tolerance_lu = 1.0, .max_true_peak_dbtp = -1.0};

    SECTION("exactly on target passes") {
        const auto v = evaluate_qc_gate(preset, -23.0, std::nullopt);
        REQUIRE(v.loudness_delta_lu.has_value());
        CHECK(*v.loudness_delta_lu == Approx(0.0));
        CHECK(v.loudness_pass);
    }
    SECTION("exactly at the tolerance boundary (both directions) passes") {
        CHECK(evaluate_qc_gate(preset, -22.0, std::nullopt).loudness_pass);  // +1.0 LU
        CHECK(evaluate_qc_gate(preset, -24.0, std::nullopt).loudness_pass);  // -1.0 LU
    }
    SECTION("just past the tolerance boundary fails") {
        CHECK_FALSE(evaluate_qc_gate(preset, -21.9, std::nullopt).loudness_pass);
        CHECK_FALSE(evaluate_qc_gate(preset, -24.1, std::nullopt).loudness_pass);
    }
    SECTION("no measurement (nullopt) never passes") {
        const auto v = evaluate_qc_gate(preset, std::nullopt, std::nullopt);
        CHECK_FALSE(v.loudness_delta_lu.has_value());
        CHECK_FALSE(v.loudness_pass);
    }
    SECTION("delta is signed (measured minus target), not just its magnitude") {
        // |delta| alone cannot tell measured-too-loud from measured-too-quiet
        // apart, and loudness_pass is symmetric in the sign either way - this
        // is the assertion that actually pins down the sign, which a
        // target-minus-measured bug (same |delta|, same pass/fail verdict)
        // would otherwise sail straight through undetected.
        CHECK(*evaluate_qc_gate(preset, -20.0, std::nullopt).loudness_delta_lu ==
             Approx(3.0));   // 3 LU louder than target, not -3.0
        CHECK(*evaluate_qc_gate(preset, -26.0, std::nullopt).loudness_delta_lu ==
             Approx(-3.0));  // 3 LU quieter than target, not +3.0
    }
}

TEST_CASE("evaluate_qc_gate: true peak gates on a one-sided ceiling, not a band", "[qc]") {
    const ac3::meta::QcPreset preset{
        .target_lkfs = -24.0, .tolerance_lu = 2.0, .max_true_peak_dbtp = -2.0};

    SECTION("well under the ceiling passes") {
        const auto v = evaluate_qc_gate(preset, std::nullopt, -6.0);
        REQUIRE(v.true_peak_margin_dbtp.has_value());
        CHECK(*v.true_peak_margin_dbtp == Approx(4.0));
        CHECK(v.true_peak_pass);
    }
    SECTION("exactly at the ceiling passes") {
        CHECK(evaluate_qc_gate(preset, std::nullopt, -2.0).true_peak_pass);
    }
    SECTION("over the ceiling fails, however slightly") {
        const auto v = evaluate_qc_gate(preset, std::nullopt, -1.9);
        REQUIRE(v.true_peak_margin_dbtp.has_value());
        CHECK(*v.true_peak_margin_dbtp == Approx(-0.1).margin(1e-9));
        CHECK_FALSE(v.true_peak_pass);
    }
    SECTION("no measurement (nullopt) never passes") {
        CHECK_FALSE(evaluate_qc_gate(preset, std::nullopt, std::nullopt).true_peak_pass);
    }
}

TEST_CASE("QcVerdict::pass() is both halves together, not either alone", "[qc]") {
    const ac3::meta::QcPreset preset{
        .target_lkfs = -23.0, .tolerance_lu = 1.0, .max_true_peak_dbtp = -1.0};

    CHECK(evaluate_qc_gate(preset, -23.0, -1.0).pass());        // both pass
    CHECK_FALSE(evaluate_qc_gate(preset, -30.0, -1.0).pass());  // loudness fails alone
    CHECK_FALSE(evaluate_qc_gate(preset, -23.0, 0.0).pass());   // peak fails alone
    CHECK_FALSE(evaluate_qc_gate(preset, -30.0, 0.0).pass());   // both fail
}
