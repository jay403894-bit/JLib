#pragma once

// Named physics tolerances -- every value here used to be a bare literal scattered
// through PhysicsSystem.cpp. World units in this engine ARE pixels (tiles 32-64,
// gravity ~900 px/s^2), so these are pixel quantities; do not "correct" them against
// advice assuming 1 unit = 16px.
//
// Two distinct species live here -- know which one you're touching:
//
//  KINEMATIC TOLERANCES scale with how far things move per fixed substep, or with
//  render geometry. They are gameplay-feel values with physical derivations. Because
//  the engine runs a FIXED accumulator step (the game's PHYSICS_STEP, 1/120s), a
//  "max speed x step" derivation is deterministic -- NOT framerate-dependent, which
//  it would be under a variable dt.
//
//  SENSOR GEOMETRY defines the probe shapes the tile sensors cast. Shared by every
//  directional sensor; change one constant, every sensor agrees.
namespace PlatformerPhysics2D {
namespace Tuning {

    // ---- kinematic tolerances ----

    // Max distance the feet may be from a slope's surface and still snap/climb onto
    // it (CheckSlopeSnap/HandleSlopeSnap/IsStandingOnSlope). MUST exceed the farthest
    // a body can fall in ONE fixed substep, or fast falls skip the snap window and
    // wall against the slope instead: at the current extremes that's
    // maxFallSpeed(~900) * PHYSICS_STEP(1/120) = 7.5px -> 8. If either of those
    // numbers grows, recheck this one.
    constexpr float kSlopeSnapReach = 8.0f;

    // Grounding tolerance (CheckGrounding): feet within half a pixel of the floor
    // count as ON it. Sub-pixel gaps are invisible at render time; a meaningfully
    // larger value makes isGrounded flicker-proof but lets the body visibly hover.
    constexpr float kGroundingEpsilon = 0.5f;

    // Landing tolerance onto a slope surface (ResolveParticle's slope branch) --
    // same half-pixel reasoning as kGroundingEpsilon, kept separate so slope feel
    // can be tuned without touching flat-ground grounding.
    constexpr float kSlopeLandingEpsilon = 0.5f;

    // ---- sensor probe geometry ----

    // How far beyond the body's AABB edge a directional sensor probe sits. Big
    // enough to reliably reach the neighboring tile across float slop; small enough
    // not to sense tiles a full pixel-gap away.
    constexpr float kSensorProbeOffset = 2.0f;

    // Half-width of the thin directional probes (a ~2px-wide strip).
    constexpr float kSensorProbeHalfWidth = 1.0f;

    // Sensor half-height (or half-width, for vertical probes) as a fraction of the
    // body's own half-extent -- shrunk so a probe hugging one face never clips the
    // tile diagonally adjacent at a corner and reports a phantom wall/floor.
    constexpr float kSensorShrink = 0.8f;

} // namespace Tuning
} // namespace PlatformerPhysics2D
