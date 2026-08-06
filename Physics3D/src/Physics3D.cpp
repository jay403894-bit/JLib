// JLib::Physics3D implementation -- the Jolt-backed physics library (see Physics3D.h for the interface
// contract). Boilerplate (layer interfaces, allocator/factory/type registration, the step call) adapted
// from Jolt's public-domain HelloWorld.cpp. All Jolt/JPH types stay inside THIS file (PIMPL). Jolt's jobs
// run on JLib::Scheduler via SchedulerJobSystem below (fastJob=true, loPri -> E-cores under P/E routing).

// Jolt.h MUST be included before any other Jolt header.
#include <Jolt/Jolt.h>
#include <Jolt/RegisterTypes.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Core/JobSystemWithBarrier.h>   // base for our scheduler-backed JobSystem (supplies barriers)
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/CylinderShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>   // pyramid/cone/etc. -- any convex shape from points
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/ContactListener.h>          // impact events -> particle bursts / gameplay
#include <Jolt/Physics/Collision/RayCast.h>                   // RRayCast (RayCast query input)
#include <Jolt/Physics/Collision/CastResult.h>                // RayCastResult
#include <Jolt/Physics/Body/BodyLock.h>                       // BodyLockRead (surface normal at the ray hit)
#include <Jolt/Physics/Body/BodyLockMulti.h>                  // BodyLockMultiWrite (both bodies of a constraint)
#include <Jolt/Physics/Character/CharacterVirtual.h>          // player/NPC controller (collide-and-slide)
#include <Jolt/Physics/Collision/Shape/MeshShape.h>           // static triangle-mesh level geometry
#include <Jolt/Physics/Collision/Shape/ScaledShape.h>         // uniform scale baked onto a shape
#include <Jolt/Physics/Constraints/HingeConstraint.h>         // doors/hatches/levers + ragdoll joints
#include <Jolt/Physics/Constraints/PointConstraint.h>         // ball joint -- rope/chain links
#include <cmath>   // std::cos/std::sin (cone base ring)
#include <mutex>   // contact-event accumulation (listener runs on worker fibers)
#include <unordered_set>   // PairFilter's disabled-pair set (per-pair collision exemption)
#include <Jolt/Physics/Collision/GroupFilter.h>     // base class for the per-pair filter below
#include <Jolt/Physics/Collision/CollisionGroup.h>  // CollisionGroup::SubGroupID

#include "Physics3D.h"
#include <Windows.h>   // OutputDebugStringA (trace/assert callbacks)
#include <cstdarg>
#include <cstdio>
#include <cassert>   // globals-lifecycle misuse: fires in debug, compiled out by NDEBUG in release
#include <vector>
#include <thread>            // hardware_concurrency (GetMaxConcurrency)
#include <TaskScheduler.h>   // JLib::TaskScheduler -- run Jolt's jobs on the fiber work-stealing pool

JPH_SUPPRESS_WARNINGS
using namespace JPH;
using namespace JPH::literals;

// ---- Jolt trace/assert callbacks -> the debug output window --------------------------------------------
static void TraceImpl(const char* inFMT, ...) {
    va_list list; va_start(list, inFMT);
    char buf[1024]; vsnprintf(buf, sizeof(buf), inFMT, list); va_end(list);
    OutputDebugStringA(buf); OutputDebugStringA("\n");
}
#ifdef JPH_ENABLE_ASSERTS
static bool AssertFailedImpl(const char* inExpr, const char* inMsg, const char* inFile, uint inLine) {
    char buf[1024];
    snprintf(buf, sizeof(buf), "[Jolt] %s:%u: (%s) %s\n", inFile, inLine, inExpr, inMsg ? inMsg : "");
    OutputDebugStringA(buf);
    return true;   // trigger a breakpoint
}
#endif

// ---- Collision layers (static "NON_MOVING" vs dynamic "MOVING") ---------------------------------------
namespace Layers {
    static constexpr ObjectLayer NON_MOVING = 0;
    static constexpr ObjectLayer MOVING     = 1;
    static constexpr ObjectLayer NUM_LAYERS = 2;
}
namespace BroadPhaseLayers {
    static constexpr BroadPhaseLayer NON_MOVING(0);
    static constexpr BroadPhaseLayer MOVING(1);
    static constexpr uint NUM_LAYERS(2);
}

class ObjectLayerPairFilterImpl : public ObjectLayerPairFilter {
public:
    virtual bool ShouldCollide(ObjectLayer o1, ObjectLayer o2) const override {
        switch (o1) {
        case Layers::NON_MOVING: return o2 == Layers::MOVING;   // static only collides with dynamic
        case Layers::MOVING:     return true;                    // dynamic collides with everything
        default:                 JPH_ASSERT(false); return false;
        }
    }
};
class BPLayerInterfaceImpl final : public BroadPhaseLayerInterface {
public:
    BPLayerInterfaceImpl() {
        mObjectToBroadPhase[Layers::NON_MOVING] = BroadPhaseLayers::NON_MOVING;
        mObjectToBroadPhase[Layers::MOVING]     = BroadPhaseLayers::MOVING;
    }
    virtual uint GetNumBroadPhaseLayers() const override { return BroadPhaseLayers::NUM_LAYERS; }
    virtual BroadPhaseLayer GetBroadPhaseLayer(ObjectLayer l) const override {
        JPH_ASSERT(l < Layers::NUM_LAYERS); return mObjectToBroadPhase[l];
    }
#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
    virtual const char* GetBroadPhaseLayerName(BroadPhaseLayer l) const override {
        switch ((BroadPhaseLayer::Type)l) {
        case (BroadPhaseLayer::Type)BroadPhaseLayers::NON_MOVING: return "NON_MOVING";
        case (BroadPhaseLayer::Type)BroadPhaseLayers::MOVING:     return "MOVING";
        default: JPH_ASSERT(false); return "INVALID";
        }
    }
#endif
private:
    BroadPhaseLayer mObjectToBroadPhase[Layers::NUM_LAYERS];
};
class ObjectVsBroadPhaseLayerFilterImpl : public ObjectVsBroadPhaseLayerFilter {
public:
    virtual bool ShouldCollide(ObjectLayer l1, BroadPhaseLayer l2) const override {
        switch (l1) {
        case Layers::NON_MOVING: return l2 == BroadPhaseLayers::MOVING;
        case Layers::MOVING:     return true;
        default:                 JPH_ASSERT(false); return false;
        }
    }
};

// ---- The payoff: a Jolt JobSystem that runs on JLib::Scheduler (the fiber work-stealing pool) ---------
// Jolt calls its physics work "jobs" with a dependency graph. We inherit JobSystemWithBarrier, which
// supplies CreateBarrier/DestroyBarrier/WaitForJobs and the barrier machinery -- so we only implement job
// allocation + QueueJob. QueueJob fire-and-forgets each ready job onto the scheduler (which runs it on a
// worker fiber). WaitForJobs is called from the thread driving PhysicsSystem::Update (the main thread);
// it blocks there while the pool runs the jobs AND executes ready barrier jobs inline, so it still makes
// progress even if the pool is busy with rendering. REQUIRES TaskScheduler::Init() to have run first.
// NOTE: jobs are new/delete'd per-create for now -- correct and cheap at these counts; a FixedSizeFreeList
// (like Jolt's own JobSystemThreadPool) is the later optimization if job churn ever shows up in a profile.
class SchedulerJobSystem final : public JobSystemWithBarrier {
public:
    explicit SchedulerJobSystem(uint inMaxBarriers) : JobSystemWithBarrier(inMaxBarriers) {}

