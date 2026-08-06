#include "pch.h"
#include "CharacterScene.h"
#include <Helpers.h>
#include <Colors.h>
#include <cmath>
#include <cstdio>

using namespace DirectX;

CharacterScene::CharacterScene(JLib::Font* font_, JLib::Renderer2D& r2d_, JLib::ResourceManager& rm_,
                               JLib::Renderer3D& r3d_, std::shared_ptr<JLib::InputManager> input_,
                               unsigned int width, unsigned int height)
	: font(font_), r2d(r2d_), rm(rm_), r3d(r3d_), input(std::move(input_)),
	  screenW((float)width), screenH((float)height)
{
	woodTex = rm.LoadTextureAsync(JLib::ExeRelative(L"textures\\wood.png"));
	boxMesh = JLib::MakeCubeMesh(rm);
	boxMesh.material.albedo = woodTex; boxMesh.material.roughness = 0.85f;

	// Tinted copies. Copying a Mesh copies its buffer VIEWS, not the buffers -- so these five all
	// draw the same cube geometry and differ only by baseColorFactor. Colour-coding the course is
	// the whole point of the scene: an obstacle's behaviour should be readable before you touch it.
	auto tinted = [&](DirectX::XMFLOAT4 c) { JLib::Mesh m = boxMesh; m.material.baseColorFactor = c; return m; };
	groundMesh = tinted({ 0.55f, 0.55f, 0.58f, 1.0f });
	rampMesh   = tinted({ 0.35f, 0.60f, 1.00f, 1.0f });   // blue   -- walk up
	stairMesh  = tinted({ 0.95f, 0.85f, 0.55f, 1.0f });   // tan    -- auto-step
	ledgeMesh  = tinted({ 1.00f, 0.55f, 0.15f, 1.0f });   // orange -- must jump
	steepMesh  = tinted({ 0.90f, 0.25f, 0.25f, 1.0f });   // red    -- too steep

	capsuleMesh = JLib::MakeCapsuleMesh(rm);
	capsuleMesh.material.albedo = woodTex; capsuleMesh.material.roughness = 0.5f;
	capsuleMesh.material.baseColorFactor = { 0.5f, 1.0f, 0.6f, 1.0f };   // green so the player reads instantly

	r3d.EnableShadows(true);
	// SSAO. Radius is in world units and is the real tuning knob: 0.5m is about the scale of the gaps
	// that matter here -- column bases meeting the floor, the springing of the arches, the recessed
	// gallery. Intensity above 1 because ambient is doing most of the lighting work indoors, so the
	// occlusion has to be visible against it.
	r3d.EnableSSAO(true);
	r3d.SetSSAOParams(0.5f, 1.4f);

	// Image-based lighting from a real HDR sky. When this succeeds it REPLACES the hemisphere ambient
	// set above -- ambient then comes from the environment's actual irradiance rather than two picked
	// colours. If the file is missing the call fails harmlessly and the hemisphere path stays, so the
	// scene still runs for anyone who doesn't have the asset.
	r3d.LoadEnvironment(L"textures\\citrus_orchard_puresky_2k.hdr");   // opt-in: costs a second geometry traversal into the depth map

	// Decorative GPU fountain near the ramp. Particles are simulated on the compute queue and drawn
	// as billboards -- they neither cast nor receive shadows (they aren't in m_Batches, which is what
	// the depth pass replays), so this is also a check that the compute path and the new shadow
	// passes share a frame without stepping on each other.
	JLib::Renderer3D::ParticleEmitterDesc fd;
	fd.position = kFountainPos;
	fd.spread   = 0.35f;    // fairly tight jet -- a wide cone reads as fog rather than a fountain
	fd.speed    = 7.5f;     // ~2.8m of rise against -9.8 gravity
	fd.gravity  = -9.8f;
	fd.lifetime = 2.4f;
	fd.size     = 0.06f;
	fd.color    = { 0.45f, 0.75f, 1.0f, 1.0f };
	fd.count    = 6000;
	//fountain = r3d.AddParticleEmitter(fd);

	physics3d.Init();

	// Try real level geometry first. Render and collision are loaded SEPARATELY and deliberately:
	// LoadGlbModel uploads per-material meshes to the GPU, LoadMeshGeometry does a CPU-side read for
	// the collision mesh -- a loaded Mesh keeps nothing on the CPU, so there is no way to derive one
	// from the other.
	{
		const std::string lvl = JLib::ExeRelativeA("models\\Sponza\\glTF\\Sponza.gltf");
		levelMeshes = JLib::LoadGlbModel(lvl, rm);
		std::vector<XMFLOAT3> pos; std::vector<uint32_t> idx;
		if (!levelMeshes.empty() && JLib::LoadMeshGeometry(lvl, pos, idx)) {
			physics3d.AddStaticMesh(pos.data(), pos.size(), idx.data(), idx.size(),
			                        { 0.0f, 0.0f, 0.0f }, kLevelScale);
			levelLoaded = true;
			char buf[160];
			snprintf(buf, sizeof(buf), "[CharacterScene] level: %zu meshes, %zu tris collision\n",
			         levelMeshes.size(), idx.size() / 3);
			OutputDebugStringA(buf);
		}
	}
	if (!levelLoaded) BuildCourse();   // no asset -> the hand-built obstacle course

	// Character: radius 0.3, cylinder half 0.6 -> 1.8m tall overall, roughly human. stepHeight 0.35
	// is a little over the 0.3 stair rise below, so stairs are climbable and the 0.5 ledge is not --
	// which is the point of having both.
	// stepHeight 0.45 against 0.3 stair rises -- see BuildCourse for why the margin matters.
	// maxSlopeAngle 40, not 45. On CURVED architecture (Sponza's arches) a surface sweeps continuously
	// from floor to wall, so somewhere along it the slope passes just under the limit and Jolt calls
	// that flank "walkable" -- the character jumps into an arch and stands on its side, pinned, which
	// is correct behaviour for a character controller (you don't slide down walkable slopes) but reads
	// as a bug. Shrinking the limit shrinks that band. Below ~35 legitimate ramps stop being climbable.
	player = physics3d.AddCharacter(kSpawnPos, kCharRadius, kCharCylHalf, 40.0f, 0.45f);

	physics3d.Finalize();
}

