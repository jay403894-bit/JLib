#pragma once
#include <vector>
#include <memory>
#include <DirectXMath.h>
#include <Renderer2D.h>
#include <Renderer3D.h>     // 3D-world pass -- scene submits its 3D test content here
#include <Primitives3D.h>   // MakeCubeMesh
#include <ModelLoader.h>    // LoadObjMesh (teapot test)
#include <Camera2D.h>
#include <InputManager.h>
#include "Physics3D.h"     // Jolt-backed 3D physics (PIMPL -- no Jolt/JPH types leak here)
#include <SoundManager.h>
#include <fstream>
#include <Font.h>
#include "Scene.h"
#include "Rect.h"
class StartMenuScene : public Scene
{
public:
	struct ParticleSoA {
		alignas(64) float* posX;
		alignas(64) float* posY;
		alignas(64) float* velX;
		alignas(64) float* velY;
		alignas(64) float* life;      // Remaining lifetime in seconds
		alignas(64) float* maxLife;   // Initial lifetime for normalized alpha/scale
		alignas(64) DirectX::XMFLOAT4* color;  // RGBA colors

		size_t count = 0;
		size_t capacity = 0;

	};
	// font/sound are raw, non-owning pointers -- owned once by main.cpp (outlives the whole
	// SceneManager stack), not loaded/initialized separately per scene. See main.cpp's Font
	// comment for why a raw pointer is safe here.
	StartMenuScene(JLib::Font* font, bool& imguiEnabled, JLib::SoundManager* sound, JLib::Renderer2D& renderer, JLib::ResourceManager& resourceManager, JLib::Renderer3D& r3d, std::shared_ptr<JLib::InputManager> input, JLib::Camera2D& camera,
		JLib::Mesh* quadMesh, JLib::Mesh* slopeUpRightMesh, JLib::Mesh* slopeUpLeftMesh,
		JLib::TextureHandle tileTexture, uint32_t dustEffect, uint32_t width, uint32_t height, HWND windowHandle);
	void KillParticle(ParticleSoA& particles, size_t index) {
		if (index >= particles.count) return;
		size_t last = particles.count - 1;
		if (index != last) {
			particles.posX[index] = particles.posX[last];
			particles.posY[index] = particles.posY[last];
			particles.velX[index] = particles.velX[last];
			particles.velY[index] = particles.velY[last];
			particles.life[index] = particles.life[last];
			particles.maxLife[index] = particles.maxLife[last];
			particles.color[index] = particles.color[last];
		}
		particles.count--;
	}
	void UpdateParticleRange(ParticleSoA& p, size_t start, size_t end, float dt) {
		for (size_t i = start; i < end; ++i) {
			p.posX[i] += p.velX[i] * dt;
			p.posY[i] += p.velY[i] * dt;
			p.life[i] -= dt;
		}
	}
	void UpdateCpuParticles(ParticleSoA& p, float dt) {
		// Parallel update via scheduler (debugging task execution)
		constexpr int kChunkSize = 512;
		if (JLib::TaskScheduler::IsInitialized() && p.count > 256) {
			JLib::TaskScheduler& scheduler = JLib::TaskScheduler::Instance();
			scheduler.ParallelFor(0, static_cast<int>(p.count), kChunkSize,
				[&p, dt](int chunkStart, int chunkEnd) {
					for (int i = chunkStart; i < chunkEnd; ++i) {
						p.posX[i] += p.velX[i] * dt;
						p.posY[i] += p.velY[i] * dt;
						p.life[i] -= dt;
					}
				}
			);
		} else {
			// Fallback to sequential
			for (size_t i = 0; i < p.count; ++i) {
				p.posX[i] += p.velX[i] * dt;
				p.posY[i] += p.velY[i] * dt;
				p.life[i] -= dt;
			}
		}

		// Remove dead particles (sequential, can't parallelize due to count shrinking)
		size_t i = 0;
		while (i < p.count) {
			if (p.life[i] <= 0.0f) {
				size_t last = p.count - 1;
				if (i != last) {
					p.posX[i] = p.posX[last];
					p.posY[i] = p.posY[last];
					p.velX[i] = p.velX[last];
					p.velY[i] = p.velY[last];
					p.life[i] = p.life[last];
					p.maxLife[i] = p.maxLife[last];
					p.color[i] = p.color[last];
				}
				p.count--;
			}
			else {
				i++;
			}
		}

	}
	void Update(bool& isRunning, float dt = 0.0f) override;
	void Draw() override;
	void HandleInput(float dt) override;
	~StartMenuScene() override = default;
private:
	void SpawnParticles(size_t count);