    virtual int GetMaxConcurrency() const override {
        unsigned hw = std::thread::hardware_concurrency();
        return (int)(hw > 1 ? hw : 1);   // ~ the pool's worker count; Jolt uses it to size how it splits work
    }

    virtual JobHandle CreateJob(const char* inName, ColorArg inColor, const JobFunction& inJobFunction,
                                uint32 inNumDependencies) override {
        Job* job = new Job(inName, inColor, this, inJobFunction, inNumDependencies);
        JobHandle handle(job);                       // takes a reference -> keeps the job alive during this call
        if (inNumDependencies == 0) QueueJob(job);   // no dependencies -> ready to run right now
        return handle;
    }

protected:
    virtual void QueueJob(Job* inJob) override {
        // Hold a reference across the hop to the pool; the task releases it after running the job.
        inJob->AddRef();
        // fastJob = true: a Jolt job is PURE COMPUTE and never suspends (it never calls the scheduler's
        // WaitOnEvent), so it needs no fiber of its own -- run it inline on the worker's stack. That skips
        // the per-job fiber allocation + context switch, which matters because one physics step spawns many
        // small jobs. (It's the CreateTask default too, but pinned explicitly so intent is clear.)
        auto& sched = JLib::TaskScheduler::Instance();
        auto* task = sched.CreateTask([inJob]() { inJob->Execute(); inJob->Release(); },
                                      /*hipri*/ false, JLib::FiberSize::Standard, /*fastJob*/ true);
        if (task)
            sched.Push(task);
        else
            inJob->Release();   // pool allocator exhausted (~never at 1M slots): undo the AddRef; the barrier's
                                // WaitForJobs will run this job inline on the calling thread instead.
    }
    virtual void QueueJobs(Job** inJobs, uint inNumJobs) override {
        for (uint i = 0; i < inNumJobs; ++i) QueueJob(inJobs[i]);
    }
    virtual void FreeJob(Job* inJob) override { delete inJob; }
};

// Accumulates contact events for the scene. Jolt calls OnContactAdded DURING PhysicsSystem::Update, from
// physics jobs -- which run on JLib::Scheduler WORKER FIBERS -- so this only appends plain data under a
// mutex (never touches the renderer/scene). Only NEW contacts (not persisted ones) are recorded, so a box
// resting on the floor doesn't emit every step; the closing-speed field lets the scene threshold out gentle
// touches. Capped so a pathological pile-up can't grow the vector unbounded mid-step.
class ContactCollector final : public ContactListener {
public:
    std::mutex mtx;
    std::vector<JLib::Physics3D::ContactEvent> events;

    virtual void OnContactAdded(const Body& b1, const Body& b2, const ContactManifold& manifold,
                                ContactSettings& /*settings*/) override {
        RVec3 p = manifold.GetWorldSpaceContactPointOn1(0);
        Vec3  n = manifold.mWorldSpaceNormal;
        float speed = std::abs((b1.GetLinearVelocity() - b2.GetLinearVelocity()).Dot(n));
        std::lock_guard<std::mutex> g(mtx);
        if (events.size() < 256)
            events.push_back({ { (float)p.GetX(), (float)p.GetY(), (float)p.GetZ() },
                               { n.GetX(), n.GetY(), n.GetZ() }, speed,
                               b1.GetUserData(), b2.GetUserData(),
                               (b1.IsSensor() || b2.IsSensor()) ? 1u : 0u });
    }
};

namespace JLib {

// Sizing (small -- this is a demo; a real world uses ~65536 bodies).
static constexpr uint cMaxBodies            = 1024;
static constexpr uint cNumBodyMutexes       = 0;
static constexpr uint cMaxBodyPairs         = 1024;
static constexpr uint cMaxContactConstraints = 1024;

// All the Jolt state lives here. Member DECLARATION ORDER matters for destruction: physics_system holds
// references to the filters, so it's declared AFTER them -> destroyed BEFORE them (reverse order).
// Per-PAIR collision exemption. Jolt collides constrained bodies like any other pair, which is almost
// never what a joint author wants: a door hinged at its own edge sweeps its thickness straight through
// the frame it is hinged to and jams, and a ragdoll's upper arm permanently overlaps its shoulder.
// Every engine solves this the same way (Unity's joints default "Enable Collision" off, PhysX and
// Bullet the same), so constraints here disable the pair by default too.
//
// Jolt's stock GroupFilterTable is built for ONE group with a fixed sub-group count decided up front,
// which fits a ragdoll and not a scene that adds bodies as it goes. This filter instead gives every
// body its own sub-group ID (its BodyHandle) and keeps a set of disabled ID pairs, so ANY two bodies
// can be exempted at any time.
//
// THREADING: CanCollide runs on physics worker threads during a step. The set is only ever mutated at
// authoring time (adding constraints / calling SetBodiesCollide), never mid-step, so concurrent reads
// of an unmutated container are safe. Mutating it from inside a contact callback would not be.
namespace {
    class PairFilter : public JPH::GroupFilter {
    public:
        static uint64_t Key(JPH::CollisionGroup::SubGroupID a, JPH::CollisionGroup::SubGroupID b) {
            if (a > b) { auto t = a; a = b; b = t; }      // order-independent: (a,b) == (b,a)
            return ((uint64_t)a << 32) | (uint64_t)b;
        }
        bool CanCollide(const JPH::CollisionGroup& g1, const JPH::CollisionGroup& g2) const override {
            return disabled.find(Key(g1.GetSubGroupID(), g2.GetSubGroupID())) == disabled.end();
        }
        std::unordered_set<uint64_t> disabled;
    };
}

struct Physics3D::Impl {
    TempAllocatorImpl                 temp_allocator{ 10 * 1024 * 1024 };
    SchedulerJobSystem                job_system{ cMaxPhysicsBarriers };   // runs Jolt jobs on JLib::Scheduler
    BPLayerInterfaceImpl              bpli;
    ObjectVsBroadPhaseLayerFilterImpl ovbplf;
    ObjectLayerPairFilterImpl         oolpf;
    ContactCollector                  contacts;   // declared BEFORE physics_system (which holds a pointer to it)
    PhysicsSystem                     physics_system;
    BodyInterface*                    bi = nullptr;   // cached (stable for physics_system's lifetime)

    // Every body the scene has added, in creation order -- the BodyHandle returned by Add* is the index
    // here. Each body's RENDER half-extent rides alongside its BodyID so GetBody can hand the scene a
    // transform + size without the scene knowing anything about the underlying Jolt shape.
    struct BodyRec { BodyID id; DirectX::XMFLOAT3 halfExtent; bool active = true; };
    std::vector<BodyRec>              bodies;

    // Character controllers. Held by Ref<> because CharacterVirtual is intrusively refcounted like
    // shapes are -- a raw `new` arrives at refcount 0 and would be owned by nothing. Slots go inert
    // rather than being erased, so handles (which are indices) never shift under the caller.
    struct CharRec {
        Ref<CharacterVirtual> ch;
        RefConst<Shape>       shape;      // keep the capsule alive alongside the controller
        bool                  active = true;
        float                 halfHeight = 0.0f;   // cylinder half, for render extents
        float                 radius = 0.0f;
    };
    std::vector<CharRec>              characters;

