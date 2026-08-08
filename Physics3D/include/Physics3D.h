// SPDX-License-Identifier: BSD-3-Clause
// Copyright (c) 2026 Joshua Makler. Part of JLib -- see LICENSE at the repository root.

#pragma once
#include <DirectXMath.h>
#include <memory>
#include <cstddef>
#include <cstdint>
#include <vector>

// JLib::Physics3D -- the Jolt-backed 3D physics library (extracted from 3DTest 2026-08-01; deploys to
// C:\libs\Physics3D via deploy_lib.bat). **Consumers link Physics3D.lib ALONE** -- Jolt is MERGED into it
// by deploy_lib.bat, so no Jolt.lib and, crucially, no JPH_* define set to get wrong: a consumer that never
// compiles against Jolt headers cannot mismatch its configuration, which kills the silent-ABI-break class
// of bug (LNK2001 GetSubmergedVolume-style vtable errors) outright.
// Deliberately a PIMPL with a plain-types interface (XMFLOAT3/XMFLOAT4 only) so NO Jolt header
// leaks to consumers -- that keeps Jolt's <Jolt/Physics/PhysicsSystem.h> from colliding with the 2D
// <PhysicsSystem.h> a scene may also include, and keeps Jolt's heavy headers out of app compiles. Jolt's
// jobs run on JLib::Scheduler (SchedulerJobSystem in the .cpp). See the roadmap notes for what's wrapped
// vs. still to wrap (CharacterVirtual, MeshShape, constraints, ...).
namespace JLib {
    class Physics3D {
    public:
        Physics3D();
        ~Physics3D();
        Physics3D(const Physics3D&) = delete;
        Physics3D& operator=(const Physics3D&) = delete;

        // --- Process-wide Jolt lifecycle ------------------------------------------------------------------
        // Jolt's allocator, Factory and type registry are PROCESS-wide, not per-world: they must be set up
        // once before the first Physics3D exists and torn down once after the last one dies. That lifetime
        // belongs to the host, not to whichever scene happened to construct a Physics3D first, so it is
        // explicit -- call InitGlobals() next to TaskScheduler::Init() and ShutdownGlobals() in the same
        // teardown block that already does core.Cleanup() / input->Shutdown().
        // Both are idempotent and must be called from ONE thread (the host's startup/shutdown path).
        // ShutdownGlobals() REFUSES to run while any Physics3D instance is still alive -- see the .cpp.
        static void InitGlobals();
        static void ShutdownGlobals();
        static std::size_t LiveInstanceCount();   // diagnostics: instances constructed but not yet destroyed

        void Init();           // builds THIS world (PhysicsSystem + collision layers). Requires InitGlobals().
                               // Creates NO bodies -- the scene authors its world via the Add* verbs below.

