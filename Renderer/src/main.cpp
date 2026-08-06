/*
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#include <shellapi.h>   // CommandLineToArgvW
#include <cstdint>
#include <cwchar>       // wcscmp, wcstol
#include <TaskDAG.h>
#include "../include/Window.h"
#include "../include/Renderer2D.h"
#include "../include/Helpers.h"
#include "../include/Font.h"
#include "../include/Camera2D.h"
//#include "../include/InputManager.h"
// -w/--width, -h/--height, -warp/--warp. Parsed here (an app concern), then handed
// to the Window (size) and Renderer (warp adapter).
static void ParseCommandLine(uint32_t& width, uint32_t& height, bool& useWarp)
{
	int argc = 0;
	wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv) return;

	for (int i = 0; i < argc; ++i)
	{
		if ((wcscmp(argv[i], L"-w") == 0 || wcscmp(argv[i], L"--width") == 0) && i + 1 < argc)
			width = wcstol(argv[++i], nullptr, 10);
		else if ((wcscmp(argv[i], L"-h") == 0 || wcscmp(argv[i], L"--height") == 0) && i + 1 < argc)
			height = wcstol(argv[++i], nullptr, 10);
		else if (wcscmp(argv[i], L"-warp") == 0 || wcscmp(argv[i], L"--warp") == 0)
			useWarp = true;
	}
	LocalFree(argv);
}

// The whole program now reads as the lifecycle of two objects:
//   Window   -- owns the OS window + message pump (+ fullscreen/keys)
//   Renderer -- owns every GPU object + the render/resize/cleanup logic
int CALLBACK wWinMain(HINSTANCE hInstance, HINSTANCE, PWSTR, int)
{
	JLib::TaskScheduler::Init();
	JLib::TaskScheduler& scheduler = JLib::TaskScheduler::Instance();

	// 100% client-area scaling, DPI-aware non-client chrome.
	SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

	// WIC (used by DirectXTex's LoadFromWICFile to load textures) is COM-based and
	// requires a COM apartment on this thread; without this the texture load fails with
	// CO_E_NOTINITIALIZED. Initialize once for the app, uninitialize on exit.
	if (FAILED(CoInitializeEx(nullptr, COINIT_MULTITHREADED)))
		return -1;

	uint32_t width = 1280, height = 720;
	bool useWarp = false;
	ParseCommandLine(width, height, useWarp);

	Window   window(hInstance, L"DirectX 12", width, height);
	// RendererCore: device/swap chain/fences/PRE-POST lists/particle system -- the low-level
	// GPU guts, reusable by a future 3D renderer. Renderer2D: sprite/text batching, driven BY
	// core's BeginFrame/PresentFrame via SetRenderer2D(). See both classes' header comments.
	RendererCore core;
	Renderer2D   renderer;
	core.SetRenderer2D(&renderer);
	window.SetRenderer(&core);                           // so WM_SIZE reaches Core (owns Resize/VSync)

	core.Initialize(window.GetHandle(), width, height, useWarp);
	renderer.Initialize(core); // builds root sig/PSO/instance buffer/SRV heap/font against core's device
	// Register particle effects AFTER both Initialize() calls (needs core's m_ComputeRootSignature/
	// m_CommandQueue), same pattern as RegisterEffect() for sprite shaders. Each gets its OWN
	// compute shader -- see UpdateParticles.hlsl (straight-line motion) and WaveParticles.hlsl
	// (sin-driven wave) -- and its own particle pool, so they can never step on each other's slots.
	// zLayer=1 here: draws over anything at zLayer 0 (e.g. background tiles) and under zLayer 2+
	// (e.g. the FPS/text overlay) -- see RendererCore::PresentFrame's layer-interleaving comment.
	uint32_t linearEffect = core.RegisterParticleEffect(L"shaders\\UpdateParticles.cso", 256000, 1);
	uint32_t waveEffect = core.RegisterParticleEffect(L"shaders\\WaveParticles.cso", 50000, 1);
	//InputManager input;
	//input.Initialize(); // real window (window.GetHandle()) exists now -- GameInput needs a focusable HWND
	window.Show();
	Vertex vertices[] = {
		// New X, Y      Color (RGBA)          Tex (U, V)
		{  0.0f,  0.3333f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.5f, 0.0f }, // Top
		{  0.3f, -0.1667f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }, // Right
		{ -0.3f, -0.1667f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }  // Left
	};
	uint32_t triangleIndices[] = { 0, 1, 2 }; // Just one triangle

	Vertex quadVertices[] = {
		{ -0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 0.0f }, // Top-Left
		{  0.5f,  0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 0.0f }, // Top-Right
		{ -0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  0.0f, 1.0f }, // Bottom-Left
		{  0.5f, -0.5f, 0.0f,  1.0f, 1.0f, 1.0f, 1.0f,  1.0f, 1.0f }  // Bottom-Right
	};
	uint32_t quadIndices[] = { 0, 1, 2, 1, 3, 2 }; // The two-triangle quad
	auto resourceManager = renderer.GetResourceManager();
	auto cmdList = renderer.GetCommandList();
	TextureHandle wall;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		wall = resourceManager->LoadTexture(ExeRelative(L"textures\\wall.png"), cmd);
		});
	TextureHandle wood;
	renderer.ExecuteUploadCommand([&](ID3D12GraphicsCommandList* cmd) {
		wood = resourceManager->LoadTexture(ExeRelative(L"textures\\wood.png"), cmd);
		});
	Font font;
	font.Load(ExeRelative(L"fonts\\Aldrich-Regular.fnt"), ExeRelative(L"fonts\\Aldrich-Regular.png"), renderer);
	// Meshes MUST be their own persistent variables, not stored by value inside a SpriteBatchItem --
	// Submit() caches a pointer to whatever Mesh an item points at, permanently, across every
	// future frame (see SpriteBatchItem::mesh's comment), so the Mesh needs to outlive the item.
	Mesh triangleMesh = resourceManager->CreateMesh(vertices, 3, triangleIndices, 3);
	Mesh quadMesh = resourceManager->CreateMesh(quadVertices, 4, quadIndices, 6);
	BatchItem triangleItem;
	BatchItem quadItem;
	triangleItem.mesh = &triangleMesh;
	quadItem.mesh = &quadMesh;
	float rotation = 0.0f;
	float clearColor[] = { 0.0f, 0.2f, 0.4f, 1.0f }; // Define your background
	float size = 500.0f;
	bool zoomSwitch = false;
	Camera2D camera;
	camera.position = { 1280 / 2,720 / 2 }; // center of the screen
	struct Object { DirectX::XMFLOAT2 worldPos; BatchItem item; };
	Object quadA{ { 1280 / 2,720 / 2 } };
	Object triA{ {200.0f, 200.0f} };
	Object triB{ {400.0f, 400.0f} };
	Object triC{ {600.0f, 600.0f} };
	Object quadB{ {500.0f, 500.0f} };
	quadItem.position = { 1280 / 2,720 / 2 };
	quadItem.tex = wood;

	renderer.Submit(quadItem);
	triangleItem.position = { 200,200 };
	triangleItem.size = { size, size };
	triangleItem.zLayer = 1;
	triangleItem.rotation = rotation;
	triangleItem.tex = wall;
	renderer.Submit(triangleItem);
	triangleItem.position = { 400,400 };
	renderer.Submit(triangleItem);
	triangleItem.position = { 600,600 };
	renderer.Submit(triangleItem);
	quadItem.position = { 500,500 };

	bool cameraswitch = false;

	while (window.ProcessMessages())
	{
		//input.Update();
		//if (input.IsKeyPressed(VK_SPACE))
		//	OutputDebugStringA("Space pressed (via InputManager)\n");
		
		float dt = renderer.GetFrameTime();
		renderer.UpdateGlobalUniforms(renderer.GetScreenSize(), camera.position, camera.zoom);
		if (size <= 0.0f)
			zoomSwitch = true;
		else if (size >= 500.0f)
			zoomSwitch = false;
		if (!zoomSwitch)
			size -= 30.0f * dt;
		else
			size += 30.0f * dt;
		rotation += 30.0f * dt; // 30 degrees per second, now truly framerate-independent
		static float direction = 1.0f;

		// 2. Apply movement
		camera.position.x += 30.0f * direction * dt;

		// 3. Boundary check (The "Push" Method)
		if (camera.position.x >= width) {
			camera.position.x = width; // Snap to boundary
			direction = -1.0f;         // Reverse
		}
		else if (camera.position.x <= 0) {
			camera.position.x = 0;     // Snap to boundary
			direction = 1.0f;          // Reverse
		}
		// Per-frame DAG: StartFrame -> {sprites, text} (main-affinity, parallel-in-principle
		// but both actually run on main, serially, via ProcessMainThread) -> PresentFrame.
		// EVERY Submit-producer node MUST be added as a dependency of presentNode below, or
		// PresentFrame's FlushBatchParallel merge can race with a producer still submitting.
		JLib::TaskDAG dag(scheduler);

		core.m_StartFrameCtx = { &core, { clearColor[0], clearColor[1], clearColor[2], clearColor[3] } };
		auto* startTask = scheduler.CreateTask(RendererCore::StartFrameTask, &core.m_StartFrameCtx,false);
		auto* startNode = dag.CreateMainNode(startTask);

		auto* spritesTask = scheduler.CreateTask([&renderer, &quadItem, &triangleItem, wood, wall, rotation, linearEffect, waveEffect,size, &camera,width,height] {
			quadItem.position = camera.WorldToScreen(quadItem.position, {(float)width, (float)height});
			quadItem.size = { size, size };
			quadItem.zLayer = 1;
			quadItem.rotation = rotation;
			quadItem.tex = wood;
			renderer.Submit(quadItem);
			triangleItem.position = camera.WorldToScreen({ 200,200 }, {(float)width, (float)height});
			triangleItem.size = { size, size };
			triangleItem.zLayer = 1;
			triangleItem.rotation = rotation;
			triangleItem.tex = wall;
			renderer.Submit(triangleItem);
			triangleItem.position = camera.WorldToScreen({ 400,400 }, {(float)width, (float)height});
			renderer.Submit(triangleItem);
			triangleItem.position = camera.WorldToScreen({ 600,600 }, {(float)width, (float)height});
			renderer.Submit(triangleItem);
			quadItem.position = camera.WorldToScreen({ 500,500 }, {(float)width, (float)height});
			renderer.Submit(quadItem);
			// effectID, basePosition, offsetRadius (jitter so multiple spawns scatter instead of
			// stacking), velocity, lifetime, color.
			renderer.RequestSpawn(linearEffect, { 300,300 }, 20.0f, { 0.0f, -50.0f }, 5.0f, { 1.0f, 0.6f, 0.1f, 1.0f });
			// velocity.y is repurposed as wave AMPLITUDE by WaveParticles.hlsl, not a real
			// vertical speed -- see that file's comment.
			renderer.RequestSpawn(waveEffect, { 900,300 }, 20.0f, { 30.0f, 60.0f }, 5.0f, { 0.2f, 0.6f, 1.0f, 1.0f });
			});
		auto* spritesNode = dag.CreateMainNode(spritesTask);
		dag.AddDependency(spritesNode, startNode);

		auto* textTask = scheduler.CreateTask([&renderer] {
			Font* f = renderer.GetSysFont();
			renderer.SubmitText(*f,  400.0f, 150.0f, "Left Aligned\ntest", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Left, 0);
			renderer.SubmitText(*f,  400.0f, 250.0f, "Center Aligned", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Center, 0);
			renderer.SubmitText(*f,  400.0f, 350.0f, "Right Aligned\ntest", 1.0f, {1,1,1,1}, 0.0f, TextAlign::Right, 0);
			renderer.UpdateFPS(); // its own SubmitText call -- same tier, same rules
			});
		auto* textNode = dag.CreateMainNode(textTask);
		dag.AddDependency(textNode, startNode);

		core.m_UpdateParticlesCtx = { &core };
		auto* particlesTask = scheduler.CreateTask(RendererCore::UpdateParticlesTask, &core.m_UpdateParticlesCtx);
		auto* particlesNode = dag.CreateMainNode(particlesTask);
		dag.AddDependency(particlesNode, startNode);

		core.m_PresentFrameCtx = { &core };
		auto* presentTask = scheduler.CreateTask(RendererCore::PresentFrameTask, &core.m_PresentFrameCtx);
		auto* presentNode = dag.CreateMainNode(presentTask);
		dag.AddDependency(presentNode, spritesNode);
		dag.AddDependency(presentNode, textNode);
		dag.AddDependency(presentNode, particlesNode);

		JLib::WaitGroup frameWg;
		frameWg.n.store(1, std::memory_order_relaxed); // 1 = the PresentFrame tail node
		presentTask->waitGroup = &frameWg;             // set BEFORE dag.Submit()

		dag.Submit();
		scheduler.WaitForMain(frameWg); // pumps ProcessMainThread; frame N+1 can't start until this returns

	}                               // continuous render loop
	// MUST be explicit, here, before renderer (Renderer2D) goes out of scope -- local
	// destruction order is REVERSE of declaration order, so renderer destructs BEFORE core
	// does. Relying on ~RendererCore() alone means Cleanup()'s GPU-idle Flush() would run
	// AFTER Renderer2D's command lists/allocators/instance buffer are already released,
	// letting the GPU still be mid-flight on resources that just got torn out from under it --
	// that's the shutdown freeze (driver doing extra recovery work), not a coincidence.
	core.Cleanup();
	CoUninitialize();
	return 0;
}
*/