#include "pch.h"
#include "StartMenuScene.h"
#include <Helpers.h>
#include <cmath>
#include <SceneManager.h>
#include <memory>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <cstdio>

static std::ofstream g_debugLog("scene_debug.log", std::ios::app);
#define SCENE_LOG(fmt, ...) do { \
	char buf[512]; \
	snprintf(buf, sizeof(buf), fmt "\n", ##__VA_ARGS__); \
	g_debugLog << buf; g_debugLog.flush(); \
} while(0)

StartMenuScene::StartMenuScene(JLib::Font* font, bool& imguiEnabled, JLib::SoundManager* sound, JLib::Renderer2D& renderer, JLib::ResourceManager& resourceManager, JLib::Renderer3D& r3d, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
	JLib::Mesh* quadMesh, JLib::Mesh* slopeUpRightMesh, JLib::Mesh* slopeUpLeftMesh,
	JLib::TextureHandle tileTexture, uint32_t dustEffect, uint32_t width, uint32_t height, HWND windowHandle)
	: font(font)
	, imguiEnabled(imguiEnabled)
	, sound(sound)
	, resourceManager(resourceManager)
	, r2d(renderer)
	, r3d(r3d)
	, input(input)
	, camera(camera)
	, quadMesh(quadMesh)
	, slopeUpRightMesh(slopeUpRightMesh)
	, slopeUpLeftMesh(slopeUpLeftMesh)
	, tileTexture(tileTexture)
	, dustEffect(dustEffect)
	, width(width)
	, height(height)
	, windowHandle(windowHandle)
{
	music = sound->PlayLoop(JLib::ExeRelativeA("sound\\bounce_light_3.flac").c_str());
	if (!music.IsValid()) {
		throw("PlayLoop(\"bounce_light_3.flac\") failed to load -- put a real file next to the exe to test.\n");
	}
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		// LoadFromWICFile (inside LoadTexture) resolves a relative path against the process's
		// CURRENT WORKING DIRECTORY, not the exe's directory -- when launching from the VS
		// debugger those aren't the same, so a bare "textures\\coin.png" fails to resolve even
		// though the file is really sitting next to the exe, and LoadTexture's ThrowIfFailed
		// throws on that failure (the exact unhandled exception this crashed with). ExeRelative
		// anchors the path to the exe's own directory instead -- same fix already used for
		// fonts/sound elsewhere in this codebase.
		woodTex = resourceManager.LoadTexture(JLib::ExeRelative(L"textures\\wood.png"), cmd);
		});
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		// LoadFromWICFile (inside LoadTexture) resolves a relative path against the process's
		// CURRENT WORKING DIRECTORY, not the exe's directory -- when launching from the VS
		// debugger those aren't the same, so a bare "textures\\coin.png" fails to resolve even
		// though the file is really sitting next to the exe, and LoadTexture's ThrowIfFailed
		// throws on that failure (the exact unhandled exception this crashed with). ExeRelative
		// anchors the path to the exe's own directory instead -- same fix already used for
		// fonts/sound elsewhere in this codebase.
		wallTex = resourceManager.LoadTexture(JLib::ExeRelative(L"textures\\wall.png"), cmd);
		});
	// Scene-owned 3D test content (device is ready by now). teapot may fail to load (missing file) --
	// its Mesh then has a null vertexBuffer, which Draw() checks before submitting.
	cube3d = JLib::MakeCubeMesh(resourceManager);
	cube3d.material.albedo = woodTex;   // self-describing now -- set the material once, not per Submit
	// Batch-sweep variants: kMaxBatchVariants distinct cube meshes (identical geometry, but separate Mesh
	// objects -> separate (mesh,albedo) batches). The stress grid round-robins over the first `batchStress`
	// of them, so draw-call count == batchStress. LEFT/RIGHT adjust it; the B benchmark reads each level.
	cubeVariants.reserve(kMaxBatchVariants);
	for (int i = 0; i < kMaxBatchVariants; ++i) {
		JLib::Mesh m = JLib::MakeCubeMesh(resourceManager);
		m.material.albedo = woodTex;
		cubeVariants.push_back(m);
	}
	teapot = JLib::LoadObjMesh(JLib::ExeRelativeA("models\\teapot.obj"), resourceManager);
	// albedo now comes from teapot's OWN default.mtl (map_Kd default.png) -- LoadObjMesh loads it, no
	// hand-assignment needed. Kept metal so the teapot stays a PBR metal test subject.
	teapot.material.metallic  = 1.0f;
	teapot.material.roughness = 0.30f;
	// Static glTF test: CesiumMan.glb loads in bind pose (skinning/animation ignored). cgltf doesn't
	// decode the embedded texture, so assign an albedo by hand for now (same as the teapot).
	cesium = JLib::LoadGlbMesh(JLib::ExeRelativeA("models\\CesiumMan.glb"), resourceManager);
	cesium.material.albedo = woodTex;
	// Skinned load of the same asset, drawn via the skinning pipeline. With the identity bone palette
	// this must render IDENTICAL to the static cesium above -- the verification checkpoint.
	cesiumSkinned = JLib::LoadGlbSkinnedMesh(JLib::ExeRelativeA("models\\CesiumMan.glb"), resourceManager);
	//cesiumSkinned.mesh.material.albedo = tileTexture;
	// PBR material test: DamagedHelmet is THE reference glTF PBR model -- metallic/roughness live in a
	// TEXTURE (not scalars), plus emissive + occlusion maps. This is what the material-texture work is for.
	damagedHelmet = JLib::LoadGlbMesh(JLib::ExeRelativeA("models\\DamagedHelmet.glb"), resourceManager);

	// Walkable 3D world: a ground slab + a building (built from scaled cubes). wallTex earns its keep here
	// (it was freed when the teapot stopped hand-assigning it); the ground uses woodTex.
	floorMesh = JLib::MakeCubeMesh(resourceManager);
	floorMesh.material.albedo   = woodTex;
	floorMesh.material.metallic = 0.0f;
	floorMesh.material.roughness = 0.9f;   // matte floor
	wallMesh = JLib::MakeCubeMesh(resourceManager);
	wallMesh.material.albedo    = wallTex;
	wallMesh.material.metallic  = 0.0f;
	wallMesh.material.roughness = 0.85f;

	// Physics render meshes: boxes draw as the wood cube; the demo also drops a sphere and a capsule.
	sphere3d = JLib::MakeSphereMesh(resourceManager);
	sphere3d.material.albedo = woodTex;
	capsule3d = JLib::MakeCapsuleMesh(resourceManager);
	capsule3d.material.albedo = woodTex;
	cylinder3d = JLib::MakeCylinderMesh(resourceManager);
	cylinder3d.material.albedo = woodTex;
	pyramid3d = JLib::MakePyramidMesh(resourceManager);
	pyramid3d.material.albedo = woodTex;
	cone3d = JLib::MakeConeMesh(resourceManager);
	cone3d.material.albedo = woodTex;

	// Jolt physics: the SCENE authors its world now (Physics3D only simulates). A static floor to land on,
	// a cluster of dynamic boxes, plus one sphere + one capsule so the other collision shapes are visible.
	// For each dynamic body, store its handle + the mesh to draw it as + that mesh's unit half-size (so Draw
	// scales by bodyHalf/meshUnitHalf). The static floor isn't stored -- it's invisible collision; the scene
	// already draws its own visual ground slab (floorMesh) at the same height.
	physics3d.Init();
	physics3d.AddStaticBox({ 0.0f, -1.0f, 0.0f }, { 50.0f, 0.5f, 50.0f });   // floor top at y=-0.5 (matches the slab)
	for (int i = 0; i < 8; ++i) {
		float x = ((i % 4) - 1.5f) * 1.2f;
		float z = 4.0f + (float)(i / 4) * 1.2f;
		float y = 6.0f + (float)i * 1.0f;
		physBodies.push_back({ physics3d.AddDynamicBox({ x, y, z }, { 0.5f, 0.5f, 0.5f }), &cube3d, { 0.5f, 0.5f, 0.5f } });
	}
	physBodies.push_back({ physics3d.AddDynamicSphere({ 0.6f, 14.0f, 4.5f }, 0.5f),          &sphere3d,  { 0.5f, 0.5f, 0.5f } });
	physBodies.push_back({ physics3d.AddDynamicCapsule({ -0.6f, 15.0f, 4.5f }, 0.35f, 0.3f), &capsule3d, { 0.3f, 0.65f, 0.3f } });
	physBodies.push_back({ physics3d.AddDynamicCylinder({ 1.2f, 17.0f, 5.2f }, 0.5f, 0.4f), &cylinder3d, { 0.5f, 0.5f, 0.5f } });
	physBodies.push_back({ physics3d.AddDynamicPyramid({ 0.0f, 16.0f, 5.2f }, { 0.5f, 0.5f, 0.5f }), &pyramid3d, { 0.5f, 0.5f, 0.5f } });
	physBodies.push_back({ physics3d.AddDynamicCone({ -1.2f, 18.0f, 5.2f }, 0.5f, 0.5f), &cone3d, { 0.5f, 0.5f, 0.5f } });
	physics3d.Finalize();

	// 3D GPU particles (phase 1): a self-recycling fountain near the physics pile. Runs entirely on the GPU
	// (compute update + camera-facing billboards); the renderer drives it every frame after this one call.
	r3d.AddParticleEmitter({ { 0.0f, -0.5f, 4.5f }, 0.6f, 6.5f, -9.8f, 2.0f, 0.12f, { 0.5f, 0.85f, 1.0f, 1.0f }, 4096 });
	// Impact-burst pool (phase 2A): filled on demand from Jolt contact events in Update. 8192 slots is
	// generous headroom (bursts are ~24 particles x ~0.6s life; the ring only wraps over dead slots).
	burstPool = r3d.AddBurstPool(8192);
}