        // --- World authoring -----------------------------------------------------------------------------
        // The SCENE builds its own content (a different scene = a different world) by calling these; Physics3D
        // just simulates. Each returns an opaque BodyHandle to store and later query with GetBody. Plain types
        // only (XMFLOAT3/float) so no Jolt type leaks into this header. Call Finalize() ONCE after adding bodies.
        using BodyHandle = std::size_t;
        BodyHandle AddStaticBox     (DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 halfExtents); // immovable (floor/walls)
        // massKg 0 = derive from volume at Jolt's default density (1000 kg/m^3 -- WATER). That default
        // is wildly heavy for anything hollow: a 1.2 x 2.0 x 0.16m door comes out at ~384kg, which no
        // character can push and which needs thousands of N.m to drive with a motor. Real doors are
        // ~30kg, crates ~15. Override it whenever a prop is meant to be moved by gameplay.
        BodyHandle AddDynamicBox    (DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 halfExtents,
                                     float massKg = 0.0f);                                    // falls + collides
        // massKg 0 = Jolt's default (volume x density ~1000kg/m^3). Override for gameplay feel -- e.g. the
        // slingshot projectile must out-mass a tower block (~512kg for a 0.8 cube) or it plinks off.
        // ccd = continuous collision detection (Jolt LinearCast motion quality): set for FAST bodies
        // (projectiles) so they can't tunnel through thin geometry between physics steps.
        BodyHandle AddDynamicSphere (DirectX::XMFLOAT3 center, float radius, float massKg = 0.0f, bool ccd = false);
        BodyHandle AddDynamicCapsule (DirectX::XMFLOAT3 center, float halfHeight, float radius); // halfHeight = cylinder half
        BodyHandle AddDynamicCylinder(DirectX::XMFLOAT3 center, float halfHeight, float radius); // Jolt CylinderShape primitive
        BodyHandle AddDynamicPyramid (DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 halfExtents);  // convex hull (no pyramid primitive)
        BodyHandle AddDynamicCone    (DirectX::XMFLOAT3 center, float radius, float halfHeight); // convex hull (no cone primitive)
        // SENSOR: a static trigger volume -- fires contact events when a dynamic body overlaps it but has
        // NO collision response (things pass through). Goal zones, pickups, kill planes. Identify its
        // events via ContactEvent::isSensor + the user data you set on it (SetUserData below).
        // Static triangle-mesh collision -- level geometry. This is the 3D analogue of the 2D
        // tilemap: arbitrary concave world shape, as opposed to the convex primitives above.
        //
        // STATIC ONLY, and that's a Jolt rule rather than a wrapper limitation: a triangle mesh is a
        // surface, not a solid, so it has no volume, no centre of mass, and no inertia tensor -- all
        // things a dynamic body needs. Moving level geometry has to be a convex shape or a compound
        // of them.
        //
        // Takes RAW ARRAYS rather than a renderer Mesh so this library never depends on the renderer.
        // Note the loaded render mesh usually can't supply them: once uploaded, vertex data lives on
        // the GPU only. Use JLib::LoadGlbGeometry (a CPU-side parse) to get positions and indices,
        // and prefer a separate low-poly collision mesh over the render mesh where you have one --
        // collision cost scales with triangle count and visual detail rarely helps it.
        //
        // `scale` is applied uniformly at build time (Jolt bakes it into the shape); `position`
        // offsets the body. Returns kInvalidBody if the mesh is empty or Jolt rejects it.
        BodyHandle AddStaticMesh(const DirectX::XMFLOAT3* vertices, std::size_t vertexCount,
                                 const uint32_t* indices, std::size_t indexCount,
                                 DirectX::XMFLOAT3 position = { 0.0f, 0.0f, 0.0f },
                                 float scale = 1.0f);

        BodyHandle AddSensorBox      (DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 halfExtents);
        // KINEMATIC: moved by code, immovable by physics (infinite mass from the sim's perspective) --
        // moving platforms, doors. Drive it with MoveKinematic each frame; dynamic bodies get pushed.
        BodyHandle AddKinematicBox   (DirectX::XMFLOAT3 center, DirectX::XMFLOAT3 halfExtents);
        // Velocity-based kinematic move: Jolt computes the velocity that carries the body to the target
        // pose over dt (so it PUSHES dynamic bodies correctly, unlike teleporting the position).
        void MoveKinematic(BodyHandle h, DirectX::XMFLOAT3 targetPos, DirectX::XMFLOAT4 targetRotQuat, float dt);
        void       Finalize();     // OptimizeBroadPhase once, AFTER all bodies are added (do NOT call per frame)

        void Update(float dt);     // steps the simulation this frame

        // How many bodies Jolt is actually SOLVING this step, as opposed to how many exist.
        // Jolt deactivates a body once it has been at rest long enough, and a sleeping body costs
        // essentially nothing -- so this, not BodyCount(), is the number that explains solver time.
        //
        // It is the direct diagnostic for "physics time climbs and never comes back down": if the
        // pile has visibly settled and this stays high, bodies are NOT sleeping (usually because
        // something keeps waking them or they are jittering at rest), and the cost is a bug rather
        // than the honest price of more contacts.
        std::size_t ActiveBodyCount() const;

        // Set a dynamic body's linear velocity (and wake it) -- the slingshot launch: spawn a sphere, then
        // SetLinearVelocity(handle, aimDir * power). Plain types; safe on the main thread outside Update.
        void SetLinearVelocity(BodyHandle h, DirectX::XMFLOAT3 v);

        // Instantaneous impulse at the center of mass (kg*m/s) -- explosions, knockback, kicks. Unlike
        // SetLinearVelocity this ADDS momentum scaled by the body's mass instead of overwriting velocity.
        void AddImpulse(BodyHandle h, DirectX::XMFLOAT3 impulse);

        // Per-body surface material (Jolt defaults: friction 0.2, restitution 0 = no bounce). Call after
        // Add*; applies immediately. restitution 0..1 (bounciness), friction 0..~1+ (slide resistance).
        void SetFriction(BodyHandle h, float friction);
        void SetRestitution(BodyHandle h, float restitution);

