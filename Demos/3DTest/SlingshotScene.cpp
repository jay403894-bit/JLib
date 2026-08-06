#include "pch.h"
#include "SlingshotScene.h"
#include <Helpers.h>    // ExeRelative
#include <Colors.h>
#include <cmath>
#include <cstdio>

using namespace DirectX;

SlingshotScene::SlingshotScene(JLib::Font* font_, JLib::SoundManager* sound_, JLib::Renderer2D& r2d_,
                               JLib::ResourceManager& rm_, JLib::Renderer3D& r3d_,
                               std::shared_ptr<JLib::InputManager> input_,
                               unsigned int width, unsigned int height)
	: font(font_), r2d(r2d_), rm(rm_), r3d(r3d_), input(std::move(input_)),
	  screenW((float)width), screenH((float)height), sound(sound_)
{
	// Primitive render meshes; wood albedo pops in async (Submit gates on IsTextureReady internally).
	woodTex = rm.LoadTextureAsync(JLib::ExeRelative(L"textures\\wood.png"));
	floorMesh = JLib::MakeCubeMesh(rm);
	floorMesh.material.albedo = woodTex; floorMesh.material.roughness = 0.9f;
	boxMesh = JLib::MakeCubeMesh(rm);
	boxMesh.material.albedo = woodTex; boxMesh.material.roughness = 0.8f;
	sphereMesh = JLib::MakeSphereMesh(rm);
	sphereMesh.material.albedo = woodTex; sphereMesh.material.roughness = 0.5f;
	if (sound) {
		music = sound->PlayLoop(JLib::ExeRelativeA("sound\\neocrey - Jump to win.mp3").c_str());
		if (!music.IsValid())
			OutputDebugStringA("[SlingshotScene] PlayLoop(neocrey - Jump to win.mp3) failed -- "
			                   "is the file next to the exe (sound\\ folder)?\n");
	}
	// Physics world: floor + the target tower. (Projectiles are added per shot at runtime.)
	physics3d.Init();
	physics3d.AddStaticBox({ 0.0f, -1.0f, 0.0f }, { 50.0f, 0.5f, 50.0f });   // top at y = -0.5
	BuildTower();
	// Bonus gate: a SENSOR volume floating on the arc path -- shots that fly through it score without
	// touching it (no collision response). The visual frame is drawn in Draw(); this is just the trigger.
	gateSensor = physics3d.AddSensorBox(kGatePos, { 0.85f, 0.85f, 0.15f });
	physics3d.SetUserData(gateSensor, kUserGate);
	physics3d.Finalize();

	burstPool = r3d.AddBurstPool(8192);   // impact sparks (contact -> burst chain)
}

void SlingshotScene::BuildTower() {
	// 3 columns x 4 rows of unit-ish boxes at z=14 -- far enough that aim + power matter.
	const float half = 0.4f;
	for (int row = 0; row < 4; ++row)
		for (int col = -1; col <= 1; ++col) {
			XMFLOAT3 pos = { col * 0.85f, -0.5f + half + row * (half * 2.0f), 14.0f };
			auto h = physics3d.AddDynamicBox(pos, { half, half, half });
			physics3d.SetUserData(h, (uint64_t)targets.size() + 1);   // block tag = index+1 (see IsBlockUD)
			targets.push_back({ h, pos, false, false });
		}
}

void SlingshotScene::ResetGame() {
	for (auto& t : targets)     physics3d.RemoveBody(t.handle);
	for (auto& p : projectiles) physics3d.RemoveBody(p.handle);
	targets.clear();
	projectiles.clear();
	BuildTower();
	physics3d.Finalize();   // re-optimize the broadphase after the rebuild (fine outside Update)
	shotsLeft = kShots;
	power = kPowerMin; charging = false;
	score = 0;
}

DirectX::XMFLOAT3 SlingshotScene::AimDir() const {
	float cp = cosf(aimPitch);
	return { sinf(aimYaw) * cp, sinf(aimPitch), cosf(aimYaw) * cp };   // yaw 0 = +Z, straight at the tower
}