void StartMenuScene::Update(bool& isRunning, float dt)
{
	spin3d += dt;   // 3D spin advanced HERE (game state); Draw() reads it -- Update-before-Draw ordering.
	physics3d.Update(dt);   // step Jolt (falling boxes); Draw() reads the resulting body transforms

	// Impact effects: drain this step's NEW contacts and fire a particle burst at each hard one. speed is
	// the closing speed along the contact normal -- the >2 threshold skips gentle settling; count/size scale
	// mildly with impact so a box slamming down puffs bigger than a nudge. Capped per frame for sanity.
	contactEvents.clear();
	physics3d.DrainContactEvents(contactEvents);
	int burstsFired = 0;
	for (const auto& ce : contactEvents) {
		if (ce.speed < 2.0f) continue;
		if (++burstsFired > 8) break;
		JLib::Renderer3D::BurstDesc b;
		b.position   = ce.position;
		b.normal     = ce.normal;
		b.count      = ce.speed > 6.0f ? 40u : 24u;
		b.speed      = 2.0f + ce.speed * 0.25f;
		b.lifetime   = 0.6f;
		b.size       = 0.07f;
		b.normalBias = 0.6f;
		b.color      = { 0.95f, 0.8f, 0.45f, 1.0f };   // dusty spark
		r3d.RequestBurst3D(burstPool, b);
	}
	// (Startup landings burst during load-in stutter -- only a few frames render in the burst's 0.6s
	// life, so they're near-invisible; runtime impacts (F / slingshot shots) display normally.)

	// --- Third-person controls: mouse orbits the camera (yaw/pitch); WASD moves the CHARACTER relative to
	// the camera; Shift runs. The character faces its move direction and walk-animates only while moving.
	if (input) {
		using namespace DirectX;
		// GetMouseDeltaX/Y is really GameInput's CUMULATIVE accumulator -- difference it for the true
		// per-frame delta. Update prev EVERY frame (even when not looking) so pressing the look button
		// doesn't dump a jump from all the motion that happened while it was released.
		float accX = input->GetMouseDeltaX(), accY = input->GetMouseDeltaY();
		if (!mouseInit) { prevMouseAccX = accX; prevMouseAccY = accY; mouseInit = true; }
		float dx = accX - prevMouseAccX, dy = accY - prevMouseAccY;
		prevMouseAccX = accX; prevMouseAccY = accY;
		dx = std::max(-200.0f, std::min(200.0f, dx));   // clamp one-frame spikes (focus changes, etc.)
		dy = std::max(-200.0f, std::min(200.0f, dy));
		// Always-on mouse look (no button needed). dx/dy are the true per-frame delta computed above.
		const float sens = 0.0025f;
		camYaw   += dx * sens;
		camPitch -= dy * sens;                          // screen-y grows downward; invert so mouse-up looks up
		const float lim = 1.55f;                        // ~89deg; avoids the gimbal flip at straight up/down
		camPitch = std::max(-lim, std::min(lim, camPitch));

		// WASD moves the CHARACTER on the ground plane, relative to where the CAMERA faces (so W is always
		// "away from camera"). Movement uses only the yaw (horizontal) so you don't fly when looking up/down.
		float mf = 0.0f, ms = 0.0f;
		if (input->IsKeyDown('W')) mf += 1.0f;
		if (input->IsKeyDown('S')) mf -= 1.0f;
		if (input->IsKeyDown('D')) ms += 1.0f;
		if (input->IsKeyDown('A')) ms -= 1.0f;
		charMoving = (mf != 0.0f || ms != 0.0f);
		if (charMoving) {
			float sYaw = sinf(camYaw), cYaw = cosf(camYaw);
			XMVECTOR fwdH   = XMVectorSet(sYaw, 0.0f, cYaw, 0.0f);    // camera's horizontal forward
			XMVECTOR rightH = XMVectorSet(cYaw, 0.0f, -sYaw, 0.0f);   // camera's horizontal right
			XMVECTOR dir = XMVector3Normalize(XMVectorAdd(XMVectorScale(fwdH, mf), XMVectorScale(rightH, ms)));
			float sp = 4.0f * dt;
			if (input->IsKeyDown(0x10)) sp *= 2.0f;                   // 0x10 = VK_SHIFT (run)
			XMStoreFloat3(&charPos, XMVectorAdd(XMLoadFloat3(&charPos), XMVectorScale(dir, sp)));
			XMFLOAT3 d; XMStoreFloat3(&d, dir);
			charYaw = atan2f(d.x, d.z);   // face the move direction (add an offset here if the model faces "wrong")
		}
	}

	// Discrete-key commands (ESC quit, R reset, B benchmark, UP/DOWN speed, LEFT/RIGHT batches). HandleInput
	// was defined but NEVER called, so none of these keys did anything (WASD/mouse-look work only because
	// they're handled inline above). It has no isRunning& of its own, so ESC queues CMD::QUIT into cmdQ --
	// drain that here, where isRunning lives, to actually stop the game loop.
	if (input) HandleInput(dt);
	for (CMD c : cmdQ) if (c == CMD::QUIT) isRunning = false;
	cmdQ.clear();

	// Advance the CesiumMan walk clock ONLY while moving (charMoving from last frame) so the character
	// walks when you move and holds its pose when you stop. Loops by the clip's duration.
	if (charMoving && !cesiumSkinned.animations.empty()) {
		animTime += dt;
		float dur = cesiumSkinned.animations[0].duration;
		if (dur > 0.0f && animTime > dur) animTime = std::fmod(animTime, dur);
	}

	auto screenSize = r2d.GetScreenSize();

	// Debug: check screen size on first update
	if (cpuParticles.capacity == 0) {
		// Screen size should be valid before we allocate particles
		if (screenSize.x <= 0 || screenSize.y <= 0) {
			// Can't spawn particles without valid screen size
			return;
		}
	}

	// Spawn falling particles on a timer (much slower for debugging)
	spawnTimer += dt;
	if (spawnTimer >= spawnInterval && cpuParticles.count < (size_t)maxParticles) {
	//	SpawnParticles(spawnPerBurst);
		spawnTimer -= spawnInterval;
	}

	// Only update if we have particles
	if (cpuParticles.count > 0) {
		SCENE_LOG("[Update] count=%zu, calling UpdateCpuParticles", cpuParticles.count);

		// Update particles in parallel via scheduler
		UpdateCpuParticles(cpuParticles, dt);

		SCENE_LOG("[Update] UpdateCpuParticles done");

		// Remove particles that hit the bottom (recycle them)
		size_t i = 0;
		while (i < cpuParticles.count) {
			if (cpuParticles.posY[i] > screenSize.y) {
				KillParticle(cpuParticles, i);
				// Don't increment i here - KillParticle swapped the last particle into this slot
			} else {
				i++;
			}
		}
		SCENE_LOG("[Update] removal done, count=%zu", cpuParticles.count);
	}
}