        // --- Raycast (aiming, pickers, ground checks, line of sight) -----------------------------------
        // Casts from `from` along `dir` (normalized internally) up to maxDist. Returns true on hit and
        // fills `out`: world-space hit point + surface normal + the hit body's handle (kInvalidBody if the
        // hit body wasn't created through this wrapper -- shouldn't happen, every body goes through Add*).
        static constexpr BodyHandle kInvalidBody = static_cast<BodyHandle>(-1);
        struct RayHit {
            DirectX::XMFLOAT3 position;
            DirectX::XMFLOAT3 normal;
            BodyHandle        body = kInvalidBody;
        };
        bool RayCast(DirectX::XMFLOAT3 from, DirectX::XMFLOAT3 dir, float maxDist, RayHit& out) const;

        // --- Body user data (contact attribution) -------------------------------------------------------
        // A uint64 the app owns per body -- entity id, pointer, packed tag, whatever. Carried back on
        // every ContactEvent (userA/userB) so a contact can be mapped to GAME OBJECTS (scoring: "the
        // projectile hit tower block #7"), not just a position. 0 until set.
        void     SetUserData(BodyHandle h, uint64_t data);
        uint64_t GetUserData(BodyHandle h) const;

        // Remove + destroy a body (game reset / despawn). The handle's SLOT stays (handles are indices, so
        // no other handle shifts); the slot just goes inert -- GetBody returns zeros, other ops no-op.
        void RemoveBody(BodyHandle h);

        // --- Contact events (for impact effects / gameplay) --------------------------------------------
        // Jolt invokes the contact listener DURING Update, on scheduler worker fibers -- so Physics3D just
        // accumulates plain-type events under a mutex. Call DrainContactEvents AFTER Update (main thread):
        // it moves this step's events into `out` (appends) and clears the internal list. speed = closing
        // speed along the contact normal (m/s-ish) -- threshold it (~2+) to ignore resting/settling contacts.
        struct ContactEvent {
            DirectX::XMFLOAT3 position;   // world-space contact point
            DirectX::XMFLOAT3 normal;     // world-space contact normal
            float             speed;      // relative closing speed along the normal
            uint64_t          userA;      // SetUserData of the two bodies involved (0 if never set) --
            uint64_t          userB;      //   maps the contact to game entities for scoring/effects
            uint32_t          isSensor;   // 1 = a sensor was involved (trigger enter, no collision response;
                                          //     speed is usually low -- do NOT apply impact thresholds to these)
        };
        void DrainContactEvents(std::vector<ContactEvent>& out);

        // Query a body by the handle Add* returned. pos = world center, rot = world orientation (xyzw
        // quaternion), halfExtent = render half-extents (box: as given; sphere: r,r,r; capsule: r, halfH+r, r).
        std::size_t BodyCount() const;
        void GetBody(BodyHandle h, DirectX::XMFLOAT3& outPos,
                     DirectX::XMFLOAT4& outRotQuat, DirectX::XMFLOAT3& outHalfExtent) const;

        // --- Character controller ----------------------------------------------------------------------
        // A PLAYER/NPC controller, not a rigid body. Rigid bodies make terrible characters: they tip over,
        // slide down slopes, bounce off steps, and accumulate momentum you then have to fight. This wraps
        // Jolt's CharacterVirtual, which instead does collide-and-slide against the world every frame --
        // it has a position and a velocity you SET directly, and it resolves penetration, walks up steps,
        // and refuses to climb slopes past a limit.
        //
        // "Virtual" means it has no body in the simulation: the world pushes IT, it does not push the
        // world. That is what you want for a player (a crate shouldn't launch because you walked into it)
        // and it's why it needs its own update call rather than riding the normal body pipeline.
        using CharacterHandle = std::size_t;
        static constexpr CharacterHandle kInvalidCharacter = static_cast<CharacterHandle>(-1);

        // Capsule-shaped controller. `radius` is the capsule radius; `halfHeight` is the CYLINDER half
        // (total standing height = 2*halfHeight + 2*radius), matching AddDynamicCapsule's convention.
        // maxSlopeAngleDeg: steeper than this counts as a wall, not a floor (45 is a normal platformer
        // feel). stepHeight: how tall a ledge the character walks up without jumping -- 0 disables stair
        // walking, and it should be well under the capsule radius or it climbs walls.
        CharacterHandle AddCharacter(DirectX::XMFLOAT3 position, float radius = 0.3f,
                                     float halfHeight = 0.6f, float maxSlopeAngleDeg = 45.0f,
                                     float stepHeight = 0.3f);

