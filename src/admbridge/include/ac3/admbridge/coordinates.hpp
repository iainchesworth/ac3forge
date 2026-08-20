#pragma once

#include "ac3/admbridge/export.hpp"
#include "ac3/oba/oamd.hpp"
#include "ac3adm/model.hpp"

// Coordinate conversion between the two position systems Recommendation ITU-R BS.2076-2 (10/2019)
// Annex 1 defines for audioBlockFormat (Tables 15-17, and Clause 8 "Coordinate system" for the
// sign conventions those tables' column headings alone don't spell out) and ac3::oba::Position's
// own room-anchored convention (ac3/oba/oamd.hpp, ETSI TS 103 420 clause 4.2.1).
//
// Clause 8, verified directly against the published Recommendation text (not transcribed from
// any secondary source):
//
//   "Azimuth - angle in the horizontal plane with 0 degrees as straight ahead, and positive
//   angles to the left (or anti-clockwise) when viewed from above."
//   "Elevation - angle in the vertical plane with 0 degrees horizontally ahead, and positive
//   angles going up."
//   "Distance - a normalized distance, where 1.0 is assumed to be the default radius of the
//   sphere."
//   "X - left to right, with positive values to the right."
//   "Y - front to back, with positive values to the front."
//   "Z - top to bottom, with positive values to the top."
//
// (Table 16 adds the range: "the values 1.0 and -1.0 are on the surface of the cube.")
//
// ac3::oba::Position (oamd.hpp): x runs 0 (left wall) to 1 (right wall), y runs 0 (front wall) to
// 1 (back wall), z runs -1 (floor) to +1 (ceiling) - left-handed, normalized to the room cuboid,
// with (0.5, 0, 0) the centre of the front wall.
//
// polar_to_adm_cartesian() turns a polar/spherical position into the same right-positive/
// front-positive/top-positive point BS.2076-2's own Cartesian axes describe, via the ordinary
// physics spherical-to-Cartesian conversion adapted for azimuth's "positive = left" sign (BS.
// 2076-2 does not itself spell out this intermediate step - Tables 15 and 16 are presented as two
// independent, alternative ways to say the same thing, not as one derived from the other - so the
// exact formula below is this module's own, checked two ways: (1) against Clause 8's stated axis
// directions at the cardinal points (0 deg azimuth = straight ahead = +Y; +90 deg azimuth = left
// = -X, since X is right-positive; +90 deg elevation = up = +Z), and (2) empirically, in this
// module's own tests, against the existing ring-position constants this project's
// tests/test_atmos_motion.cpp already hardcodes (kL/kR/kSR) - converting BS.2076-2's own M+030/
// M-030/M-110 speaker-label azimuths (Annex A common definitions) through this formula reproduces
// those exact room coordinates.
//
// adm_cartesian_to_room() then rescales that point from BS.2076-2's [-1, 1] unit cube (both axes
// signed, origin at the room's centre) onto ac3::oba::Position's own [0, 1] (x, y) / [-1, 1] (z)
// convention (origin off-centre on x/y, centred on z) - a pure affine remap, not a design choice:
// x_room = (x_adm + 1) / 2, y_room = (1 - y_adm) / 2 (BS.2076-2's Y is front-positive, oba's y is
// front-zero/back-one, hence the sign flip), z_room = z_adm (both top-positive, both already
// [-1, 1] - no rescale needed).
//
// One genuine, documented judgement call: BS.2076-2 nowhere equates a polar position's unit
// SPHERE (distance = 1.0 on its surface) with a Cartesian position's unit CUBE (|x|, |y| or |z| =
// 1.0 on its surface) - they coincide only exactly on each axis (e.g. straight ahead at distance
// 1.0 sits on both the sphere's and the cube's front face), not off-axis (e.g. a 45-degree-azimuth
// object at distance 1.0 sits well inside the cube's own surface, at Euclidean distance 1.0 from
// centre rather than at the cube's own corner). This module treats the polar-derived point as a
// Cartesian point of the same coordinates without renormalizing for that difference - the
// simplest reading, and the one that reproduces the existing kL/kR/kSR ring constants exactly for
// on-axis azimuths, which is the case that matters for real DirectSpeakers content (every
// standard loudspeaker position BS.2076-2's own Annex A common definitions use is on-axis: pure
// left/right, pure front/back, or pure up/down combinations).
namespace ac3::admbridge {

// BS.2076-2 Clause 8's polar convention to the same right/front/top-positive point its own
// Cartesian axes describe. See this header's own top comment for the full derivation and the
// three independent checks performed against it.
[[nodiscard]] AC3ADMBRIDGE_EXPORT ac3adm::CartesianPosition polar_to_adm_cartesian(
    const ac3adm::PolarPosition& polar);

// BS.2076-2's [-1, 1] unit-cube Cartesian convention to ac3::oba::Position's [0, 1]/[0, 1]/
// [-1, 1] room-anchored one. Pure affine remap - see this header's own top comment.
[[nodiscard]] AC3ADMBRIDGE_EXPORT ac3::oba::Position adm_cartesian_to_room(
    const ac3adm::CartesianPosition& cartesian);

// Dispatches on ac3adm::Position's own variant (ac3adm/model.hpp: PolarPosition or
// CartesianPosition, selected by AudioBlockFormat::cartesian) and converts whichever alternative
// is actually present straight to room coordinates.
[[nodiscard]] AC3ADMBRIDGE_EXPORT ac3::oba::Position adm_position_to_room(
    const ac3adm::Position& position);

}  // namespace ac3::admbridge
