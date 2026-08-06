#pragma once
// SlingshotScene -- the FIRST 3D GAME: a static-camera slingshot (Angry-Birds-like). Deliberately a NEW
// scene, separate from StartMenuScene (which stays the renderer demo/test bench): no character, no menu,
// no stress grid. Fixed camera behind the slingshot; aim with arrows; hold SPACE to charge, release to
// fire a sphere along a BALLISTIC ARC (previewed as dots); knock the box tower down. Impacts fire the
// contact->burst particle chain (phase 2A). Primitives only -- zero art dependencies beyond wood.png.
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Renderer3D.h>
#include <Primitives3D.h>
#include <InputManager.h>
#include <Font.h>
#include <SoundManager.h>
#include "Physics3D.h"
#include "Scene.h"

class SlingshotScene : public Scene
{
public:
	// `sound` is a raw, NON-OWNING pointer -- main.cpp owns the SoundManager for the whole program
	// (outlives every scene), same contract as `font`. May be null (audio simply disabled).
	SlingshotScene(JLib::Font* font, JLib::SoundManager* sound, JLib::Renderer2D& r2d, JLib::ResourceManager& rm,
	               JLib::Renderer3D& r3d, std::shared_ptr<JLib::InputManager> input,
	               unsigned int width, unsigned int height);

	void Update(bool& isRunning, float dt) override;
	void Draw() override;
	void HandleInput(float dt) override;

private:
	void BuildTower();     // (re)spawns the target tower; also used by reset
	void ResetGame();      // removes projectiles + tower, respawns tower, restores shots
	DirectX::XMFLOAT3 AimDir() const;   // unit fire direction from aimYaw/aimPitch (yaw 0 = +Z, toward the tower)

	JLib::Font* font;
	JLib::Renderer2D& r2d;
	JLib::ResourceManager& rm;
	JLib::Renderer3D& r3d;
	std::shared_ptr<JLib::InputManager> input;
	float screenW, screenH;

	// Render meshes (all unit-sized primitives; scaled per draw).
	JLib::Mesh floorMesh;     // visual ground slab
	JLib::Mesh boxMesh;       // tower blocks
	JLib::Mesh sphereMesh;    // projectiles + arc dots + slingshot pouch
	JLib::TextureHandle woodTex;
	JLib::SoundHandle music;
	JLib::SoundManager* sound = nullptr;   // non-owning; ALWAYS initialized (a raw uninitialized
	                                       // pointer here previously "deadlocked": PlayLoop locked
	                                       // m_VoicesMutex through a garbage `this` and never returned)

	// Physics + game objects. Targets remember their spawn position for topple detection.
	JLib::Physics3D physics3d;
	struct Target     { JLib::Physics3D::BodyHandle handle; DirectX::XMFLOAT3 spawnPos; bool toppled = false; bool directHit = false; };
	struct Projectile { JLib::Physics3D::BodyHandle handle; bool gateCredited = false; };
	std::vector<Target>     targets;
	std::vector<Projectile> projectiles;
	JLib::Physics3D::BodyHandle gateSensor = 0;   // floating bonus ring (sensor: fly the shot through it)

	// User-data tags: contacts identify the ENTITIES involved (ContactEvent.userA/userB), not just where.
	// Blocks are 1..N (index+1), the gate is kUserGate, projectiles are kUserProjectile+shotIndex.
	static constexpr uint64_t kUserGate       = 500;
	static constexpr uint64_t kUserProjectile = 1000;
	static bool     IsBlockUD(uint64_t u)      { return u >= 1 && u < kUserGate; }
	static bool     IsProjectileUD(uint64_t u) { return u >= kUserProjectile; }

	// Scoring: +100 per toppled block, +50 the first time the PROJECTILE ITSELF strikes a block (direct
	// hit -- distinguishable from block-on-block tumble ONLY because of user data), +25 gate bonus per shot.
	int score = 0;
	static constexpr int kScoreTopple    = 100;
	static constexpr int kScoreDirectHit = 50;
	static constexpr int kScoreGate      = 25;
	static constexpr DirectX::XMFLOAT3 kGatePos = { 0.0f, 2.9f, 7.0f };   // on the natural mid-power arc

	// Impact particles (phase 2A chain).
	JLib::Renderer3D::ParticleEmitterHandle burstPool = 0;
	std::vector<JLib::Physics3D::ContactEvent> contactEvents;

	// Aim/charge state. Power charges while SPACE is held and fires on release.
	// Tuned (playtest 1): min power lands SHORT of the tower, ~mid-charge is on target, full charge just
	// overs -- so charge time is the skill. Old 8..26 @14/s meant even a TAP hit dead center and the whole
	// band overshot. Pitch default lowered for a flatter, readable arc.
	float aimYaw    = 0.0f;    // radians around +Y; 0 = straight at the tower (+Z)
	float aimPitch  = 0.40f;   // radians above horizontal
	float power     = 7.0f;    // launch speed; kPowerMin..kPowerMax
	bool  charging  = false;
	static constexpr float kPowerMin  = 7.0f;
	static constexpr float kPowerMax  = 16.0f;
	static constexpr float kChargeRate = 6.0f;    // power/sec while held (~1.5s tap-to-full)
	static constexpr float kProjectileMass = 800.0f;   // > a tower block (~512kg) so shots plow, not plink

	int  shotsLeft = 5;
	static constexpr int kShots = 5;

	// Mouse aim: GameInput's GetMouseDeltaX/Y is a CUMULATIVE accumulator, NOT a per-frame delta -- it
	// must be differenced against last frame (same documented gotcha + fix as the demo's camera look).
	float prevMouseAccX = 0.0f, prevMouseAccY = 0.0f;
	bool  mouseInit = false;
	static constexpr float kMouseSens = 0.0030f;   // radians per accumulator count

	bool quitRequested = false;
	static constexpr DirectX::XMFLOAT3 kSlingPos = { 0.0f, 1.6f, 0.0f };   // launch origin
};