void StartMenuScene::Draw()
{
	auto screenSize = r2d.GetScreenSize();

	// --- 3D test content (renders UNDER the 2D particles/HUD). This runs inside the sprites DAG
	// main-node: AFTER Update (so spin3d is this frame's value) and BEFORE PresentFrame reads it. ---
	{
		using namespace DirectX;
		float aspect = (screenSize.y > 0.0f) ? screenSize.x / screenSize.y : 1.0f;
		// Third-person orbit camera: look at the character's torso, sit camDist behind along the mouse-driven
		// yaw/pitch. camPos (the eye) is derived from charPos each frame, not moved directly.
		float cp = cosf(camPitch), sp = sinf(camPitch), sy = sinf(camYaw), cy = cosf(camYaw);
		XMVECTOR camFwd = XMVectorSet(cp * sy, sp, cp * cy, 0.0f);
		XMVECTOR target = XMVectorAdd(XMLoadFloat3(&charPos), XMVectorSet(0.0f, 1.2f, 0.0f, 0.0f)); // torso height
		XMVECTOR eye    = XMVectorSubtract(target, XMVectorScale(camFwd, camDist));                 // sit behind
		XMStoreFloat3(&camPos, eye);   // keep the member in sync (PBR PS view vector reads it via SetCamera)
		XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0.0f, 1.0f, 0.0f, 0.0f));
		XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(60.0f), aspect, 0.1f, 100.0f);
		r3d.SetCamera(view * proj, camPos);   // camPos feeds the PBR PS's view vector (specular)

		// --- The walkable world: a ground slab + a building you can enter (both scaled cubes). -----------
		// Ground: unit cube y-span [-0.5,0.5] scaled thin, translated so the TOP sits at y=-0.5 (unit
		// showcase objects centered at y=0 rest on it). 60x60 so there's room to roam.
		r3d.Submit(floorMesh, XMMatrixScaling(60.0f, 1.0f, 60.0f) * XMMatrixTranslation(0.0f, -1.0f, 0.0f));
		// Building at x=-12 (off to your LEFT from the start view), doorway facing +X toward the showcase.
		// Walls are 4 units tall (center y=1.5 -> span [-0.5,3.5], sitting on the ground); 0.3 thick.
		{
			const float H = 4.0f, T = 0.3f, cyw = 1.5f, Cx = -12.0f;   // Cz = 0
			auto wall = [&](float sx, float sy, float sz, float px, float py, float pz) {
				r3d.Submit(wallMesh, XMMatrixScaling(sx, sy, sz) * XMMatrixTranslation(px, py, pz));
			};
			wall(T, H, 6.3f, Cx - 3.0f, cyw,  0.0f);   // back wall (x=-15)
			wall(6.3f, H, T, Cx,        cyw, -3.0f);    // side wall (z=-3)
			wall(6.3f, H, T, Cx,        cyw,  3.0f);    // side wall (z=+3)
			wall(T, H, 2.0f, Cx + 3.0f, cyw, -2.0f);   // front-lower segment (doorway gap in the middle)
			wall(T, H, 2.0f, Cx + 3.0f, cyw,  2.0f);   // front-upper segment
			wall(T, 1.0f, 2.0f, Cx + 3.0f, 3.0f, 0.0f);// lintel above the doorway (y 2.5..3.5)
			wall(6.3f, T, 6.3f, Cx, 3.65f, 0.0f);      // roof (on top of the walls)
		}

		// Light rig, rebuilt each frame (cheap). A cool directional "key" from upper-front-left + a warm
		// point light for fill, over a dim ambient. `dir` is the direction light TRAVELS (PS negates it).
		r3d.SetAmbient({ 0.12f, 0.12f, 0.14f });   // stands in for env reflection; metals read as dark without it (no IBL yet)
		r3d.ClearLights();
		r3d.AddDirectionalLight({ -0.4f, -0.8f, 0.4f }, { 1.0f, 0.97f, 0.9f }, 3.0f);   // key (sun-ish)
		r3d.AddPointLight({ 2.0f, 1.5f, -1.0f }, { 1.0f, 0.5f, 0.2f }, 10.0f, 8.0f);     // warm fill

		// Cube (unit, at origin-ish, spinning). Offset left so the teapot fits beside it.
		// Albedo now travels WITH the mesh (mesh.material, set in the ctor) -- Submit is just geometry+transform.
		r3d.Submit(cube3d, XMMatrixRotationY(spin3d) * XMMatrixTranslation(-1.2f, 0.0f, 0.0f));

		// Teapot: LoadObjMesh normalizes it to a unit, origin-centered model, so it needs no per-file
		// nudge -- scale up to ~1.5 units to read next to the cube, place right. (Guard on vertexBuffer:
		// a failed load leaves it null, which would crash RecordCommandList.)
		if (teapot.vertexBuffer)
			r3d.Submit(teapot, XMMatrixScaling(1.5f, 1.5f, 1.5f) * XMMatrixRotationY(spin3d)
			                   * XMMatrixTranslation(1.2f, 0.0f, 0.0f));

		// DamagedHelmet -- the PBR material test. Ahead of the start position, elevated to eye level and
		// spinning so the per-texel metal/rough (and the glowing emissive damage) sweep across it. glTF
		// models come in Y-up already, so no stand-up rotation like CesiumMan needs.
		if (damagedHelmet.vertexBuffer)
			r3d.Submit(damagedHelmet, XMMatrixScaling(1.5f, 1.5f, 1.5f) * XMMatrixRotationY(spin3d)
			                          * XMMatrixTranslation(0.0f, 1.5f, 4.0f));

		// Jolt dynamic bodies -- draw each at its live physics transform using the mesh the scene paired with
		// it. Scale = bodyHalf / meshUnitHalf so non-unit meshes (the capsule) render at the right size.
		for (const PhysBody& pb : physBodies) {
			XMFLOAT3 bpos, bhalf; XMFLOAT4 brot;
			physics3d.GetBody(pb.handle, bpos, brot, bhalf);
			r3d.Submit(*pb.mesh, XMMatrixScaling(bhalf.x / pb.meshUnitHalf.x,
			                                     bhalf.y / pb.meshUnitHalf.y,
			                                     bhalf.z / pb.meshUnitHalf.z)
			                     * XMMatrixRotationQuaternion(XMLoadFloat4(&brot))
			                     * XMMatrixTranslation(bpos.x, bpos.y, bpos.z));
		}

		// CesiumMan: sample its animation clip at animTime into a bone palette, then draw skinned --
		// this is the ANIMATED character (walk cycle). An empty pose (no clip) falls back to bind pose.
		if (cesiumSkinned.mesh.vertexBuffer) {
			std::vector<XMFLOAT4X4> pose;
			if (!cesiumSkinned.animations.empty())
				pose = JLib::SamplePose(cesiumSkinned.skeleton, cesiumSkinned.animations[0], animTime);
			// -90deg X stands CesiumMan upright: the asset's root node carries a Z-up->Y-up rotation
			// the loader doesn't bake in (see below). Leftmost = applied first, in object space. Flip
			// the sign / swap to Z if it's still off -- it's a tuning knob until the loader handles it.
			// The PLAYER now: stand upright (-90 X), scale, face charYaw, place at charPos (driven by WASD).
			r3d.SubmitSkinned(cesiumSkinned.mesh,
				XMMatrixRotationX(-XM_PIDIV2) * XMMatrixScaling(1.8f, 1.8f, 1.8f)
					* XMMatrixRotationY(charYaw) * XMMatrixTranslation(charPos.x, charPos.y, charPos.z),
				pose);
		}

		// --- Dense instance stress test: an NxN grid of small cubes, one Submit each. With instancing,
		// N*N objects sharing a mesh collapse to ONE draw -- so to exercise the parallel-vs-serial
		// crossover we spread the grid across `batchStress` distinct cube meshes (round-robin), giving
		// exactly `batchStress` batches/draw-calls. Raise batchStress (LEFT/RIGHT) and read the B bench. ---
		{
			const int   N       = stressGrid;
			const int   K       = batchStress < 1 ? 1 : batchStress;   // distinct meshes to spread across
			const float spacing = 0.55f;
			const float half    = (N - 1) * spacing * 0.5f;
			for (int gy = 0; gy < N; ++gy)
				for (int gx = 0; gx < N; ++gx) {
					float x = gx * spacing - half;
					float y = gy * spacing - half;
					const JLib::Mesh& m = cubeVariants[(gy * N + gx) % K];   // round-robin -> K batches
					r3d.Submit(m, XMMatrixScaling(0.18f, 0.18f, 0.18f) * XMMatrixRotationY(spin3d)
					              * XMMatrixTranslation(x, y, 7.0f));
				}
		}
			
	}

	// Render particles as small quads
	for (size_t i = 0; i < cpuParticles.count; ++i) {
		JLib::BatchItem item;
		item.position = { cpuParticles.posX[i], cpuParticles.posY[i] };
		item.size = { 4.0f, 4.0f };  // 4x4 pixel particles
		item.mesh = quadMesh;
		item.tex = tileTexture;

		// Color with alpha based on remaining life
		float alpha = cpuParticles.life[i] / cpuParticles.maxLife[i];
		item.color = cpuParticles.color[i];
		item.color.w = alpha;  // fade out as they die

		r2d.Submit(item);
	}

	// Performance stats overlay
	std::string stats = "Particles: " + std::to_string(cpuParticles.count) + " | Speed: " + std::to_string((int)particleSpeed) + "px/s";
	r2d.SubmitText(*font, 10.0f, r2d.GetScreenSize().y - 60, stats, 1.0f, JLib::Colors::White);

	// 3D record readout: wall time (ms), current mode, REAL draw calls (instanced batches) + instances.
	char rec[224];
	snprintf(rec, sizeof(rec), "3D record: %.3f ms  [AUTO: %s]  draws=%zu  instances=%zu  culled=%zu   (B: benchmark)",
		r3d.GetLastRecordMs(), r3d.IsParallelRecording() ? "parallel" : "serial",
		r3d.GetLastDrawCallCount(), r3d.GetLastInstanceCount(), r3d.GetLastCulledCount());
	r2d.SubmitText(*font, 10.0f, 30.0f, rec, 1.0f, JLib::Colors::White);

	// A/B benchmark readout: when on, both modes' rolling-average record times at the current batch count.
	if (r3d.IsAutoBenchmark()) {
		char bench[192];
		snprintf(bench, sizeof(bench), "BENCH @ %zu batches:  serial %.3f ms   vs   parallel %.3f ms",
			r3d.GetLastDrawCallCount(), r3d.GetSerialAvgMs(), r3d.GetParallelAvgMs());
		r2d.SubmitText(*font, 10.0f, 50.0f, bench, 1.0f, JLib::Colors::Yellow);
	}

	std::string controls = "F: fire sphere | UP/DOWN: adjust spawn speed | R: reset | ESC: quit";
	r2d.SubmitText(*font, 10.0f, screenSize.y - 20.0f, controls, 0.8f, JLib::Colors::Gray);
}