void SlingshotScene::HandleInput(float dt) {
	// MOUSE aims (primary). GetMouseDeltaX/Y is a TRUE per-frame delta now (Raw Input reports relative
	// motion natively) -- the old hand-differencing of GameInput's cumulative accumulator is gone.
	// Mouse right = arc right, mouse up = arc higher. Arrows remain as a keyboard fallback.
	{
		float dx = std::max(-200.0f, std::min(200.0f, input->GetMouseDeltaX()));
		float dy = std::max(-200.0f, std::min(200.0f, input->GetMouseDeltaY()));
		aimYaw   += dx * kMouseSens;
		aimPitch -= dy * kMouseSens;   // screen-y grows downward; mouse-up raises the arc
	}
	const float rate = 0.9f * dt;
	if (input->IsKeyDown(VK_LEFT))  aimYaw -= rate;
	if (input->IsKeyDown(VK_RIGHT)) aimYaw += rate;
	if (input->IsKeyDown(VK_UP))    aimPitch += rate;
	if (input->IsKeyDown(VK_DOWN))  aimPitch -= rate;
	aimYaw   = std::max(-0.6f, std::min(0.6f, aimYaw));
	aimPitch = std::max(0.10f, std::min(0.90f, aimPitch));   // cap lower: high lobs just waste the shot

	// Hold LEFT MOUSE (or SPACE) to charge; RELEASE fires. Power resets to min after each shot.
	const bool held = input->IsMouseButtonDown(JLib::InputManager::MouseLeftButton) || input->IsKeyDown(VK_SPACE);
	if (shotsLeft > 0 && held) {
		if (!charging) { charging = true; power = kPowerMin; }
		power = std::min(kPowerMax, power + kChargeRate * dt);
	}
	if (charging && !held) {   // release (either input) = fire
		charging = false;
		XMFLOAT3 dir = AimDir();
		XMFLOAT3 spawn = { kSlingPos.x + dir.x * 0.7f, kSlingPos.y + dir.y * 0.7f, kSlingPos.z + dir.z * 0.7f };
		auto h = physics3d.AddDynamicSphere(spawn, 0.3f, kProjectileMass, /*ccd*/ true);   // heavy + LinearCast:
		// plows through blocks and can't TUNNEL through thin geometry at max power between physics steps
		physics3d.SetLinearVelocity(h, { dir.x * power, dir.y * power, dir.z * power });
		physics3d.SetUserData(h, kUserProjectile + (uint64_t)projectiles.size());   // shot tag (see IsProjectileUD)
		projectiles.push_back({ h, false });
		--shotsLeft;
		power = kPowerMin;
	}

	if (input->IsKeyPressed('R'))       ResetGame();
	if (input->IsKeyPressed(VK_ESCAPE)) quitRequested = true;
}