    // Constraints. Ref<> for the same reason characters are: Jolt constraints are intrusively
    // refcounted, so a raw `new` arrives at refcount 0 owned by nothing. physics_system.AddConstraint
    // takes its own reference, but holding one here is what lets RemoveConstraint drop it deliberately
    // instead of relying on the system's. Slots go inert rather than being erased so handles (indices)
    // never shift under the caller.
    struct ConRec { Ref<Constraint> c; bool active = true; };
    std::vector<ConRec>               constraints;

    // Shared by every body; see PairFilter above. Ref<> because GroupFilter is refcounted and the
    // bodies only hold a raw pointer to it -- it has to outlive them, and this owns the reference.
    Ref<PairFilter>                   pairFilter = new PairFilter();

    // Pixels per physics meter -- the 2D bridge (see SetPixelsPerMeter). 100 means a 1.8m character is
    // 180px. Only ever read by the 2D helpers; 3D callers never touch it.
    float                             pixelsPerMeter = 100.0f;

    // Reverse map: Jolt BodyID -> our BodyHandle. Queries (raycasts, and every shape/overlap query
    // that follows) hand back a BodyID, and callers need the handle they created.
    //
    // A DENSE VECTOR indexed by BodyID::GetIndex(), not a hash map: Jolt allocates body indices
    // densely from its own free list, so this is a bounded array with an O(1) load and no hashing,
    // no pointer chasing, and no allocation on lookup. It replaced a linear scan over every body --
    // fine for a demo, O(n) per query the moment a scene has thousands of bodies, and raycasts are
    // exactly the thing gameplay calls many times per frame.
    std::vector<Physics3D::BodyHandle> idToHandle;

    void MapId(BodyID id, Physics3D::BodyHandle h) {
        const uint32 idx = id.GetIndex();
        if (idx >= idToHandle.size()) idToHandle.resize(idx + 1, Physics3D::kInvalidBody);
        idToHandle[idx] = h;
    }
    Physics3D::BodyHandle HandleOf(BodyID id) const {
        if (id.IsInvalid()) return Physics3D::kInvalidBody;
        const uint32 idx = id.GetIndex();
        if (idx >= idToHandle.size()) return Physics3D::kInvalidBody;
        const Physics3D::BodyHandle h = idToHandle[idx];
        // Guard against a stale entry: Jolt RECYCLES body indices, so a removed body's slot can be
        // handed to a new body. Verify the handle still points at this exact BodyID before trusting it.
        if (h == Physics3D::kInvalidBody || h >= bodies.size()) return Physics3D::kInvalidBody;
        return (bodies[h].active && bodies[h].id == id) ? h : Physics3D::kInvalidBody;
    }

    // Shared creation path for every Add* verb -- defined HERE (in the .cpp) so it can take Jolt types
    // without leaking them into the header. Wraps `shape` in a body at `center`, adds it to the world
    // (static starts inactive, dynamic active), records it + its render extent, and returns the handle.
    // Takes ShapeRefC (NOT a raw Shape*): Jolt shapes are intrusively refcounted, and a `new XShape(...)`
    // arrives here at refcount 0 -- as a raw pointer it would be owned by NOTHING until BodyCreationSettings
    // adopts it, leaking on any early return. ShapeRefC claims the reference AT THE CALL SITE (implicit
    // conversion from the raw new), so ownership is airtight no matter what happens inside.
    Physics3D::BodyHandle add(ShapeRefC shape, DirectX::XMFLOAT3 c, EMotionType motion,
                              ObjectLayer layer, DirectX::XMFLOAT3 renderHalf, float massKg = 0.0f,
                              bool ccd = false, bool sensor = false) {
        const EActivation act = (motion == EMotionType::Static) ? EActivation::DontActivate : EActivation::Activate;
        BodyCreationSettings bcs(shape, RVec3(c.x, c.y, c.z), Quat::sIdentity(), motion, layer);
        if (massKg > 0.0f) {   // gameplay mass override; inertia recalculated for the shape at this mass
            bcs.mOverrideMassProperties = EOverrideMassProperties::CalculateInertia;
            bcs.mMassPropertiesOverride.mMass = massKg;
        }
        if (ccd)   // continuous collision: fast bodies sweep (LinearCast) instead of teleporting per step
            bcs.mMotionQuality = EMotionQuality::LinearCast;
        bcs.mIsSensor = sensor;   // sensor = contact events, no collision response (trigger volume)
        // Every body gets its OWN sub-group ID -- its BodyHandle -- so any pair can later be exempted
        // from colliding (see PairFilter). Group ID is always 0: the filter keys purely on sub-groups.
        bcs.mCollisionGroup = CollisionGroup(pairFilter, 0,
                                             (CollisionGroup::SubGroupID)bodies.size());
        bodies.push_back({ bi->CreateAndAddBody(bcs, act), renderHalf });
        MapId(bodies.back().id, bodies.size() - 1);   // keep the BodyID -> handle map in step
        return bodies.size() - 1;
    }
};

// ---- Process-wide Jolt lifecycle ----------------------------------------------------------------------
// Not a function-local `static bool` guard inside Init() any more. That guard was wrong twice over:
//   1. It never tore anything down, so Factory::sInstance and the whole type registry leaked for the
//      life of the process. Fine for a demo that exits; not fine for a library a host embeds.
//   2. `static bool x = false;` is CONSTANT-initialized, so the compiler emits no thread-safe-static
//      guard -- the check-then-set was a plain non-atomic read/write. Two threads reaching Init() at
//      once would both register the types and both `new Factory()`, leaking one and asserting inside
//      Jolt. Never fired because scene setup happens on the main thread, but nothing enforced that.
// The lifetime is the HOST's, so it is explicit and single-threaded by contract (see the header).
static bool   s_globalsUp     = false;
static size_t s_liveInstances = 0;   // Physics3D objects constructed but not yet destroyed

void Physics3D::InitGlobals() {
    if (s_globalsUp) return;   // idempotent: a host that calls it twice is not an error
    RegisterDefaultAllocator();
    Trace = TraceImpl;
    JPH_IF_ENABLE_ASSERTS(AssertFailed = AssertFailedImpl;)
    Factory::sInstance = new Factory();
    RegisterTypes();
    s_globalsUp = true;
}

void Physics3D::ShutdownGlobals() {
    if (!s_globalsUp) return;
    // REFUSE rather than tear down under a live world. ~PhysicsSystem and every ~Shape reach into the
    // Factory and the type registry, so freeing them first turns scene destruction into a use-after-free.
    // The realistic way to hit this is a scene stack with static storage duration (SceneManager::scenes
    // is exactly that): its scenes -- and the Physics3D each one owns -- are destroyed at static
    // destruction time, i.e. AFTER main returns and after any ShutdownGlobals() call inside main.
    // Bailing out here leaves the globals up for process exit to reclaim: the old leaky behaviour,
    // which is never WORSE than what this replaced. Destroy your scenes first (SceneManager::Clear()).
    if (s_liveInstances != 0) {
        char buf[160];
        snprintf(buf, sizeof(buf),
                 "[JLib::Physics3D] ShutdownGlobals() skipped: %zu instance(s) still alive. "
                 "Destroy them first.\n", s_liveInstances);
        OutputDebugStringA(buf);
        // Plain assert(), NOT JPH_ASSERT: deploy_lib.bat does not define JPH_ENABLE_ASSERTS, so every
        // JPH_ASSERT in this build compiles to nothing. assert() keys off NDEBUG, which the script sets
        // only on the release half -- so this actually fires in debug, which is the whole point.
        assert(false && "Physics3D::ShutdownGlobals() called with live instances");
        return;
    }
    UnregisterTypes();
    delete Factory::sInstance;
    Factory::sInstance = nullptr;
    s_globalsUp = false;
}

size_t Physics3D::LiveInstanceCount() { return s_liveInstances; }

Physics3D::Physics3D() { ++s_liveInstances; }
Physics3D::~Physics3D() { --s_liveInstances; }

void Physics3D::Init() {
    // Fail LOUD in debug, keep running in release. A missing InitGlobals() otherwise surfaces as an
    // assert from deep inside Jolt (or worse, a null Factory deref) with nothing pointing at the real
    // cause; a shipped game should not die over it when one call fixes it up.
    if (!s_globalsUp) {
        OutputDebugStringA("[JLib::Physics3D] Init() before InitGlobals() -- initialising globals now. "
                           "Call Physics3D::InitGlobals() from your startup path.\n");
        assert(false && "Physics3D::Init() called before Physics3D::InitGlobals()");
        InitGlobals();   // release-mode safety net: one missed call should not kill a shipped game
    }

    m_impl = std::make_unique<Impl>();
    Impl& p = *m_impl;
    p.physics_system.Init(cMaxBodies, cNumBodyMutexes, cMaxBodyPairs, cMaxContactConstraints,
                          p.bpli, p.ovbplf, p.oolpf);
    p.bi = &p.physics_system.GetBodyInterface();
    p.physics_system.SetContactListener(&p.contacts);   // impact events -> DrainContactEvents (see header)
    // No bodies are created here anymore -- the scene authors its world via the Add* verbs + Finalize().
}

void Physics3D::SetLinearVelocity(BodyHandle h, DirectX::XMFLOAT3 v) {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return;
    const Impl::BodyRec& rec = m_impl->bodies[h];
    m_impl->bi->SetLinearVelocity(rec.id, Vec3(v.x, v.y, v.z));
    m_impl->bi->ActivateBody(rec.id);   // a sleeping body ignores velocity until woken
}

Physics3D::BodyHandle Physics3D::AddSensorBox(DirectX::XMFLOAT3 c, DirectX::XMFLOAT3 half) {
    // Static + sensor: overlapping dynamic bodies fire ContactEvents (isSensor=1) but pass through.
    return m_impl->add(new BoxShape(Vec3(half.x, half.y, half.z)), c, EMotionType::Static, Layers::NON_MOVING,
                       half, 0.0f, false, /*sensor*/ true);
}

Physics3D::BodyHandle Physics3D::AddKinematicBox(DirectX::XMFLOAT3 c, DirectX::XMFLOAT3 half) {
    // MOVING layer (it moves, so it must collide with everything dynamic AND be seen by statics).
    return m_impl->add(new BoxShape(Vec3(half.x, half.y, half.z)), c, EMotionType::Kinematic, Layers::MOVING, half);
}

void Physics3D::MoveKinematic(BodyHandle h, DirectX::XMFLOAT3 pos, DirectX::XMFLOAT4 rot, float dt) {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active || dt <= 0.0f) return;
    m_impl->bi->MoveKinematic(m_impl->bodies[h].id, RVec3(pos.x, pos.y, pos.z),
                              Quat(rot.x, rot.y, rot.z, rot.w), dt);
}