void CharacterScene::BuildCourse() {
	auto addPiece = [&](XMFLOAT3 c, XMFLOAT3 he, const JLib::Mesh& mesh) {
		auto h = physics3d.AddStaticBox(c, he);
		pieces.push_back({ h, c, he, &mesh });
	};

	// Ground.
	addPiece({ 0.0f, -0.5f, 0.0f }, { 30.0f, 0.5f, 30.0f }, groundMesh);

	// Walkable ramp, built as a short staircase of thin slabs rather than one rotated box: static
	// bodies here are AABBs, so a genuinely rotated ramp would collide as its bounding box and the
	// character would walk on an invisible flat lid. Stepping it keeps physics and visuals identical.
	for (int i = 0; i < 10; ++i) {
		float y = 0.15f * (float)i;
		addPiece({ -8.0f, y, -2.0f + (float)i * 0.9f }, { 2.0f, 0.15f, 0.45f }, rampMesh);
	}

	// Staircase: 0.3 rises against the character's 0.45 stepHeight. That 50% margin is deliberate --
	// stepHeight is a CAST DISTANCE, not a threshold, so a rise that merely fits under it climbs
	// unreliably. Treads are 1.0 deep, well over the 0.3 capsule radius, so there's real floor to
	// land on: BOTH conditions have to hold or stairs feel broken no matter how you tune the other.
	for (int i = 0; i < 6; ++i) {
		float y = 0.3f * (float)i;
		addPiece({ 0.0f, y, 3.0f + (float)i * 1.0f }, { 2.0f, 0.15f, 0.5f }, stairMesh);
	}

	// A single 0.5 ledge -- ABOVE stepHeight, so the character must jump it. The contrast with the
	// staircase is what proves stepHeight is actually doing something.
	addPiece({ 5.0f, 0.25f, 2.0f }, { 1.5f, 0.25f, 1.5f }, ledgeMesh);

	// Too-steep slope: 0.4 rises (over stepHeight) on treads 0.8 deep -- WIDE enough to stand on, so
	// this tests slope REFUSAL only. An earlier version used 0.19 treads, narrower than the capsule
	// radius, which meant it was also secretly testing "there is no floor here" -- two failures at
	// once, and impossible to tell apart when it misbehaves.
	for (int i = 0; i < 8; ++i) {
		float y = 0.4f * (float)i;
		addPiece({ 9.0f, y, -2.0f + (float)i * 0.8f }, { 2.0f, 0.2f, 0.4f }, steepMesh);
	}

	// Loose crates: walking into these pushes them, and they must never shove the character back --
	// that one-way relationship is the whole reason a player is a CharacterVirtual and not a body.
	for (int i = 0; i < 5; ++i) {
		auto h = physics3d.AddDynamicBox({ -3.0f + (float)i * 1.2f, 0.4f, 8.0f }, { 0.4f, 0.4f, 0.4f });
		crates.push_back(h);
	}
}