        // Set the character's velocity for this frame (m/s, world space). This is a DIRECT set, not a
        // force -- gravity is applied for you inside UpdateCharacters, so a typical caller writes
        // horizontal movement from input and leaves Y alone except when jumping (see CharacterJump).
        void SetCharacterVelocity(CharacterHandle h, DirectX::XMFLOAT3 velocity);
        DirectX::XMFLOAT3 GetCharacterVelocity(CharacterHandle h) const;
        DirectX::XMFLOAT3 GetCharacterPosition(CharacterHandle h) const;   // capsule CENTER
        void SetCharacterPosition(CharacterHandle h, DirectX::XMFLOAT3 position);  // teleport (respawn)

        // What the character is standing on. Grounded is the one gameplay usually branches on; Sliding
        // means it's touching something too steep to stand on (so it will slide down and can't jump);
        // InAir means nothing underfoot.
        enum class GroundState : uint8_t { Grounded, Sliding, InAir };
        GroundState GetCharacterGroundState(CharacterHandle h) const;
        // World-space normal of the surface underfoot (0,1,0 when in air) -- for aligning a mesh to
        // slopes, or deciding a slide direction.
        DirectX::XMFLOAT3 GetCharacterGroundNormal(CharacterHandle h) const;

        // Convenience: sets vertical velocity to `speed` only when grounded. Returns whether it jumped,
        // so callers can trigger a sound/animation off the same call.
        bool CharacterJump(CharacterHandle h, float speed);

        // Steps every character: applies gravity, collides and slides, walks stairs, and sticks to the
        // floor going down slopes. Call ONCE per frame AFTER Update() -- the character resolves against
        // the world as the physics step left it, so running it first makes it collide with stale geometry.
        void UpdateCharacters(float dt);

        void RemoveCharacter(CharacterHandle h);

        // ---------------------------- Constraints (joints) ----------------------------
        // A constraint ties two bodies together and removes some of their relative freedom. These two
        // cover most of what a game actually needs: a HINGE for anything that swings about a fixed
        // axis (doors, hatches, levers, trapdoors, and the joints a ragdoll is built from), and a
        // POINT constraint -- a ball joint -- which pins two bodies at a spot while letting them
        // rotate freely, which is what a rope or chain is made of.
        //
        // Both take WORLD-space anchors, taken at the moment you create them: position the bodies
        // where you want them joined first, then constrain. Jolt records each body's local offset from
        // that anchor, so the pair holds its initial relative pose.
        //
        // Either body may be static -- that is the usual case (a door hinged to a wall, a rope anchored
        // to a ceiling). Two static bodies is pointless but harmless.
        using ConstraintHandle = std::size_t;
        static constexpr ConstraintHandle kInvalidConstraint = static_cast<ConstraintHandle>(-1);

        // Turn collision between two specific bodies on or off, independently of any constraint.
        //
        // Constrained bodies collide like any other pair unless told not to, and that is almost never
        // wanted: a door hinged at its own edge sweeps its thickness through the frame it hangs on and
        // jams after a few degrees; a ragdoll's upper arm permanently overlaps its shoulder. Both
        // AddHingeConstraint and AddPointConstraint therefore disable the pair BY DEFAULT (pass
        // disableCollision = false to keep it). This is the same default Unity, PhysX and Bullet use.
        //
        // Call it directly for pairs that aren't joined by a constraint -- a lift and its shaft, a
        // player and a trigger prop. Authoring-time only: do NOT call it during a step or from a
        // contact callback.
        void SetBodiesCollide(BodyHandle a, BodyHandle b, bool collide);

        // ------------------------- 2D / 2.5D mode (Plane2D) --------------------------
        // Locks a body to the XY plane: it can translate in X and Y and rotate about Z, nothing else.
        // That is a real 2D rigid body, simulated by the same solver, with the same constraints, shapes
        // and queries as everything above -- no second physics engine, no second set of concepts.
        //
        // "2.5D" is just this applied inside a 3D scene rendered with Renderer3D; "2D" is the same
        // bodies drawn with Renderer2D. The physics is identical either way.
        //
        // Call it AFTER creating the body. Static bodies don't need it (they never move).
        void SetBodyPlane2D(BodyHandle h);