void Physics3D::SetUserData(BodyHandle h, uint64_t data) {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return;
    m_impl->bi->SetUserData(m_impl->bodies[h].id, data);
}

uint64_t Physics3D::GetUserData(BodyHandle h) const {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return 0;
    return m_impl->bi->GetUserData(m_impl->bodies[h].id);
}

void Physics3D::AddImpulse(BodyHandle h, DirectX::XMFLOAT3 v) {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return;
    m_impl->bi->AddImpulse(m_impl->bodies[h].id, Vec3(v.x, v.y, v.z));   // wakes the body itself
}

void Physics3D::SetFriction(BodyHandle h, float f) {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return;
    m_impl->bi->SetFriction(m_impl->bodies[h].id, f);
}

void Physics3D::SetRestitution(BodyHandle h, float r) {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return;
    m_impl->bi->SetRestitution(m_impl->bodies[h].id, r);
}

bool Physics3D::RayCast(DirectX::XMFLOAT3 from, DirectX::XMFLOAT3 dir, float maxDist, RayHit& out) const {
    if (!m_impl) return false;
    float len = std::sqrt(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
    if (len < 1e-8f || maxDist <= 0.0f) return false;
    Vec3 d(dir.x / len * maxDist, dir.y / len * maxDist, dir.z / len * maxDist);

    RRayCast ray{ RVec3(from.x, from.y, from.z), d };
    RayCastResult hit;
    if (!m_impl->physics_system.GetNarrowPhaseQuery().CastRay(ray, hit)) return false;

    RVec3 p = ray.GetPointOnRay(hit.mFraction);
    Vec3  n = Vec3::sAxisY();   // safe fallback if the body lock fails (body removed mid-query)
    {
        BodyLockRead lock(m_impl->physics_system.GetBodyLockInterface(), hit.mBodyID);
        if (lock.Succeeded())
            n = lock.GetBody().GetWorldSpaceSurfaceNormal(hit.mSubShapeID2, p);
    }
    out.position = { (float)p.GetX(), (float)p.GetY(), (float)p.GetZ() };
    out.normal   = { n.GetX(), n.GetY(), n.GetZ() };
    out.body     = m_impl->HandleOf(hit.mBodyID);
    return true;
}

void Physics3D::RemoveBody(BodyHandle h) {
    if (!m_impl || h >= m_impl->bodies.size()) return;
    Impl::BodyRec& rec = m_impl->bodies[h];
    if (!rec.active) return;            // already removed
    m_impl->MapId(rec.id, kInvalidBody); // drop the reverse mapping BEFORE Jolt can recycle the index
    m_impl->bi->RemoveBody(rec.id);     // out of the simulation
    m_impl->bi->DestroyBody(rec.id);    // free the body slot
    rec.active = false;                 // slot stays (handles are indices); GetBody now returns zeros
}

void Physics3D::DrainContactEvents(std::vector<ContactEvent>& out) {
    if (!m_impl) return;
    std::lock_guard<std::mutex> g(m_impl->contacts.mtx);
    auto& ev = m_impl->contacts.events;
    out.insert(out.end(), ev.begin(), ev.end());
    ev.clear();
}

// --- World authoring verbs: thin wrappers over Impl::add, each picking a shape + motion type + layer. The
// render half-extent handed to add() is what GetBody returns for scaling the scene's mesh (box: as given;
// sphere: r on every axis; capsule: r wide, halfHeight+r tall). -----------------------------------------
Physics3D::BodyHandle Physics3D::AddStaticBox(DirectX::XMFLOAT3 c, DirectX::XMFLOAT3 half) {
    return m_impl->add(new BoxShape(Vec3(half.x, half.y, half.z)), c, EMotionType::Static, Layers::NON_MOVING, half);
}
Physics3D::BodyHandle Physics3D::AddDynamicBox(DirectX::XMFLOAT3 c, DirectX::XMFLOAT3 half, float massKg) {
    return m_impl->add(new BoxShape(Vec3(half.x, half.y, half.z)), c, EMotionType::Dynamic, Layers::MOVING,
                       half, massKg);
}
Physics3D::BodyHandle Physics3D::AddDynamicSphere(DirectX::XMFLOAT3 c, float r, float massKg, bool ccd) {
    return m_impl->add(new SphereShape(r), c, EMotionType::Dynamic, Layers::MOVING, { r, r, r }, massKg, ccd);
}
Physics3D::BodyHandle Physics3D::AddDynamicCapsule(DirectX::XMFLOAT3 c, float halfHeight, float r) {
    // Jolt CapsuleShape(halfHeightOfCylinder, radius): total height = 2*(halfHeight + r).
    return m_impl->add(new CapsuleShape(halfHeight, r), c, EMotionType::Dynamic, Layers::MOVING,
                       { r, halfHeight + r, r });
}
Physics3D::BodyHandle Physics3D::AddDynamicCylinder(DirectX::XMFLOAT3 c, float halfHeight, float r) {
    // CylinderShape(halfHeight, radius): flat-capped tube, total height 2*halfHeight -- a Jolt PRIMITIVE
    // (no hull needed, unlike the pyramid/cone). Render extent (r, halfHeight, r).
    return m_impl->add(new CylinderShape(halfHeight, r), c, EMotionType::Dynamic, Layers::MOVING,
                       { r, halfHeight, r });
}
Physics3D::BodyHandle Physics3D::AddDynamicPyramid(DirectX::XMFLOAT3 c, DirectX::XMFLOAT3 half) {
    // Jolt has NO pyramid primitive -- but a pyramid is CONVEX, so build a ConvexHullShape from its 5 points
    // (Jolt computes the hull). Points match MakePyramidMesh: square base +/-half in XZ at -half.y, apex at
    // +half.y. ConvexHullShape rounds the edges by a tiny convex radius for stable contacts (invisible next
    // to the sharp visual mesh). This is the general path for ANY convex shape that isn't a built-in primitive.
    Array<Vec3> points;
    points.reserve(5);
    points.push_back(Vec3(0.0f, half.y, 0.0f));            // apex
    points.push_back(Vec3(-half.x, -half.y, -half.z));     // base corners
    points.push_back(Vec3( half.x, -half.y, -half.z));
    points.push_back(Vec3( half.x, -half.y,  half.z));
    points.push_back(Vec3(-half.x, -half.y,  half.z));
    // NOTE: no SetEmbedded() here -- that's only needed when a stack RefTarget gets REFERENCED (Jolt's
    // HelloWorld passes &settings into BodyCreationSettings, which refs it). Create() only copies data out.
    ConvexHullShapeSettings settings(points);
    ShapeRefC shape = settings.Create().Get();
    return m_impl->add(shape, c, EMotionType::Dynamic, Layers::MOVING, half);
}
Physics3D::BodyHandle Physics3D::AddDynamicCone(DirectX::XMFLOAT3 c, float radius, float halfHeight) {
    // No cone primitive either -- TaperedCylinderShape can't reach a zero-radius tip -- so a convex hull of
    // the apex + a ring of base points (matches MakeConeMesh: base radius at -halfHeight, apex at +halfHeight).
    // The curved side becomes a faceted hull; kRing controls how round the collision is.
    const int   kRing = 20;
    const float TAU   = 6.28318530717959f;
    Array<Vec3> points;
    points.reserve(kRing + 1);
    points.push_back(Vec3(0.0f, halfHeight, 0.0f));   // apex
    for (int i = 0; i < kRing; ++i) {
        float th = TAU * i / kRing;
        points.push_back(Vec3(radius * std::cos(th), -halfHeight, radius * std::sin(th)));
    }
    ConvexHullShapeSettings settings(points);   // no SetEmbedded -- see AddDynamicPyramid's note
    ShapeRefC shape = settings.Create().Get();
    return m_impl->add(shape, c, EMotionType::Dynamic, Layers::MOVING, { radius, halfHeight, radius });
}
void Physics3D::Finalize() {
    if (m_impl) m_impl->physics_system.OptimizeBroadPhase();   // one-time, AFTER authoring; NEVER per frame
}

void Physics3D::Update(float dt) {
    if (!m_impl) return;
    if (dt <= 0.0f) return;
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;   // clamp a hitch so the sim doesn't explode
    // 1 collision step per ~1/60s is stable for these step sizes.
    m_impl->physics_system.Update(dt, 1, &m_impl->temp_allocator, &m_impl->job_system);
}

std::size_t Physics3D::BodyCount() const { return m_impl ? m_impl->bodies.size() : 0; }

void Physics3D::GetBody(BodyHandle h, DirectX::XMFLOAT3& outPos,
                        DirectX::XMFLOAT4& outRotQuat, DirectX::XMFLOAT3& outHalfExtent) const {
    const Impl& p = *m_impl;
    if (h >= p.bodies.size() || !p.bodies[h].active) {   // removed/invalid: inert zeros (radius 0 = draw nothing sane)
        outPos = { 0, -1000.0f, 0 }; outRotQuat = { 0, 0, 0, 1 }; outHalfExtent = { 0, 0, 0 };
        return;
    }
    const Impl::BodyRec& rec = p.bodies[h];
    RVec3 pos = p.bi->GetPosition(rec.id);   // const query -- safe through the cached interface pointer
    Quat  rot = p.bi->GetRotation(rec.id);
    outPos        = { (float)pos.GetX(), (float)pos.GetY(), (float)pos.GetZ() };
    outRotQuat    = { rot.GetX(), rot.GetY(), rot.GetZ(), rot.GetW() };
    outHalfExtent = rec.halfExtent;
}

// ============================ Character controller (CharacterVirtual) ============================
// See the header for why a character is NOT a rigid body. Everything Jolt-typed stays in this file;
// the header exposes only handles and DirectXMath types.

Physics3D::CharacterHandle Physics3D::AddCharacter(DirectX::XMFLOAT3 position, float radius,
                                                   float halfHeight, float maxSlopeAngleDeg,
                                                   float stepHeight) {
    if (!m_impl) return kInvalidCharacter;
    Impl& p = *m_impl;

    CharacterVirtualSettings settings;
    // Capsule matching AddDynamicCapsule's convention: halfHeight is the CYLINDER half, so the
    // total standing height is 2*halfHeight + 2*radius.
    RefConst<Shape> shape = new CapsuleShape(halfHeight, radius);
    settings.mShape = shape;
    settings.mMaxSlopeAngle = DegreesToRadians(maxSlopeAngleDeg);
    // The plane that decides "am I inside a wall". Jolt wants it at the bottom of the capsule,
    // offset by the radius, or the controller can be pushed through thin geometry.
    settings.mSupportingVolume = Plane(Vec3::sAxisY(), -radius);
    // Mass only matters for how hard the character pushes dynamic bodies it walks into.
    settings.mMass = 70.0f;
    // How hard it can push, in NEWTONS. Jolt's default is 100N, which is roughly "can nudge an empty
    // cardboard box" -- walk into anything with real volume and nothing moves, which reads as the
    // collision being broken rather than the character being weak. A 1.2 x 2.0 x 0.16m door at default
    // density is already ~380kg, so 100N does not visibly move it. 600N is about what a person can
    // shove with their whole body, and makes doors/crates respond without letting the player bulldoze
    // heavy geometry. This is a FORCE cap, not a mass override: genuinely heavy things still resist.
    settings.mMaxStrength = 600.0f;

    Ref<CharacterVirtual> ch = new CharacterVirtual(
        &settings, RVec3(position.x, position.y, position.z), Quat::sIdentity(), &p.physics_system);

    Impl::CharRec rec;
    rec.ch = ch;
    rec.shape = shape;
    rec.halfHeight = halfHeight;
    rec.radius = radius;
    rec.active = true;
    p.characters.push_back(rec);

    // Stash the step height on the controller itself via its user data slot would be opaque; keep it
    // in a parallel field instead. (Stored on the record below so UpdateCharacters can read it.)
    p.characters.back().ch->SetUserData((uint64)(stepHeight * 1000.0f));   // mm, see UpdateCharacters
    return p.characters.size() - 1;
}

void Physics3D::SetCharacterVelocity(CharacterHandle h, DirectX::XMFLOAT3 v) {
    if (!m_impl || h >= m_impl->characters.size() || !m_impl->characters[h].active) return;
    m_impl->characters[h].ch->SetLinearVelocity(Vec3(v.x, v.y, v.z));
}

DirectX::XMFLOAT3 Physics3D::GetCharacterVelocity(CharacterHandle h) const {
    if (!m_impl || h >= m_impl->characters.size() || !m_impl->characters[h].active) return { 0, 0, 0 };
    Vec3 v = m_impl->characters[h].ch->GetLinearVelocity();
    return { v.GetX(), v.GetY(), v.GetZ() };
}

DirectX::XMFLOAT3 Physics3D::GetCharacterPosition(CharacterHandle h) const {
    if (!m_impl || h >= m_impl->characters.size() || !m_impl->characters[h].active)
        return { 0, -1000.0f, 0 };   // same inert sentinel GetBody uses for dead slots
    RVec3 pos = m_impl->characters[h].ch->GetPosition();
    return { (float)pos.GetX(), (float)pos.GetY(), (float)pos.GetZ() };
}

void Physics3D::SetCharacterPosition(CharacterHandle h, DirectX::XMFLOAT3 pos) {
    if (!m_impl || h >= m_impl->characters.size() || !m_impl->characters[h].active) return;
    m_impl->characters[h].ch->SetPosition(RVec3(pos.x, pos.y, pos.z));
}

Physics3D::GroundState Physics3D::GetCharacterGroundState(CharacterHandle h) const {
    if (!m_impl || h >= m_impl->characters.size() || !m_impl->characters[h].active)
        return GroundState::InAir;
    switch (m_impl->characters[h].ch->GetGroundState()) {
        case CharacterBase::EGroundState::OnGround:      return GroundState::Grounded;
        case CharacterBase::EGroundState::OnSteepGround: return GroundState::Sliding;
        // NotSupported means "touching something, but it can't support us" -- gameplay-wise that is
        // the same as sliding, not the same as standing.
        case CharacterBase::EGroundState::NotSupported:  return GroundState::Sliding;
        default:                                         return GroundState::InAir;
    }
}

DirectX::XMFLOAT3 Physics3D::GetCharacterGroundNormal(CharacterHandle h) const {
    if (!m_impl || h >= m_impl->characters.size() || !m_impl->characters[h].active) return { 0, 1, 0 };
    Vec3 n = m_impl->characters[h].ch->GetGroundNormal();
    if (n.IsNearZero()) return { 0, 1, 0 };
    return { n.GetX(), n.GetY(), n.GetZ() };
}

bool Physics3D::CharacterJump(CharacterHandle h, float speed) {
    if (GetCharacterGroundState(h) != GroundState::Grounded) return false;
    Vec3 v = m_impl->characters[h].ch->GetLinearVelocity();
    m_impl->characters[h].ch->SetLinearVelocity(Vec3(v.GetX(), speed, v.GetZ()));
    return true;
}

void Physics3D::UpdateCharacters(float dt) {
    if (!m_impl || dt <= 0.0f) return;
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;   // same hitch clamp Update() uses
    Impl& p = *m_impl;
    const Vec3 gravity = p.physics_system.GetGravity();

    for (auto& rec : p.characters) {
        if (!rec.active) continue;
        CharacterVirtual* ch = rec.ch;

        // Gravity is integrated here rather than by the solver: a virtual character isn't in the
        // simulation, so nothing else would ever accelerate it downward. Only while NOT grounded --
        // accumulating gravity while standing still builds a large downward velocity that fights
        // stair-walking and makes the character stick to slopes it should walk up.
        if (ch->GetGroundState() != CharacterBase::EGroundState::OnGround) {
            ch->SetLinearVelocity(ch->GetLinearVelocity() + gravity * dt);
        }
        else if (ch->GetLinearVelocity().GetY() < 0.0f) {
            // Grounded: cancel residual downward velocity so it doesn't accumulate frame over frame.
            Vec3 v = ch->GetLinearVelocity();
            ch->SetLinearVelocity(Vec3(v.GetX(), 0.0f, v.GetZ()));
        }

        const float stepHeight = (float)ch->GetUserData() / 1000.0f;

        // ExtendedUpdate (not Update) is what gives stairs and slopes their solid feel: it walks the
        // character UP small ledges instead of stopping dead, and STICKS it to the floor going down
        // slopes instead of launching it into a series of little hops.
        CharacterVirtual::ExtendedUpdateSettings us;
        us.mWalkStairsStepUp = Vec3(0, stepHeight, 0);
        us.mStickToFloorStepDown = Vec3(0, -stepHeight, 0);

        ch->ExtendedUpdate(dt, gravity, us,
            p.physics_system.GetDefaultBroadPhaseLayerFilter(Layers::MOVING),
            p.physics_system.GetDefaultLayerFilter(Layers::MOVING),
            {}, {}, p.temp_allocator);
    }
}

void Physics3D::RemoveCharacter(CharacterHandle h) {
    if (!m_impl || h >= m_impl->characters.size() || !m_impl->characters[h].active) return;
    Impl::CharRec& rec = m_impl->characters[h];
    rec.ch = nullptr;       // Ref<> release -- the controller owns no body, so nothing to remove
    rec.shape = nullptr;
    rec.active = false;     // slot stays so other handles (indices) don't shift
}


// ================================ Constraints (hinge / point) ================================
// Both verbs share a shape: validate the two handles, take a WRITE LOCK on both bodies, build the
// settings in world space, Create() the constraint, hand it to the physics system, and record it.
//
// The body lock is not optional. TwoBodyConstraintSettings::Create takes `Body&`, not BodyID, and the
// only safe way to turn an ID into a reference is through the lock interface -- the body array can be
// reallocated by other work, and a raw pointer taken outside a lock can dangle. BodyLockMultiWrite
// takes both at once specifically so two separate locks can't deadlock against another thread
// grabbing the same pair in the opposite order.

// Shared helper: resolve two BodyHandles to locked Body pointers. Returns false (and locks nothing
// usable) if either handle is dead.
namespace {
    struct LockedPair {
        JPH::BodyLockMultiWrite lock;
        JPH::Body* a = nullptr;
        JPH::Body* b = nullptr;
        LockedPair(const JPH::BodyLockInterface& iface, const JPH::BodyID* ids)
            : lock(iface, ids, 2) {
            a = lock.GetBody(0);
            b = lock.GetBody(1);
        }
        bool valid() const { return a != nullptr && b != nullptr; }
    };
}

Physics3D::ConstraintHandle Physics3D::AddHingeConstraint(BodyHandle bodyA, BodyHandle bodyB,
                                                          DirectX::XMFLOAT3 pivotWorld,
                                                          DirectX::XMFLOAT3 axisWorld,
                                                          float minAngleDeg, float maxAngleDeg,
                                                          bool disableCollision) {
    if (!m_impl) return kInvalidConstraint;
    Impl& p = *m_impl;
    if (bodyA >= p.bodies.size() || !p.bodies[bodyA].active) return kInvalidConstraint;
    if (bodyB >= p.bodies.size() || !p.bodies[bodyB].active) return kInvalidConstraint;

    Vec3 axis(axisWorld.x, axisWorld.y, axisWorld.z);
    if (axis.IsNearZero()) return kInvalidConstraint;    // a hinge with no axis is meaningless
    axis = axis.Normalized();

    const BodyID ids[2] = { p.bodies[bodyA].id, p.bodies[bodyB].id };
    LockedPair lp(p.physics_system.GetBodyLockInterface(), ids);
    if (!lp.valid()) return kInvalidConstraint;

    HingeConstraintSettings s;
    s.mSpace = EConstraintSpace::WorldSpace;             // anchors given in world, not body-local
    s.mPoint1 = s.mPoint2 = RVec3(pivotWorld.x, pivotWorld.y, pivotWorld.z);
    s.mHingeAxis1 = s.mHingeAxis2 = axis;
    // The NORMAL axis is the reference direction the angle is measured FROM. It only has to be
    // perpendicular to the hinge axis; any perpendicular works, and giving both bodies the same one
    // makes the current pose read as angle 0 -- which is what the limits are documented against.
    s.mNormalAxis1 = s.mNormalAxis2 = axis.GetNormalizedPerpendicular();
    s.mLimitsMin = DegreesToRadians(minAngleDeg);
    s.mLimitsMax = DegreesToRadians(maxAngleDeg);

    Ref<Constraint> c = s.Create(*lp.a, *lp.b);
    if (c == nullptr) return kInvalidConstraint;
    p.physics_system.AddConstraint(c);

    // Stop the two joined bodies colliding, unless the caller wants them to. Without this a door
    // hinged at its own edge sweeps its thickness through the frame and jams after a few degrees --
    // it looks exactly like a broken constraint, which is the trap this default exists to avoid.
    if (disableCollision) SetBodiesCollide(bodyA, bodyB, false);

    p.constraints.push_back(Impl::ConRec{ c, true });
    return p.constraints.size() - 1;
}

void Physics3D::SetHingeMotor(ConstraintHandle h, bool enabled, float targetAngleDeg, float maxTorque) {
    if (!m_impl || h >= m_impl->constraints.size() || !m_impl->constraints[h].active) return;
    // GetSubType, NOT dynamic_cast. Jolt ships its own RTTI system and is built with C++ RTTI OFF, so
    // a dynamic_cast on a Constraint* reads a vtable slot that holds no type_info -- it doesn't fail
    // cleanly, it dereferences whatever bytes are there and access-violates on a nonsense address.
    Constraint* base = m_impl->constraints[h].c.GetPtr();
    if (!base || base->GetSubType() != EConstraintSubType::Hinge) return;   // e.g. a point constraint
    HingeConstraint* hinge = static_cast<HingeConstraint*>(base);
    if (enabled) {
        // Position mode (not velocity): the motor drives TOWARD an angle and holds there, which is
        // what a self-closing door or a powered hatch wants. Velocity mode would spin forever.
        hinge->SetMotorState(EMotorState::Position);
        hinge->SetTargetAngle(DegreesToRadians(targetAngleDeg));
        MotorSettings& ms = hinge->GetMotorSettings();
        ms.SetTorqueLimit(maxTorque);
    } else {
        hinge->SetMotorState(EMotorState::Off);
    }

    // WAKE BOTH BODIES. A dynamic body that has settled goes to sleep, and a sleeping body is not
    // solved -- so setting a motor on a hatch that is sitting shut changes the constraint and then
    // nothing happens at all, which looks exactly like the motor being ignored. Jolt does not treat
    // "someone reconfigured a constraint attached to me" as a wake condition; that is the caller's job.
    m_impl->bi->ActivateBody(hinge->GetBody1()->GetID());
    m_impl->bi->ActivateBody(hinge->GetBody2()->GetID());
}

float Physics3D::GetHingeAngle(ConstraintHandle h) const {
    if (!m_impl || h >= m_impl->constraints.size() || !m_impl->constraints[h].active) return 0.0f;
    const Constraint* base = m_impl->constraints[h].c.GetPtr();   // GetSubType, not dynamic_cast (see SetHingeMotor)
    if (!base || base->GetSubType() != EConstraintSubType::Hinge) return 0.0f;
    return RadiansToDegrees(static_cast<const HingeConstraint*>(base)->GetCurrentAngle());
}

Physics3D::ConstraintHandle Physics3D::AddPointConstraint(BodyHandle bodyA, BodyHandle bodyB,
                                                          DirectX::XMFLOAT3 pivotWorld,
                                                          bool disableCollision) {
    if (!m_impl) return kInvalidConstraint;
    Impl& p = *m_impl;
    if (bodyA >= p.bodies.size() || !p.bodies[bodyA].active) return kInvalidConstraint;
    if (bodyB >= p.bodies.size() || !p.bodies[bodyB].active) return kInvalidConstraint;

    const BodyID ids[2] = { p.bodies[bodyA].id, p.bodies[bodyB].id };
    LockedPair lp(p.physics_system.GetBodyLockInterface(), ids);
    if (!lp.valid()) return kInvalidConstraint;

    PointConstraintSettings s;
    s.mSpace = EConstraintSpace::WorldSpace;
    s.mPoint1 = s.mPoint2 = RVec3(pivotWorld.x, pivotWorld.y, pivotWorld.z);

    Ref<Constraint> c = s.Create(*lp.a, *lp.b);
    if (c == nullptr) return kInvalidConstraint;
    p.physics_system.AddConstraint(c);

    // Stop the two joined bodies colliding, unless the caller wants them to. Without this a door
    // hinged at its own edge sweeps its thickness through the frame and jams after a few degrees --
    // it looks exactly like a broken constraint, which is the trap this default exists to avoid.
    if (disableCollision) SetBodiesCollide(bodyA, bodyB, false);

    p.constraints.push_back(Impl::ConRec{ c, true });
    return p.constraints.size() - 1;
}

// ==================================== 2D / 2.5D mode (Plane2D) ====================================

void Physics3D::SetBodyPlane2D(BodyHandle h) {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return;
    Impl& p = *m_impl;
    BodyLockWrite lock(p.physics_system.GetBodyLockInterface(), p.bodies[h].id);
    if (!lock.Succeeded()) return;
    Body& body = lock.GetBody();
    if (body.IsStatic()) return;   // statics never move; nothing to restrict
    MotionProperties* mp = body.GetMotionProperties();
    if (!mp) return;
    // SetMassProperties is the only way to change allowed DOFs after creation -- Jolt recomputes the
    // inverse inertia tensor with the locked axes removed, which is what actually stops the body
    // tipping out of plane. Re-derive the mass properties from the shape and the CURRENT mass so this
    // doesn't silently discard an AddDynamicBox massKg override.
    MassProperties mass = body.GetShape()->GetMassProperties();
    const float m = 1.0f / mp->GetInverseMass();
    mass.ScaleToMass(m);
    mp->SetMassProperties(EAllowedDOFs::Plane2D, mass);
}

void  Physics3D::SetPixelsPerMeter(float pixelsPerMeter) {
    if (m_impl && pixelsPerMeter > 0.0f) m_impl->pixelsPerMeter = pixelsPerMeter;
}
float Physics3D::GetPixelsPerMeter() const { return m_impl ? m_impl->pixelsPerMeter : 100.0f; }

void Physics3D::GetBody2D(BodyHandle h, DirectX::XMFLOAT2& outPosPx, float& outRotRadians) const {
    outPosPx = { 0.0f, 0.0f }; outRotRadians = 0.0f;
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return;
    const Impl& p = *m_impl;
    RVec3 pos = p.bi->GetPosition(p.bodies[h].id);
    Quat  rot = p.bi->GetRotation(p.bodies[h].id);
    outPosPx.x = (float)pos.GetX() * p.pixelsPerMeter;
    outPosPx.y = -(float)pos.GetY() * p.pixelsPerMeter;   // Y-up (physics) -> Y-down (screen)
    // Z angle out of the quaternion. For a Plane2D body only the Z component is ever non-zero, so
    // this is exact rather than an approximation.
    //
    // NOT negated. The Y-flip argument says physics +theta should be screen -theta, and that is true
    // for a raw coordinate mirror -- but Renderer2D's rotationDeg is already defined in its own
    // Y-down space, so it applies that reversal itself. Negating here too flips it twice and renders
    // every rotated body MIRRORED about horizontal.
    // That is invisible on anything symmetric, which is why it survived: the only rotated body in the
    // 2D scene was a see-saw, and a see-saw tilted the wrong way still looks like a see-saw. What gave
    // it away was collision -- bodies landed on the (correct) physics plank while the drawn plank was
    // somewhere else, so they appeared to collide with thin air and fall through the visible surface.
    outRotRadians = std::atan2(2.0f * (rot.GetW() * rot.GetZ() + rot.GetX() * rot.GetY()),
                               1.0f - 2.0f * (rot.GetY() * rot.GetY() + rot.GetZ() * rot.GetZ()));
}

void Physics3D::SetBody2D(BodyHandle h, DirectX::XMFLOAT2 posPx, float rotRadians) {
    if (!m_impl || h >= m_impl->bodies.size() || !m_impl->bodies[h].active) return;
    Impl& p = *m_impl;
    const float inv = 1.0f / p.pixelsPerMeter;
    p.bi->SetPositionAndRotation(p.bodies[h].id,
        RVec3(posPx.x * inv, -posPx.y * inv, 0.0f),          // screen Y-down -> physics Y-up
        Quat::sRotation(Vec3::sAxisZ(), rotRadians),         // must mirror GetBody2D's sign exactly
        EActivation::Activate);
}

void Physics3D::SetGravity2D(float pixelsPerSecSquaredDown) {
    if (!m_impl) return;
    // Positive input = downward on screen, matching PlatformerPhysics2D's ~900 px/s^2 convention.
    // Negative in physics space because that axis points up.
    const float mps2 = pixelsPerSecSquaredDown / m_impl->pixelsPerMeter;
    m_impl->physics_system.SetGravity(Vec3(0.0f, -mps2, 0.0f));
}

void Physics3D::SetBodiesCollide(BodyHandle a, BodyHandle b, bool collide) {
    if (!m_impl) return;
    Impl& p = *m_impl;
    if (a >= p.bodies.size() || b >= p.bodies.size() || a == b) return;
    // Sub-group IDs ARE the body handles (see add()), so the pair key needs no lookup.
    const uint64_t key = PairFilter::Key((CollisionGroup::SubGroupID)a, (CollisionGroup::SubGroupID)b);
    if (collide) p.pairFilter->disabled.erase(key);
    else         p.pairFilter->disabled.insert(key);
    // NOT thread-safe against a running step -- see PairFilter's threading note. Authoring only.
}

void Physics3D::RemoveConstraint(ConstraintHandle h) {
    if (!m_impl || h >= m_impl->constraints.size() || !m_impl->constraints[h].active) return;
    Impl::ConRec& rec = m_impl->constraints[h];
    m_impl->physics_system.RemoveConstraint(rec.c);   // must come off the system BEFORE we drop our ref
    rec.c = nullptr;
    rec.active = false;   // slot stays inert so other handles (indices) don't shift
}


// ============================ Static triangle-mesh collision (level geometry) ============================

Physics3D::BodyHandle Physics3D::AddStaticMesh(const DirectX::XMFLOAT3* vertices, std::size_t vertexCount,
                                               const uint32_t* indices, std::size_t indexCount,
                                               DirectX::XMFLOAT3 position, float scale) {
    if (!m_impl || !vertices || !indices || vertexCount == 0 || indexCount < 3) return kInvalidBody;

    // Jolt wants its own containers: a flat vertex list plus INDEXED triangles (not a soup of
    // duplicated positions), which is also what keeps the built BVH compact.
    VertexList jverts;
    jverts.reserve((JPH::uint)vertexCount);
    for (std::size_t i = 0; i < vertexCount; ++i)
        jverts.push_back(Float3(vertices[i].x, vertices[i].y, vertices[i].z));

    IndexedTriangleList jtris;
    const std::size_t triCount = indexCount / 3;
    jtris.reserve((JPH::uint)triCount);
    for (std::size_t t = 0; t < triCount; ++t) {
        const uint32_t i0 = indices[t * 3 + 0], i1 = indices[t * 3 + 1], i2 = indices[t * 3 + 2];
        // Skip anything referencing out-of-range vertices rather than letting Jolt assert on it --
        // downloaded meshes are not always well-formed.
        if (i0 >= vertexCount || i1 >= vertexCount || i2 >= vertexCount) continue;
        jtris.push_back(IndexedTriangle(i0, i1, i2, 0));
    }
    if (jtris.empty()) return kInvalidBody;

    MeshShapeSettings settings(jverts, jtris);
    // Drops degenerate and duplicate triangles. Real-world meshes (especially exported ones) are
    // full of zero-area slivers, and they produce phantom collisions and wasted BVH nodes.
    settings.Sanitize();

    ShapeSettings::ShapeResult result = settings.Create();
    if (result.HasError()) {
        char buf[256];
        snprintf(buf, sizeof(buf), "[Physics3D] AddStaticMesh failed: %s\n", result.GetError().c_str());
        OutputDebugStringA(buf);
        return kInvalidBody;
    }
    ShapeRefC shape = result.Get();

    // Uniform scale is baked into the shape rather than applied to the body: Jolt bodies carry no
    // scale of their own, so a ScaledShape is how a mesh gets resized at all.
    if (scale != 1.0f && scale > 0.0f) {
        ShapeSettings::ShapeResult scaled = ScaledShapeSettings(shape, Vec3::sReplicate(scale)).Create();
        if (!scaled.HasError()) shape = scaled.Get();
    }

    // A mesh has no meaningful render half-extent (it isn't a box), so report zeros -- GetBody's
    // caller draws level geometry from its own render mesh, not from a physics-derived size.
    return m_impl->add(shape, position, EMotionType::Static, Layers::NON_MOVING, { 0.0f, 0.0f, 0.0f });
}

} // namespace JLib