	JLib::SoundHandle music;
	bool& imguiEnabled;
	JLib::SoundManager* sound;
	JLib::ResourceManager& resourceManager;
	JLib::TextureHandle wallTex;
	JLib::TextureHandle woodTex;

	// Particles benchmark
	ParticleSoA cpuParticles;
	float spawnTimer = 0.0f;
	const float spawnInterval = 0.001f;  // spawn burst every 100ms (was 10ms, too fast)
	const size_t spawnPerBurst = 100;   // was 100, reduced to test
	float particleSpeed = 150.0f;  // pixels/sec
	int maxParticles = 60000;  // was 50000, reduced to test
	JLib::Font* font;
	enum class CMD { QUIT };
	std::vector<CMD> cmdQ;
	JLib::Renderer2D& r2d;
	JLib::Renderer3D& r3d;
	// Scene-owned 3D test content, drawn in Draw() (a DAG main-node) so the submit is ordered
	// AFTER Update and BEFORE PresentFrame -- unlike the old cube-in-main shortcut. spin3d is
	// advanced in Update() (game state) and read in Draw(), proving the ordering matters.
	JLib::Mesh cube3d;
	// Distinct cube meshes (identical geometry, separate Mesh objects => separate (mesh,albedo) batches).
	// The stress grid round-robins over the first `batchStress` of these, so batch count == batchStress.
	// That's the knob for the parallel-vs-serial sweep: raise it (LEFT/RIGHT) and watch the B benchmark.
	static constexpr int kMaxBatchVariants = 200;
	std::vector<JLib::Mesh> cubeVariants;
	int batchStress = 1;     // how many distinct cube meshes the grid spreads across == draw-call count
	JLib::Mesh teapot;
	JLib::Mesh cesium;   // static glTF test (CesiumMan.glb, bind pose -- animation ignored)
	JLib::Mesh damagedHelmet;   // PBR material test: DamagedHelmet.glb (metal-rough/emissive/occlusion textures)
	JLib::SkinnedModel cesiumSkinned;   // skinned load of the same asset (now animated via SamplePose)
	float spin3d = 0.0f;
	float animTime = 0.0f;   // CesiumMan animation clock (seconds); advanced in Update, wrapped by clip duration
	// --- Fly camera + a little 3D "world" to walk around (a ground plane + a building you can enter).
	// Editor-style controls: hold RIGHT-MOUSE to look, WASD to move, Q/E for down/up. State is advanced in
	// Update() (has dt + fresh input) and read in Draw() to build the view matrix passed to r3d.SetCamera.
	// Third-person setup: WASD moves the CHARACTER (charPos/charYaw) relative to the camera; the camera
	// ORBITS behind the character via camYaw/camPitch (mouse) at camDist. camPos is the derived eye each frame.
	DirectX::XMFLOAT3 charPos = { 0.0f, 0.0f, 0.0f };  // the player character (CesiumMan) on the ground
	float charYaw    = 0.0f;   // character facing (radians about +Y); snaps to the move direction
	bool  charMoving = false;  // true while a move key is held -> the walk clip plays (else it holds)
	float camDist    = 6.0f;   // how far behind the character the camera sits
	DirectX::XMFLOAT3 camPos = { 0.0f, 1.2f, -6.0f };  // derived orbit eye (computed each frame in Draw)
	float camYaw   = 0.0f;   // radians about world +Y (0 => camera behind, looking down +Z)
	float camPitch = 0.0f;   // radians about the camera's right axis (clamped to +/-89 deg)
	// InputManager::GetMouseDeltaX/Y actually return GameInput's CUMULATIVE motion counter (NOT a
	// per-frame delta, despite the name) -- we difference it against last frame to get the real delta.
	float prevMouseAccX = 0.0f, prevMouseAccY = 0.0f;
	bool  mouseInit = false;  // seed prev on the first frame so we don't apply one giant startup jump
	JLib::Mesh floorMesh;    // big ground slab (woodTex)
	JLib::Mesh wallMesh;     // reused for every wall/roof segment of the building (wallTex)
	JLib::Physics3D physics3d;   // Jolt: the SCENE authors the world (floor + falling shapes) in the ctor
	// Meshes for the dynamic Jolt bodies. cube3d (above) is unit-sized (+/-0.5); sphere is R=0.5; the capsule
	// mesh is NOT unit (R=0.3, cylHalf=0.35) -- hence PhysBody::meshUnitHalf so Draw scales each correctly.
	JLib::Mesh sphere3d;
	JLib::Mesh capsule3d;
	JLib::Mesh cylinder3d;  // Jolt CylinderShape primitive
	JLib::Mesh pyramid3d;   // convex-hull physics body (Physics3D::AddDynamicPyramid); unit-sized (+/-0.5)
	JLib::Mesh cone3d;      // convex-hull physics body (Physics3D::AddDynamicCone)
	// Each dynamic body the scene created, paired with the mesh to draw it as + that mesh's object-space
	// half-size. Draw() reads the body's live transform and scales the mesh by (bodyHalf / meshUnitHalf).
	// The SCENE owns this mapping -- Physics3D just simulates and knows nothing about meshes.
	struct PhysBody { JLib::Physics3D::BodyHandle handle; JLib::Mesh* mesh; DirectX::XMFLOAT3 meshUnitHalf; };
	std::vector<PhysBody> physBodies;
	// Impact effects (particles phase 2A): Jolt contact events -> one-shot particle bursts at the contact
	// point. burstPool is a Renderer3D ring-buffer pool; contactEvents is drained scratch (reused per frame).
	JLib::Renderer3D::ParticleEmitterHandle burstPool = 0;
	std::vector<JLib::Physics3D::ContactEvent> contactEvents;
	int   stressGrid = 200;  // NxN grid of cubes submitted each frame -- dense enough that command-list
	                         // recording is milliseconds (200*200 = 40000 draws), so the parallel split
	                         // is measurable. Press P at runtime to A/B parallel vs serial recording.
	std::shared_ptr<JLib::InputManager> input;
	JLib::Camera2D& camera;
	JLib::Mesh* quadMesh;
	JLib::Mesh* slopeUpRightMesh;
	JLib::Mesh* slopeUpLeftMesh;
	JLib::TextureHandle tileTexture; // FlushBatchTask/ResourceManager::Resolve requires a valid
	                                 // handle -- a default-constructed "no texture" one throws
	                                 // instead of silently skipping, so every tile still needs
	                                 // a real texture; color still does the per-tileID tinting.
	uint32_t dustEffect; // GPU particle effectID from main.cpp's RegisterParticleEffect(), for the
	                     // landing-dust emitter -- see EmitterTable comment for why this can't be
	                     // resolved locally.
	uint32_t width, height;
	// Needed only to convert InputManager::GetMousePos() (virtual-DESKTOP screen coordinates --
	// see its own comment in InputManager.h) into this window's CLIENT coordinates via
	// ScreenToClient before comparing against newGameRect/quitRect, which are built in the same
	// client-space convention Renderer2D::Submit()/GetScreenSize() use. Without this conversion,
	// the two Rects are being hit-tested against a coordinate space they were never built in --
	// works out by coincidence only if the window sits at the desktop origin.
	HWND windowHandle;
};