void StartMenuScene::HandleInput(float dt)
{
	// UP: increase particle speed
	if (input->IsKeyPressed(VK_UP)) {
		particleSpeed = std::min(particleSpeed + 50.0f, 500.0f);
	}
	// DOWN: decrease particle speed
	else if (input->IsKeyPressed(VK_DOWN)) {
		particleSpeed = std::max(particleSpeed - 50.0f, 50.0f);
	}

	// R: reset particles
	if (input->IsKeyPressed('R')) {
		cpuParticles.count = 0;
	}

	// (Serial vs parallel is now AUTO-selected from the measured ~15k-batch crossover -- no manual toggle.
	//  The B benchmark below still forces both modes to measure them.)

	// B: toggle the A/B benchmark -- alternates modes each frame and shows both rolling averages, so you
	// can read the parallel-vs-serial crossover at the current batch count.
	if (input->IsKeyPressed('B')) {
		r3d.SetAutoBenchmark(!r3d.IsAutoBenchmark());
	}

	// LEFT/RIGHT: halve/double the distinct-mesh BATCH count (1..kMaxBatchVariants) for the sweep. Power-
	// of-2 steps (1,2,4,...128) so you can quickly find where parallel crosses under serial in the bench.
	if (input->IsKeyPressed(VK_LEFT))  batchStress = std::max(1, batchStress / 2);
	if (input->IsKeyPressed(VK_RIGHT)) batchStress = std::min(kMaxBatchVariants, batchStress * 2);

	// F: FIRE a projectile sphere (the slingshot seed -- camera-aimed, fixed power; charging comes later).
	// Spawned just ahead of the character, launched along the camera's horizontal facing with a slight
	// upward arc. Its impacts fire the contact->burst chain, so this is the repeatable effects test too.
	if (input->IsKeyPressed('F')) {
		float sYaw = sinf(camYaw), cYaw = cosf(camYaw);
		DirectX::XMFLOAT3 dir = { sYaw, 0.35f, cYaw };                    // horizontal fwd + arc
		float len = sqrtf(dir.x * dir.x + dir.y * dir.y + dir.z * dir.z);
		dir = { dir.x / len, dir.y / len, dir.z / len };
		DirectX::XMFLOAT3 spawn = { charPos.x + dir.x * 1.5f, charPos.y + 1.2f, charPos.z + dir.z * 1.5f };
		auto h = physics3d.AddDynamicSphere(spawn, 0.35f);
		physics3d.SetLinearVelocity(h, { dir.x * 18.0f, dir.y * 18.0f, dir.z * 18.0f });
		physBodies.push_back({ h, &sphere3d, { 0.5f, 0.5f, 0.5f } });     // draw it like the other spheres
	}

	// ESC: quit
	if (input->IsKeyPressed(VK_ESCAPE)) {
		cmdQ.push_back(CMD::QUIT);
	}
}