void SlingshotScene::Update(bool& isRunning, float dt) {
	HandleInput(dt);
	if (quitRequested) { isRunning = false; return; }

	physics3d.Update(dt);

	// Contact processing: user data (userA/userB) tells us WHICH entities touched, so contacts drive
	// gameplay now, not just sparks. Sensor events are handled FIRST and never impact-thresholded
	// (a clean gate pass has low closing speed by design).
	contactEvents.clear();
	physics3d.DrainContactEvents(contactEvents);
	int burstsFired = 0;
	for (const auto& ce : contactEvents) {
		// --- gate bonus: a projectile overlapping the sensor ring. Credit each shot once. ---
		if (ce.isSensor) {
			uint64_t proj = IsProjectileUD(ce.userA) ? ce.userA : (IsProjectileUD(ce.userB) ? ce.userB : 0);
			bool gate = (ce.userA == kUserGate) || (ce.userB == kUserGate);
			if (gate && proj) {
				size_t idx = (size_t)(proj - kUserProjectile);
				if (idx < projectiles.size() && !projectiles[idx].gateCredited) {
					projectiles[idx].gateCredited = true;
					score += kScoreGate;
					JLib::Renderer3D::BurstDesc b;   // cool blue ring-pass flourish
					b.position = ce.position; b.normal = { 0.0f, 1.0f, 0.0f };
					b.count = 32; b.speed = 2.5f; b.lifetime = 0.5f; b.size = 0.06f; b.normalBias = 0.0f;
					b.color = { 0.4f, 0.7f, 1.0f, 1.0f };
					r3d.RequestBurst3D(burstPool, b);
				}
			}
			continue;   // sensors never spark as impacts
		}

		// --- direct-hit bonus: the PROJECTILE ITSELF striking a block (block-on-block tumble doesn't
		// count -- only user data can make that distinction). First strike per block scores. ---
		bool projInvolved = IsProjectileUD(ce.userA) || IsProjectileUD(ce.userB);
		uint64_t blockUD = IsBlockUD(ce.userA) ? ce.userA : (IsBlockUD(ce.userB) ? ce.userB : 0);
		if (projInvolved && blockUD) {
			size_t bi = (size_t)(blockUD - 1);
			if (bi < targets.size() && !targets[bi].directHit) {
				targets[bi].directHit = true;
				score += kScoreDirectHit;
			}
		}

		// --- impact sparks (threshold skips settling; direct projectile hits burst HOT, tumbles dusty) ---
		if (ce.speed < 2.0f) continue;
		if (++burstsFired > 8) continue;
		JLib::Renderer3D::BurstDesc b;
		b.position = ce.position; b.normal = ce.normal;
		b.lifetime = 0.6f; b.normalBias = 0.6f;
		if (projInvolved) { b.count = 48; b.speed = 2.5f + ce.speed * 0.3f;  b.size = 0.09f; b.color = { 1.0f, 0.55f, 0.15f, 1.0f }; }
		else              { b.count = ce.speed > 6.0f ? 40u : 24u; b.speed = 2.0f + ce.speed * 0.25f; b.size = 0.07f; b.color = { 0.95f, 0.8f, 0.45f, 1.0f }; }
		r3d.RequestBurst3D(burstPool, b);
	}

	// Topple detection: a target counts once its center strays far enough from where it was stacked
	// (knocked off, tipped over, or the tower collapsed onto it). Sticky -- once toppled, always toppled.
	for (auto& t : targets) {
		if (t.toppled) continue;
		XMFLOAT3 p, h; XMFLOAT4 r;
		physics3d.GetBody(t.handle, p, r, h);
		float dx = p.x - t.spawnPos.x, dy = p.y - t.spawnPos.y, dz = p.z - t.spawnPos.z;
		if (dx * dx + dy * dy + dz * dz > 0.55f * 0.55f) { t.toppled = true; score += kScoreTopple; }
	}
}