        // --- Units bridge -------------------------------------------------------------
        // Physics wants meters and Y-UP. JLib's 2D world is PIXELS and Y-DOWN (PlatformerPhysics2D
        // runs gravity around 900 px/s^2). Something has to reconcile that, and it belongs HERE rather
        // than in every scene -- otherwise each caller reinvents the scale, the flip, and the rotation
        // sign, and they get it subtly different.
        //
        // NOTE: this conversion is NOT a Jolt quirk. Box2D is also meters and Y-up, and mixing pixels
        // into it is the single most common Box2D mistake. Swapping physics engines would not remove
        // one line of this.
        //
        // pixelsPerMeter: how many screen pixels one physics meter is worth. 100 is a good default --
        // a 1.8m character is then 180px. Too high and everything is tiny and jittery; too low and
        // gravity looks like the moon.
        void  SetPixelsPerMeter(float pixelsPerMeter);
        float GetPixelsPerMeter() const;

        // A Plane2D body's pose in the 2D renderer's own terms: pixels, Y-down, rotation in RADIANS
        // about the screen Z. Does the scale, the Y flip, and extracts the Z angle from the body's
        // quaternion -- so a 2D scene never sees a meter, a quaternion or a Z coordinate.
        // The rotation SIGN is flipped along with Y: mirroring an axis reverses which way positive
        // rotation turns, and getting that wrong makes everything spin backwards.
        void GetBody2D(BodyHandle h, DirectX::XMFLOAT2& outPosPx, float& outRotRadians) const;

        // Place/teleport a Plane2D body using the same pixel, Y-down convention.
        void SetBody2D(BodyHandle h, DirectX::XMFLOAT2 posPx, float rotRadians = 0.0f);

        // Gravity in PIXELS/s^2, Y-DOWN positive (so 900 falls downward on screen, matching
        // PlatformerPhysics2D's convention). Converts and flips internally.
        void SetGravity2D(float pixelsPerSecSquaredDown);

        // Hinge: bodyB swings about `axisWorld` through `pivotWorld`, like a door on its frame.
        // `axisWorld` is normalized here; {0,1,0} is a normal door, {1,0,0} or {0,0,1} a hatch/trapdoor.
        // Limits are in DEGREES relative to the pose the bodies are in RIGHT NOW (0 = as created), so a
        // door built closed and given (0, 90) opens one way only. The full -180..180 default is a free
        // spinner (a wheel or a swinging sign).
        ConstraintHandle AddHingeConstraint(BodyHandle bodyA, BodyHandle bodyB,
                                            DirectX::XMFLOAT3 pivotWorld, DirectX::XMFLOAT3 axisWorld,
                                            float minAngleDeg = -180.0f, float maxAngleDeg = 180.0f,
                                            bool disableCollision = true);

        // Drive a hinge to an angle instead of letting physics decide -- auto-closing doors, powered
        // hatches, a lever that returns to centre. `maxTorque` caps how hard it pushes: too low and a
        // heavy door never closes, too high and it slams through anything in the way (a motor is not a
        // teleport -- it still solves against contacts). Pass enabled=false to let it swing free again.
        void SetHingeMotor(ConstraintHandle h, bool enabled,
                           float targetAngleDeg = 0.0f, float maxTorque = 1000.0f);

        // Current hinge angle in degrees, measured the same way the limits are. Useful for gameplay
        // ("is the door open enough to walk through?") and for driving a door's open/closed sound.
        float GetHingeAngle(ConstraintHandle h) const;

        // Point (ball) constraint: pins the two bodies together at `pivotWorld`, rotation unrestricted.
        // A ROPE is a chain of these: N capsules in a line, each point-constrained to the next, with the
        // first pinned to a static anchor. ~8-15 segments reads as rope; fewer looks like linked bars.
        // Expect it to stretch under load -- that is inherent to an iterative solver, and the fix is
        // more solver iterations (or fewer, heavier segments), not a stiffer constraint.
        ConstraintHandle AddPointConstraint(BodyHandle bodyA, BodyHandle bodyB,
                                            DirectX::XMFLOAT3 pivotWorld, bool disableCollision = true);

        // Detach the two bodies. The slot goes inert rather than being erased, so other handles (which
        // are indices) never shift -- the same discipline bodies and characters use. Removing a body
        // does NOT remove constraints attached to it; drop the constraint first, or Jolt will be left
        // holding a dangling pair.
        void RemoveConstraint(ConstraintHandle h);

    private:
        struct Impl;                    // holds all the Jolt objects; defined in the .cpp
        std::unique_ptr<Impl> m_impl;
    };
}