void StartMenuScene::SpawnParticles(size_t count)
{
	auto screenSize = r2d.GetScreenSize();

	// Allocate if needed
	if (cpuParticles.capacity == 0) {
		cpuParticles.capacity = maxParticles;
		cpuParticles.posX = new float[cpuParticles.capacity];
		cpuParticles.posY = new float[cpuParticles.capacity];
		cpuParticles.velX = new float[cpuParticles.capacity];
		cpuParticles.velY = new float[cpuParticles.capacity];
		cpuParticles.life = new float[cpuParticles.capacity];
		cpuParticles.maxLife = new float[cpuParticles.capacity];
		cpuParticles.color = new DirectX::XMFLOAT4[cpuParticles.capacity];
	}

	size_t toSpawn = std::min(count, cpuParticles.capacity - cpuParticles.count);
	for (size_t i = 0; i < toSpawn; ++i) {
		size_t idx = cpuParticles.count++;

		// Random X position across screen top
		float randX = (rand() / (float)RAND_MAX) * screenSize.x;

		// Spawn at top of screen, falling downward
		cpuParticles.posX[idx] = randX;
		cpuParticles.posY[idx] = 0.0f;
		cpuParticles.velX[idx] = 0.0f;
		cpuParticles.velY[idx] = particleSpeed;  // downward motion

		// Life based on screen height and fall speed
		float lifetime = screenSize.y / particleSpeed;
		cpuParticles.life[idx] = lifetime;
		cpuParticles.maxLife[idx] = lifetime;

		// Random color (RGB with alpha=1, will be modulated by life in Draw)
		cpuParticles.color[idx].x = 0.5f + (rand() / (float)RAND_MAX) * 0.5f;  // R: 0.5-1.0
		cpuParticles.color[idx].y = 0.5f + (rand() / (float)RAND_MAX) * 0.5f;  // G: 0.5-1.0
		cpuParticles.color[idx].z = 0.5f + (rand() / (float)RAND_MAX) * 0.5f;  // B: 0.5-1.0
		cpuParticles.color[idx].w = 1.0f;  // A: full opacity (will be faded by life alpha in Draw)
	}
}


