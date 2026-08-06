#pragma once
// CharacterScene -- the character-controller test bed. Deliberately a course rather than a game:
// flat ground, a walkable ramp, a staircase, a ramp too steep to climb, and loose crates to shove.
// Each obstacle isolates one CharacterVirtual behavior, so a regression shows up as "I can't climb
// the stairs any more" instead of a vague feel change:
//
//   ramp (25 deg)     -> walks up smoothly, no hopping on the way down (StickToFloor)
//   staircase         -> steps up without jumping (WalkStairs), and stepHeight is the limit
//   steep ramp (65)   -> refuses to climb, ground state reads Sliding (maxSlopeAngle)
//   crates            -> character pushes them; they never launch the character (virtual = one-way)
//
// Third-person follow camera, WASD/stick to move, SPACE/A to jump. The HUD prints ground state and
// speed, which is what you actually watch when tuning.
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Renderer3D.h>
#include <Primitives3D.h>
#include <ModelLoader.h>   // LoadGlbModel (render) + LoadMeshGeometry (collision)
#include <InputManager.h>
#include <Font.h>
#include "Physics3D.h"
#include "Scene.h"

class CharacterScene : public Scene
{
public:
	CharacterScene(JLib::Font* font, JLib::Renderer2D& r2d, JLib::ResourceManager& rm, JLib::Renderer3D& r3d,
	               std::shared_ptr<JLib::InputManager> input, unsigned int width, unsigned int height);

	void Update(bool& isRunning, float dt) override;
	void Draw() override;
	void HandleInput(float dt) override;

private:
	void BuildCourse();

	JLib::Font* font;
	JLib::Renderer2D& r2d;
	JLib::ResourceManager& rm;
	JLib::Renderer3D& r3d;
	std::shared_ptr<JLib::InputManager> input;
	float screenW, screenH;

	// One cube mesh per course-piece TINT. Renderer3D::Submit takes no color, so the tint has to
	// live on the material -- these are copies of the same cube (sharing its vertex/index buffers)
	// that differ only in Material::baseColorFactor, which multiplies the wood texture.
	JLib::Mesh boxMesh;        // untinted -- crates
	JLib::Mesh groundMesh;
	JLib::Mesh rampMesh;       // blue   -- walkable slope
	JLib::Mesh stairMesh;      // tan    -- auto-step
	JLib::Mesh ledgeMesh;      // orange -- must jump
	JLib::Mesh steepMesh;      // red    -- too steep to climb
	JLib::Mesh capsuleMesh;    // the character itself
	JLib::TextureHandle woodTex;

	JLib::Physics3D physics3d;
	JLib::Physics3D::CharacterHandle player = JLib::Physics3D::kInvalidCharacter;

	// Static course pieces, kept so Draw can render exactly what physics collides with -- a mismatch
	// between the two is the classic "invisible wall" bug, so they share one list by construction.
	struct Piece {
		JLib::Physics3D::BodyHandle handle;
		DirectX::XMFLOAT3 center, halfExtents;
		const JLib::Mesh* mesh = nullptr;   // which tinted cube draws it
	};
	std::vector<Piece> pieces;
	std::vector<JLib::Physics3D::BodyHandle> crates;

	// Camera: orbits behind the character at a fixed offset, easing toward the target so it doesn't
	// snap on every direction change. Mouse orbits the yaw.
	float camYaw = 0.0f;
	DirectX::XMFLOAT3 camLookAt = { 0.0f, 1.0f, 0.0f };
	static constexpr float kCamDistance = 8.0f;
	static constexpr float kCamHeight   = 3.5f;
	static constexpr float kCamEase     = 8.0f;    // 1/s

	// Movement tuning. Air control is deliberately weaker than ground control -- full authority in
	// the air feels floaty and makes the jump arcs meaningless.
	static constexpr float kMoveSpeed   = 6.0f;    // m/s
	static constexpr float kJumpSpeed   = 6.5f;    // m/s (about 2.1m of rise at -9.8)
	static constexpr float kAirControl  = 0.35f;   // fraction of ground authority while airborne
	static constexpr float kMouseSens   = 0.0035f;

	bool quitRequested = false;
	int casterMode = 0;   // L cycles: 0 = sun (directional), 1 = lamp (spot), 2 = orb (point, 6 passes)

	// Optional real level geometry (Sponza). If the asset is present it REPLACES the box course:
	// rendered as one Mesh per material and collided as a single Jolt triangle mesh. Falls back to
	// the hand-built course if the file isn't there, so the scene always runs.
	std::vector<JLib::Mesh> levelMeshes;
	bool levelLoaded = false;
	static constexpr float kLevelScale = 1.0f;   // node transforms already carry Sponza to metre scale
	// Spawn: middle of Sponza's open atrium floor. The level spans about x -15..14, z -9..9 after
	// its root node scale, so the old (0,2,-6) sat inside the colonnade at the far end.
	static constexpr DirectX::XMFLOAT3 kSpawnPos = { 0.0f, 3.0f, 0.0f };

	// Character dimensions, in ONE place because the physics capsule and the drawn capsule have to
	// agree -- a visual that doesn't match the collider is how you get a character that looks like
	// it's floating or clipping when it's actually standing correctly.
	static constexpr float kCharRadius  = 0.3f;    // capsule radius
	static constexpr float kCharCylHalf = 0.6f;    // cylinder half-height -> 1.8m total, human-ish
	// MakeCapsuleMesh's own built-in dimensions (Primitives3D.h): R = 0.3, cylHalf = 0.35. The draw
	// scale is derived from these so changing the character size above stays correct automatically.
	static constexpr float kMeshRadius  = 0.3f;
	static constexpr float kMeshCylHalf = 0.35f;

	// A GPU particle fountain, purely decorative -- it also happens to be a nice sanity check that
	// the compute path and the shadow passes coexist without fighting over the frame.
	JLib::Renderer3D::ParticleEmitterHandle fountain = 0;
	static constexpr DirectX::XMFLOAT3 kFountainPos = { -4.0f, 0.2f, 0.0f };
};