void CharacterScene::HandleInput(float dt) {
	using IM = JLib::InputManager;
	const bool padOn = input->IsGamepadConnected(0);

	if (input->IsKeyPressed(VK_ESCAPE) || (padOn && input->IsButtonPressed(IM::GamepadMenu)))
		quitRequested = true;

	// Mouse (and right stick) orbit the camera. True per-frame delta -- no accumulator differencing.
	camYaw += input->GetMouseDeltaX() * kMouseSens;
	if (padOn) camYaw += input->GetRightStickX(0) * 2.5f * dt;

	// Movement is relative to where the CAMERA looks, which is what every third-person game does --
	// "forward" means "away from the camera", not "world +Z".
	float fwd = 0.0f, strafe = 0.0f;
	if (input->IsKeyDown('W')) fwd += 1.0f;
	if (input->IsKeyDown('S')) fwd -= 1.0f;
	if (input->IsKeyDown('D')) strafe += 1.0f;
	if (input->IsKeyDown('A')) strafe -= 1.0f;
	if (padOn) {
		fwd    += input->GetLeftStickY(0);
		strafe += input->GetLeftStickX(0);
	}
	// Normalize so diagonals aren't faster than cardinals (the classic bug).
	float mag = sqrtf(fwd * fwd + strafe * strafe);
	if (mag > 1.0f) { fwd /= mag; strafe /= mag; }

	const float s = sinf(camYaw), c = cosf(camYaw);
	XMFLOAT3 wish{ (fwd * s + strafe * c) * kMoveSpeed, 0.0f, (fwd * c - strafe * s) * kMoveSpeed };

	// Preserve the controller's own vertical velocity -- gravity and jumps live there, and
	// overwriting Y every frame would cancel both.
	XMFLOAT3 v = physics3d.GetCharacterVelocity(player);
	const bool grounded = physics3d.GetCharacterGroundState(player) == JLib::Physics3D::GroundState::Grounded;
	if (grounded) {
		v.x = wish.x; v.z = wish.z;
	} else {
		// Air control: nudge toward the wish velocity instead of setting it, so a jump keeps its arc.
		v.x += (wish.x - v.x) * kAirControl * (dt * 10.0f);
		v.z += (wish.z - v.z) * kAirControl * (dt * 10.0f);
	}
	physics3d.SetCharacterVelocity(player, v);

	if (input->IsKeyPressed(VK_SPACE) || (padOn && input->IsButtonPressed(IM::GamepadA)))
		physics3d.CharacterJump(player, kJumpSpeed);   // no-ops unless grounded

	if (input->IsKeyPressed('R'))
		physics3d.SetCharacterPosition(player, kSpawnPos);
	if (input->IsKeyPressed('L')) casterMode = (casterMode + 1) % 3;   // cycle shadow caster
}

void CharacterScene::Update(bool& isRunning, float dt) {
	HandleInput(dt);
	if (quitRequested) { isRunning = false; return; }

	physics3d.Update(dt);
	physics3d.UpdateCharacters(dt);   // AFTER the world step -- see Physics3D.h

	// Fell off the course: respawn rather than falling forever.
	XMFLOAT3 p = physics3d.GetCharacterPosition(player);
	if (p.y < -20.0f) physics3d.SetCharacterPosition(player, kSpawnPos);

	// Camera target eases toward the character so it trails rather than snapping.
	float t = dt * kCamEase; if (t > 1.0f) t = 1.0f;
	camLookAt.x += (p.x - camLookAt.x) * t;
	camLookAt.y += ((p.y + 1.0f) - camLookAt.y) * t;
	camLookAt.z += (p.z - camLookAt.z) * t;
}