void SlingshotScene::Draw() {
	auto screenSize = r2d.GetScreenSize();
	float aspect = (screenSize.y > 0.0f) ? screenSize.x / screenSize.y : 1.0f;

	// Fixed camera: behind/above the slingshot, looking downrange at the tower.
	XMVECTOR eye    = XMVectorSet(0.0f, 3.4f, -7.5f, 1.0f);
	XMVECTOR target = XMVectorSet(0.0f, 1.0f, 8.0f, 1.0f);
	XMMATRIX view = XMMatrixLookAtLH(eye, target, XMVectorSet(0, 1, 0, 0));
	XMMATRIX proj = XMMatrixPerspectiveFovLH(XMConvertToRadians(58.0f), aspect, 0.1f, 200.0f);
	XMFLOAT3 camPos; XMStoreFloat3(&camPos, eye);
	r3d.SetCamera(view * proj, camPos);

	r3d.SetAmbient({ 0.12f, 0.12f, 0.14f });
	r3d.ClearLights();
	r3d.AddDirectionalLight({ -0.4f, -0.8f, 0.4f }, { 1.0f, 0.97f, 0.9f }, 3.0f);
	r3d.AddPointLight({ 0.0f, 3.0f, 13.0f }, { 1.0f, 0.6f, 0.3f }, 8.0f, 12.0f);   // warm glow on the tower

	// Ground slab (visual; the physics floor is invisible collision at the same height).
	r3d.Submit(floorMesh, XMMatrixScaling(60.0f, 1.0f, 60.0f) * XMMatrixTranslation(0.0f, -1.0f, 0.0f));

	// Slingshot "pouch" marker at the launch origin.
	r3d.Submit(sphereMesh, XMMatrixScaling(0.5f, 0.5f, 0.5f)
	                       * XMMatrixTranslation(kSlingPos.x, kSlingPos.y, kSlingPos.z));

	// Ballistic arc preview: p(t) = origin + v0*t + 0.5*g*t^2 sampled as dots until it hits the ground.
	{
		XMFLOAT3 d = AimDir();
		const float g = -9.8f;
		for (int i = 1; i <= 26; ++i) {
			float t = i * 0.09f;
			float x = kSlingPos.x + d.x * power * t;
			float y = kSlingPos.y + d.y * power * t + 0.5f * g * t * t;
			float z = kSlingPos.z + d.z * power * t;
			if (y < -0.4f) break;
			r3d.Submit(sphereMesh, XMMatrixScaling(0.12f, 0.12f, 0.12f) * XMMatrixTranslation(x, y, z));
		}
	}

	// Bonus-gate frame: four thin bars around the sensor volume (the trigger itself is invisible; shots
	// fly through the opening). Matches kGatePos + the AddSensorBox half-extents (0.85 opening).
	{
		const XMFLOAT3 g = kGatePos;
		r3d.Submit(boxMesh, XMMatrixScaling(0.15f, 2.05f, 0.15f) * XMMatrixTranslation(g.x - 0.95f, g.y, g.z));
		r3d.Submit(boxMesh, XMMatrixScaling(0.15f, 2.05f, 0.15f) * XMMatrixTranslation(g.x + 0.95f, g.y, g.z));
		r3d.Submit(boxMesh, XMMatrixScaling(2.05f, 0.15f, 0.15f) * XMMatrixTranslation(g.x, g.y - 0.95f, g.z));
		r3d.Submit(boxMesh, XMMatrixScaling(2.05f, 0.15f, 0.15f) * XMMatrixTranslation(g.x, g.y + 0.95f, g.z));
	}

	// Tower blocks + projectiles at their live physics transforms (removed bodies come back half=0 -> skip).
	auto drawBody = [&](JLib::Physics3D::BodyHandle h, JLib::Mesh& mesh, float meshUnitHalf) {
		XMFLOAT3 p, half; XMFLOAT4 rot;
		physics3d.GetBody(h, p, rot, half);
		if (half.x <= 0.0f) return;
		float s = 1.0f / meshUnitHalf;
		r3d.Submit(mesh, XMMatrixScaling(half.x * s, half.y * s, half.z * s)
		                 * XMMatrixRotationQuaternion(XMLoadFloat4(&rot))
		                 * XMMatrixTranslation(p.x, p.y, p.z));
	};
	for (auto& t : targets)     drawBody(t.handle, boxMesh, 0.5f);
	for (auto& p : projectiles) drawBody(p.handle, sphereMesh, 0.5f);

	// ---- HUD ----
	int toppled = 0; for (auto& t : targets) if (t.toppled) ++toppled;
	const int total = (int)targets.size();

	char line[128];
	snprintf(line, sizeof(line), "Score: %d    Shots: %d    Toppled: %d / %d", score, shotsLeft, toppled, total);
	r2d.SubmitText(*font, 10.0f, 10.0f, line, 1.0f, JLib::Colors::White);

	// Power bar (text blocks; fills while SPACE is held).
	{
		float frac = (power - kPowerMin) / (kPowerMax - kPowerMin);
		int fill = (int)(frac * 20.0f + 0.5f);
		char bar[64]; int n = 0;
		bar[n++] = 'P'; bar[n++] = 'o'; bar[n++] = 'w'; bar[n++] = 'e'; bar[n++] = 'r'; bar[n++] = ' '; bar[n++] = '[';
		for (int i = 0; i < 20; ++i) bar[n++] = (i < fill) ? '#' : '-';
		bar[n++] = ']'; bar[n] = 0;
		r2d.SubmitText(*font, 10.0f, 34.0f, bar, 1.0f, charging ? JLib::Colors::Yellow : JLib::Colors::Gray);
	}

	if (total > 0 && toppled == total) {
		char win[64];
		snprintf(win, sizeof(win), "TOWER DOWN! FINAL SCORE: %d", score);
		r2d.SubmitText(*font, screenSize.x * 0.5f - 500.0f, screenSize.y * 0.4f, win, 1.4f, JLib::Colors::Green);
	}
	else if (shotsLeft == 0)
		r2d.SubmitText(*font, screenSize.x * 0.5f - 320.0f, screenSize.y * 0.4f, "Out of shots - R to retry", 1.2f, JLib::Colors::Red);

	r2d.SubmitText(*font, 10.0f, screenSize.y - 20.0f,
		"MOUSE: aim | hold LMB (or SPACE): charge, release: fire | R: reset | ESC: quit", 0.8f, JLib::Colors::Gray);
}