void CharacterScene::Draw() {
	auto screenSize = r2d.GetScreenSize();
	float aspect = (screenSize.y > 0.0f) ? screenSize.x / screenSize.y : 1.0f;

	// Orbit camera behind the character, at camYaw.
	XMVECTOR target = XMVectorSet(camLookAt.x, camLookAt.y, camLookAt.z, 1.0f);
	XMVECTOR eye = XMVectorSet(camLookAt.x - sinf(camYaw) * kCamDistance,
	                           camLookAt.y + kCamHeight,
	                           camLookAt.z - cosf(camYaw) * kCamDistance, 1.0f);
	XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0));
	XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 300.0f);
	XMFLOAT3 camPos; XMStoreFloat3(&camPos, eye);
	r3d.SetCamera(view * proj, camPos);

	// Ambient is MUCH higher indoors. Sponza is an enclosed atrium, so a directional sun is blocked by
	// the roof and upper galleries and the ground floor is legitimately in shadow -- which without any
	// global illumination means nearly black. Real engines fill that with GI or baked lightmaps; with
	// neither, ambient is the stand-in, and starving it is what made the scene unreadable.
	// The slight blue tint reads as skylight bouncing in rather than flat grey.
	// HEMISPHERE ambient, not a flat constant. Sponza is open to the sky, so upward-facing surfaces
	// (the floor, the tops of the ledges) genuinely receive cool skylight, while soffits, arch
	// undersides and the galleries only ever see light kicked back off warm stone. A single constant
	// gave all three the same fill, which is exactly why the interior read flat no matter how the
	// shadows were tuned -- orientation, not intensity, was the missing cue.
	if (levelLoaded)
		r3d.SetHemisphereAmbient({ 0.40f, 0.45f, 0.56f },    // sky: cool, open roof
		                         { 0.26f, 0.22f, 0.17f });   // ground: warm stone bounce
	else
		r3d.SetHemisphereAmbient({ 0.16f, 0.16f, 0.20f }, { 0.07f, 0.06f, 0.05f });
	r3d.ClearLights();
	// Steeper sun so more of it reaches the floor through the open roof, and warmer/stronger to read
	// as sunlight against the cool ambient.
	r3d.AddDirectionalLight({ -0.25f, -0.94f, 0.22f }, { 1.0f, 0.95f, 0.85f }, 4.0f);
	// (The second UNSHADOWED directional that used to sit here as "bounce fill from below" is gone --
	// faking floor bounce with a light aimed upward is precisely what the hemisphere ground colour
	// now does properly, and doing both would double-count it. It also gives back a light slot and
	// one per-pixel loop iteration.)
	// A warm spot over the staircase. Press L to make IT the shadow caster instead of the sun --
	// the shadows swing round and converge toward the lamp, which is the visible difference between
	// a perspective caster and a parallel one.
	r3d.AddSpotLight({ 0.0f, 7.0f, 6.0f }, { 0.0f, -1.0f, 0.0f },
	                 { 1.0f, 0.85f, 0.55f }, 14.0f, 20.0f, 22.0f, 34.0f);
	// A point light among the crates. As a shadow caster it costs SIX depth passes (a cube map)
	// versus one for the others -- which is exactly what makes it the interesting comparison.
	r3d.AddPointLight({ -1.0f, 2.2f, 8.0f }, { 0.6f, 0.8f, 1.0f }, 12.0f, 16.0f);

	// L cycles the caster: sun (directional, 1 ortho pass) -> lamp (spot, 1 perspective pass) ->
	// orb (point, 6 cube passes). Point is never auto-picked; it has to be asked for by index.
	r3d.SetShadowCaster(casterMode == 1 ? 1 : (casterMode == 2 ? 2 : -1));

	// Shadows follow the player rather than covering the whole 60m ground slab: the map's resolution
	// is fixed, so a smaller box means more texels per metre and crisper contact shadows. 22 units
	// comfortably contains the course pieces around the character at any point on it.
	// Tighter box on real level geometry: the map's resolution is fixed, so halving the extent
	// doubles the texels per metre. 12 units around the player covers the surrounding columns and
	// arches -- anything further away renders unshadowed, which is far less noticeable than mushy
	// shadows everywhere.
	r3d.SetShadowBounds({ camLookAt.x, camLookAt.y, camLookAt.z }, levelLoaded ? 12.0f : 22.0f);

	// Course. Drawn straight from the same records physics was built from, so what you see is
	// exactly what you collide with.
	if (levelLoaded) {
		// One Submit per material group -- all sharing the same world transform, which must match the
		// scale handed to AddStaticMesh or the visuals and the collision drift apart.
		const XMMATRIX lvlXform = XMMatrixScaling(kLevelScale, kLevelScale, kLevelScale);
		for (const JLib::Mesh& m : levelMeshes) r3d.Submit(m, lvlXform);
	} else {
		for (const Piece& pc : pieces) {
			r3d.Submit(*pc.mesh, XMMatrixScaling(pc.halfExtents.x * 2.0f, pc.halfExtents.y * 2.0f,
			                                     pc.halfExtents.z * 2.0f)
			                     * XMMatrixTranslation(pc.center.x, pc.center.y, pc.center.z));
		}
	}

	// Crates (dynamic -- query their live transforms).
	for (auto h : crates) {
		XMFLOAT3 pos, he; XMFLOAT4 rot;
		physics3d.GetBody(h, pos, rot, he);
		XMMATRIX m = XMMatrixScaling(he.x * 2.0f, he.y * 2.0f, he.z * 2.0f)
		           * XMMatrixRotationQuaternion(XMVectorSet(rot.x, rot.y, rot.z, rot.w))
		           * XMMatrixTranslation(pos.x, pos.y, pos.z);
		r3d.Submit(boxMesh, m);
	}

	// The character. MakeCapsuleMesh is a unit capsule, so scale by the same radius/half-height the
	// controller was created with (0.3 / 0.6 -> 1.8m tall).
	// Draw the capsule at EXACTLY the collider's size. X/Z scale matches the radius; Y scale matches
	// total height (2*cylHalf + 2*radius) rather than the cylinder alone, so the silhouette's height
	// is right. That stretches the hemispherical caps slightly -- invisible in practice, and far
	// better than a visual whose feet sit above or below where collision actually is.
	XMFLOAT3 p = physics3d.GetCharacterPosition(player);
	constexpr float kMeshTotalH = 2.0f * (kMeshCylHalf + kMeshRadius);
	constexpr float kCharTotalH = 2.0f * (kCharCylHalf + kCharRadius);
	r3d.Submit(capsuleMesh, XMMatrixScaling(kCharRadius / kMeshRadius,
	                                        kCharTotalH / kMeshTotalH,
	                                        kCharRadius / kMeshRadius)
	                        * XMMatrixTranslation(p.x, p.y, p.z));

	// HUD: ground state and speed are what you actually watch while tuning feel.
	const char* stateStr = "InAir";
	switch (physics3d.GetCharacterGroundState(player)) {
		case JLib::Physics3D::GroundState::Grounded: stateStr = "Grounded"; break;
		case JLib::Physics3D::GroundState::Sliding:  stateStr = "Sliding";  break;
		default: break;
	}
	XMFLOAT3 v = physics3d.GetCharacterVelocity(player);
	float speed = sqrtf(v.x * v.x + v.z * v.z);

	char line[192];
	snprintf(line, sizeof(line), "%s   speed %.1f m/s   y %.2f", stateStr, speed, p.y);
	r2d.SubmitText(*font, 10.0f, 10.0f, line, 1.0f,
	               physics3d.GetCharacterGroundState(player) == JLib::Physics3D::GroundState::Sliding
	                   ? JLib::Colors::Orange : JLib::Colors::White);
	char l2[160];
	snprintf(l2, sizeof(l2), "WASD move  SPACE jump  MOUSE orbit  R respawn  L shadow caster [%s]  ESC quit",
	         casterMode == 1 ? "SPOT (1 pass)"
	                         : (casterMode == 2 ? "POINT (6 passes)" : "SUN (1 pass)"));
	r2d.SubmitText(*font, 10.0f, 34.0f, l2, 1.0f, JLib::Colors::Gray);
	r2d.SubmitText(*font, 10.0f, 58.0f,
	               "blue ramp: walk up | tan stairs: auto-step | orange ledge: must jump | red ramp: too steep",
	               0.85f, JLib::Colors::Gray);
}
