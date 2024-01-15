#include "D3D12Hook.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <imgui.h>
#include <imgui_impl_dx12.h>
#include <imgui_impl_win32.h>
#include <stdio.h>
#include <string>
#include <chrono>
#include <thread>
#include <atlbase.h>
#include <fstream>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#if __has_include(<detours/detours.h>)
#include <detours/detours.h>
#define USE_DETOURS
#elif __has_include(<MinHook.h>)
#include <MinHook.h>
#ifndef USE_DETOURS
#define USE_MINHOOK
#endif
#else
#error "No hooking library defined!"
#endif

#include "lua_core.h"
#include "loader.h"

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "d3d12.lib")

namespace D3D12 {

	template<typename T>
	static void SafeRelease(T*& res) {
		if (res)
			res->Release();
		res = NULL;
	}

	//https://github.com/ocornut/imgui/blob/master/examples/example_win32_directx12/main.cpp
	struct FrameContext {
		CComPtr<ID3D12CommandAllocator> command_allocator = NULL;
		CComPtr<ID3D12Resource> main_render_target_resource = NULL;
		D3D12_CPU_DESCRIPTOR_HANDLE main_render_target_descriptor;
	};

	// Data
	static std::vector<FrameContext> g_FrameContext;
	static UINT						g_FrameBufferCount = 0;

	static ID3D12Device* pD3DDevice = NULL;
	static CComPtr<ID3D12DescriptorHeap> g_pD3DRtvDescHeap = NULL;
	static CComPtr<ID3D12DescriptorHeap> g_pD3DSrvDescHeap = NULL;
	static CComPtr<ID3D12CommandQueue> g_pD3DCommandQueue = NULL;
	static CComPtr<ID3D12GraphicsCommandList> g_pD3DCommandList = NULL;

	LRESULT APIENTRY WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

	typedef long(__fastcall* Present) (IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags);
	static Present OriginalPresent;

	typedef void(*ExecuteCommandLists)(ID3D12CommandQueue* queue, UINT NumCommandLists, ID3D12CommandList* ppCommandLists);
	static ExecuteCommandLists OriginalExecuteCommandLists;

	typedef long(__fastcall* ResizeBuffers)(IDXGISwapChain3* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
	static ResizeBuffers OriginalResizeBuffers;

	static WNDPROC OriginalWndProc;
	static HWND Window = nullptr;

	static uint64_t* g_MethodsTable = NULL;
	static bool g_Initialized = false;

	static int descriptor_index = 1;
	static bool LoadTextureFromFile(const char* filename, ID3D12Device* d3d_device, D3D12_CPU_DESCRIPTOR_HANDLE srv_cpu_handle, ID3D12Resource** out_tex_resource, int* out_width, int* out_height)
	{
		// Load from disk into a raw RGBA buffer
		int image_width = 0;
		int image_height = 0;
		unsigned char* image_data = stbi_load(filename, &image_width, &image_height, NULL, 4);
		if (image_data == NULL)
			return false;

		// Create texture resource
		D3D12_HEAP_PROPERTIES props;
		memset(&props, 0, sizeof(D3D12_HEAP_PROPERTIES));
		props.Type = D3D12_HEAP_TYPE_DEFAULT;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		D3D12_RESOURCE_DESC desc;
		ZeroMemory(&desc, sizeof(desc));
		desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
		desc.Alignment = 0;
		desc.Width = image_width;
		desc.Height = image_height;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		ID3D12Resource* pTexture = NULL;
		d3d_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_COPY_DEST, NULL, IID_PPV_ARGS(&pTexture));

		// Create a temporary upload resource to move the data in
		UINT uploadPitch = (image_width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
		UINT uploadSize = image_height * uploadPitch;
		desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
		desc.Alignment = 0;
		desc.Width = uploadSize;
		desc.Height = 1;
		desc.DepthOrArraySize = 1;
		desc.MipLevels = 1;
		desc.Format = DXGI_FORMAT_UNKNOWN;
		desc.SampleDesc.Count = 1;
		desc.SampleDesc.Quality = 0;
		desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
		desc.Flags = D3D12_RESOURCE_FLAG_NONE;

		props.Type = D3D12_HEAP_TYPE_UPLOAD;
		props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
		props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

		ID3D12Resource* uploadBuffer = NULL;
		HRESULT hr = d3d_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
			D3D12_RESOURCE_STATE_GENERIC_READ, NULL, IID_PPV_ARGS(&uploadBuffer));
		IM_ASSERT(SUCCEEDED(hr));

		// Write pixels into the upload resource
		void* mapped = NULL;
		D3D12_RANGE range = { 0, uploadSize };
		hr = uploadBuffer->Map(0, &range, &mapped);
		IM_ASSERT(SUCCEEDED(hr));
		for (int y = 0; y < image_height; y++)
			memcpy((void*)((uintptr_t)mapped + y * uploadPitch), image_data + y * image_width * 4, image_width * 4);
		uploadBuffer->Unmap(0, &range);

		// Copy the upload resource content into the real resource
		D3D12_TEXTURE_COPY_LOCATION srcLocation = {};
		srcLocation.pResource = uploadBuffer;
		srcLocation.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
		srcLocation.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srcLocation.PlacedFootprint.Footprint.Width = image_width;
		srcLocation.PlacedFootprint.Footprint.Height = image_height;
		srcLocation.PlacedFootprint.Footprint.Depth = 1;
		srcLocation.PlacedFootprint.Footprint.RowPitch = uploadPitch;

		D3D12_TEXTURE_COPY_LOCATION dstLocation = {};
		dstLocation.pResource = pTexture;
		dstLocation.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
		dstLocation.SubresourceIndex = 0;

		D3D12_RESOURCE_BARRIER barrier = {};
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = pTexture;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;

		// Create a temporary command queue to do the copy with
		ID3D12Fence* fence = NULL;
		hr = d3d_device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
		IM_ASSERT(SUCCEEDED(hr));

		HANDLE event = CreateEvent(0, 0, 0, 0);
		IM_ASSERT(event != NULL);

		D3D12_COMMAND_QUEUE_DESC queueDesc = {};
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.NodeMask = 1;

		ID3D12CommandQueue* cmdQueue = NULL;
		hr = d3d_device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&cmdQueue));
		IM_ASSERT(SUCCEEDED(hr));

		ID3D12CommandAllocator* cmdAlloc = NULL;
		hr = d3d_device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&cmdAlloc));
		IM_ASSERT(SUCCEEDED(hr));

		ID3D12GraphicsCommandList* cmdList = NULL;
		hr = d3d_device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, cmdAlloc, NULL, IID_PPV_ARGS(&cmdList));
		IM_ASSERT(SUCCEEDED(hr));

		cmdList->CopyTextureRegion(&dstLocation, 0, 0, 0, &srcLocation, NULL);
		cmdList->ResourceBarrier(1, &barrier);

		hr = cmdList->Close();
		IM_ASSERT(SUCCEEDED(hr));

		// Execute the copy
		cmdQueue->ExecuteCommandLists(1, (ID3D12CommandList* const*)&cmdList);
		hr = cmdQueue->Signal(fence, 1);
		IM_ASSERT(SUCCEEDED(hr));

		// Wait for everything to complete
		fence->SetEventOnCompletion(1, event);
		WaitForSingleObject(event, INFINITE);

		// Tear down our temporary command queue and release the upload resource
		cmdList->Release();
		cmdAlloc->Release();
		cmdQueue->Release();
		CloseHandle(event);
		fence->Release();
		uploadBuffer->Release();

		// Create a shader resource view for the texture
		D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc;
		ZeroMemory(&srvDesc, sizeof(srvDesc));
		srvDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
		srvDesc.Texture2D.MipLevels = desc.MipLevels;
		srvDesc.Texture2D.MostDetailedMip = 0;
		srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
		d3d_device->CreateShaderResourceView(pTexture, &srvDesc, srv_cpu_handle);

		// Return results
		*out_tex_resource = pTexture;
		*out_width = image_width;
		*out_height = image_height;
		stbi_image_free(image_data);

		return true;
	}

	static inline std::tuple<uintptr_t, int, int> LoadTexture(std::string file)
	{
		// We need to pass a D3D12_CPU_DESCRIPTOR_HANDLE in ImTextureID, so make sure it will fit
		static_assert(sizeof(ImTextureID) >= sizeof(D3D12_CPU_DESCRIPTOR_HANDLE), "D3D12_CPU_DESCRIPTOR_HANDLE is too large to fit in an ImTextureID");
		// We presume here that we have our D3D device pointer in pd3dDevice
		int my_image_width = 0;
		int my_image_height = 0;
		ID3D12Resource* my_texture = NULL;
		// Get CPU/GPU handles for the shader resource view
		// Normally your engine will have some sort of allocator for these - here we assume that there's an SRV descriptor heap in
		// g_pd3dSrvDescHeap with at least two descriptors allocated, and descriptor 1 is unused
		UINT handle_increment = pD3DDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
		D3D12_CPU_DESCRIPTOR_HANDLE my_texture_srv_cpu_handle = g_pD3DSrvDescHeap->GetCPUDescriptorHandleForHeapStart();
		my_texture_srv_cpu_handle.ptr += (handle_increment * descriptor_index);
		D3D12_GPU_DESCRIPTOR_HANDLE my_texture_srv_gpu_handle = g_pD3DSrvDescHeap->GetGPUDescriptorHandleForHeapStart();
		my_texture_srv_gpu_handle.ptr += (handle_increment * descriptor_index);
		descriptor_index += 1;
		// Load the texture from a file
		bool ret = LoadTextureFromFile(file.c_str(), pD3DDevice, my_texture_srv_cpu_handle, &my_texture, &my_image_width, &my_image_height);
		IM_ASSERT(ret);
		return std::make_tuple((uintptr_t)(ImTextureID)my_texture_srv_gpu_handle.ptr, my_image_width, my_image_height);
	}

	long __fastcall HookPresent(IDXGISwapChain3* pSwapChain, UINT SyncInterval, UINT Flags) {
		if (g_pD3DCommandQueue == nullptr) {
			return OriginalPresent(pSwapChain, SyncInterval, Flags);
		}
		if (!g_Initialized) {
			if (FAILED(pSwapChain->GetDevice(__uuidof(ID3D12Device), (void**)&pD3DDevice))) {
				return OriginalPresent(pSwapChain, SyncInterval, Flags);
			}

			{
				DXGI_SWAP_CHAIN_DESC desc;
				pSwapChain->GetDesc(&desc);
				Window = desc.OutputWindow;
				if (!OriginalWndProc) {
					OriginalWndProc = (WNDPROC)SetWindowLongPtr(Window, GWLP_WNDPROC, (__int3264)(LONG_PTR)WndProc);
				}
				g_FrameBufferCount = desc.BufferCount;
				g_FrameContext.clear();
				g_FrameContext.resize(g_FrameBufferCount);
			}

			{
				D3D12_DESCRIPTOR_HEAP_DESC desc = {};
				desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
				desc.NumDescriptors = g_FrameBufferCount;
				desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;

				if (pD3DDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pD3DSrvDescHeap)) != S_OK) {
					return OriginalPresent(pSwapChain, SyncInterval, Flags);
				}
			}

			{
				D3D12_DESCRIPTOR_HEAP_DESC desc;
				desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
				desc.NumDescriptors = g_FrameBufferCount;
				desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
				desc.NodeMask = 1;

				if (pD3DDevice->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&g_pD3DRtvDescHeap)) != S_OK) {
					return OriginalPresent(pSwapChain, SyncInterval, Flags);
				}

				const auto rtvDescriptorSize = pD3DDevice->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
				D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = g_pD3DRtvDescHeap->GetCPUDescriptorHandleForHeapStart();

				for (UINT i = 0; i < g_FrameBufferCount; i++) {

					g_FrameContext[i].main_render_target_descriptor = rtvHandle;
					pSwapChain->GetBuffer(i, IID_PPV_ARGS(&g_FrameContext[i].main_render_target_resource));
					pD3DDevice->CreateRenderTargetView(g_FrameContext[i].main_render_target_resource, nullptr, rtvHandle);
					rtvHandle.ptr += rtvDescriptorSize;
				}

			}

			{
				ID3D12CommandAllocator* allocator;
				if (pD3DDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)) != S_OK) {
					return OriginalPresent(pSwapChain, SyncInterval, Flags);
				}

				for (size_t i = 0; i < g_FrameBufferCount; i++) {
					if (pD3DDevice->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_FrameContext[i].command_allocator)) != S_OK) {
						return OriginalPresent(pSwapChain, SyncInterval, Flags);
					}
				}

				if (pD3DDevice->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_FrameContext[0].command_allocator, NULL, IID_PPV_ARGS(&g_pD3DCommandList)) != S_OK || g_pD3DCommandList->Close() != S_OK) {
					return OriginalPresent(pSwapChain, SyncInterval, Flags);
				}
			}

			IMGUI_CHECKVERSION();
			ImGui::CreateContext();
			ImGuiIO& io = ImGui::GetIO();
			io.IniFilename = nullptr;
			//io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls
			//io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;      // Enable Gamepad Controls

			// Setup Dear ImGui style
			ImGui::StyleColorsDark();
			//ImGui::StyleColorsClassic();

				// Setup Platform/Renderer backends
			auto fonts = ImGui::GetIO().Fonts;
			fonts->AddFontFromFileTTF(
				"c:/windows/fonts/simhei.ttf",
				13.0f,
				NULL,
				fonts->GetGlyphRangesChineseFull()
			);
			ImGui_ImplWin32_Init(Window);
			ImGui_ImplDX12_Init(pD3DDevice, g_FrameBufferCount,
				DXGI_FORMAT_R8G8B8A8_UNORM, g_pD3DSrvDescHeap,
				g_pD3DSrvDescHeap->GetCPUDescriptorHandleForHeapStart(),
				g_pD3DSrvDescHeap->GetGPUDescriptorHandleForHeapStart());

			g_Initialized = true;

			pD3DDevice->Release();
		}

		ImGui_ImplWin32_NewFrame();
		ImGui_ImplDX12_NewFrame();
		ImGui::NewFrame();

		if (LuaCore::reloadTime != 0 && LuaCore::reload != LuaCore::reloadTime) {
			LuaCore::reload = LuaCore::reloadTime;
			LuaCore::initUI = false;
		}
		if (!LuaCore::initUI)
		{
			//绑定imgui
			LuaCore::Imgui_Bindings();
			//绑定纹理获取
			for (std::string file_name : LuaCore::getLuaFils()) {
				LuaCore::LuaScriptData luae = LuaCore::getLuas()[file_name];
				if (luae.start) {
					sol::state_view lua(luae.L);
					lua.set_function("LoadTexture", LoadTexture);
				}
			}
		}
		LuaCore::run("on_imgui");

		FrameContext& currentFrameContext = g_FrameContext[pSwapChain->GetCurrentBackBufferIndex()];
		currentFrameContext.command_allocator->Reset();

		D3D12_RESOURCE_BARRIER barrier;
		barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
		barrier.Transition.pResource = currentFrameContext.main_render_target_resource;
		barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		g_pD3DCommandList->Reset(currentFrameContext.command_allocator, nullptr);
		g_pD3DCommandList->ResourceBarrier(1, &barrier);
		g_pD3DCommandList->OMSetRenderTargets(1, &currentFrameContext.main_render_target_descriptor, FALSE, nullptr);
		g_pD3DCommandList->SetDescriptorHeaps(1, &g_pD3DSrvDescHeap);
		ImGui::Render();
		ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), g_pD3DCommandList);
		barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		g_pD3DCommandList->ResourceBarrier(1, &barrier);
		g_pD3DCommandList->Close();

		g_pD3DCommandQueue->ExecuteCommandLists(1, (ID3D12CommandList**)&g_pD3DCommandList);
		return OriginalPresent(pSwapChain, SyncInterval, Flags);
	}

	void HookExecuteCommandLists(ID3D12CommandQueue* queue, UINT NumCommandLists, ID3D12CommandList* ppCommandLists) {
		if (!g_pD3DCommandQueue && queue->GetDesc().Type == D3D12_COMMAND_LIST_TYPE_DIRECT) {
			g_pD3DCommandQueue = queue;
		}

		OriginalExecuteCommandLists(queue, NumCommandLists, ppCommandLists);
	}

	void ResetState() {
		if (g_Initialized) {
			g_Initialized = false;
			ImGui_ImplWin32_Shutdown();
			ImGui_ImplDX12_Shutdown();
		}
		pD3DDevice = nullptr;
		g_pD3DCommandQueue = nullptr;
		g_FrameContext.clear();
		g_pD3DCommandList = nullptr;
		g_pD3DRtvDescHeap = nullptr;
		g_pD3DSrvDescHeap = nullptr;
	}

	long HookResizeBuffers(IDXGISwapChain3* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
		ResetState();
		return OriginalResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
	}

	Status Init() {
		WNDCLASSEX windowClass;
		windowClass.cbSize = sizeof(WNDCLASSEX);
		windowClass.style = CS_HREDRAW | CS_VREDRAW;
		windowClass.lpfnWndProc = DefWindowProc;
		windowClass.cbClsExtra = 0;
		windowClass.cbWndExtra = 0;
		windowClass.hInstance = GetModuleHandle(NULL);
		windowClass.hIcon = NULL;
		windowClass.hCursor = NULL;
		windowClass.hbrBackground = NULL;
		windowClass.lpszMenuName = NULL;
		windowClass.lpszClassName = L"Fake Window";
		windowClass.hIconSm = NULL;

		::RegisterClassEx(&windowClass);

		HWND window = ::CreateWindow(windowClass.lpszClassName, L"Fake DirectX Window", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL, windowClass.hInstance, NULL);

		HMODULE libDXGI;
		HMODULE libD3D12;

		if ((libDXGI = ::GetModuleHandle(L"dxgi.dll")) == NULL) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::ModuleNotFoundError;
		}

		if ((libD3D12 = ::GetModuleHandle(L"d3d12.dll")) == NULL) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::ModuleNotFoundError;
		}

		void* CreateDXGIFactory;
		if ((CreateDXGIFactory = ::GetProcAddress(libDXGI, "CreateDXGIFactory")) == NULL) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		CComPtr<IDXGIFactory> factory;
		if (((long(__stdcall*)(const IID&, void**))(CreateDXGIFactory))(__uuidof(IDXGIFactory), (void**)&factory) < 0) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		CComPtr<IDXGIAdapter> adapter;
		if (factory->EnumAdapters(0, &adapter) == DXGI_ERROR_NOT_FOUND) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		void* D3D12CreateDevice;
		if ((D3D12CreateDevice = ::GetProcAddress(libD3D12, "D3D12CreateDevice")) == NULL) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		CComPtr<ID3D12Device> device;
		if (((long(__stdcall*)(IUnknown*, D3D_FEATURE_LEVEL, const IID&, void**))(D3D12CreateDevice))(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&device) < 0) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		D3D12_COMMAND_QUEUE_DESC queueDesc;
		queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
		queueDesc.Priority = 0;
		queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
		queueDesc.NodeMask = 0;

		CComPtr<ID3D12CommandQueue> commandQueue;
		if (device->CreateCommandQueue(&queueDesc, __uuidof(ID3D12CommandQueue), (void**)&commandQueue) < 0) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		CComPtr<ID3D12CommandAllocator> commandAllocator;
		if (device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), (void**)&commandAllocator) < 0) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		CComPtr<ID3D12GraphicsCommandList> commandList;
		if (device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, commandAllocator, NULL, __uuidof(ID3D12GraphicsCommandList), (void**)&commandList) < 0) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		DXGI_RATIONAL refreshRate;
		refreshRate.Numerator = 60;
		refreshRate.Denominator = 1;

		DXGI_MODE_DESC bufferDesc;
		bufferDesc.Width = 100;
		bufferDesc.Height = 100;
		bufferDesc.RefreshRate = refreshRate;
		bufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
		bufferDesc.ScanlineOrdering = DXGI_MODE_SCANLINE_ORDER_UNSPECIFIED;
		bufferDesc.Scaling = DXGI_MODE_SCALING_UNSPECIFIED;

		DXGI_SAMPLE_DESC sampleDesc;
		sampleDesc.Count = 1;
		sampleDesc.Quality = 0;

		DXGI_SWAP_CHAIN_DESC swapChainDesc = {};
		swapChainDesc.BufferDesc = bufferDesc;
		swapChainDesc.SampleDesc = sampleDesc;
		swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
		swapChainDesc.BufferCount = 2;
		swapChainDesc.OutputWindow = window;
		swapChainDesc.Windowed = 1;
		swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
		swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;

		CComPtr<IDXGISwapChain> swapChain;
		if (factory->CreateSwapChain(commandQueue, &swapChainDesc, &swapChain) < 0) {
			::DestroyWindow(window);
			::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
			return Status::UnknownError;
		}

		g_MethodsTable = (uint64_t*)::calloc(150, sizeof(uint64_t));
		::memcpy(g_MethodsTable, *(uint64_t**)(void*)device, 44 * sizeof(uint64_t));
		::memcpy(g_MethodsTable + 44, *(uint64_t**)(void*)commandQueue, 19 * sizeof(uint64_t));
		::memcpy(g_MethodsTable + 44 + 19, *(uint64_t**)(void*)commandAllocator, 9 * sizeof(uint64_t));
		::memcpy(g_MethodsTable + 44 + 19 + 9, *(uint64_t**)(void*)commandList, 60 * sizeof(uint64_t));
		::memcpy(g_MethodsTable + 44 + 19 + 9 + 60, *(uint64_t**)(void*)swapChain, 18 * sizeof(uint64_t));

		::DestroyWindow(window);
		::UnregisterClass(windowClass.lpszClassName, windowClass.hInstance);
		return Status::Success;
	}

	Status Hook(uint16_t _index, void** _original, void* _function) {
		void* target = (void*)g_MethodsTable[_index];
#ifdef USE_DETOURS
		* _original = target;
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourAttach(&(PVOID&)*_original, _function);
		DetourTransactionCommit();
#endif
#ifdef USE_MINHOOK
		if (MH_CreateHook(target, _function, _original) != MH_OK || MH_EnableHook(target) != MH_OK) {
			return Status::UnknownError;
		}
#endif
		return Status::Success;
	}

	Status Unhook(uint16_t _index, void** _original, void* _function) {
		void* target = (void*)g_MethodsTable[_index];
#ifdef USE_DETOURS
		DetourTransactionBegin();
		DetourUpdateThread(GetCurrentThread());
		DetourDetach(&(PVOID&)*_original, _function);
		DetourTransactionCommit();
#endif
#ifdef USE_MINHOOK
		MH_DisableHook(target);
#endif
		return Status::Success;
	}

	LRESULT APIENTRY WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
		if (g_Initialized) {
			ImGuiIO& io = ImGui::GetIO();
			switch (msg) {
			case WM_LBUTTONDOWN:
				io.MouseDown[0] = true;
				return io.WantCaptureMouse ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_LBUTTONUP:
				io.MouseDown[0] = false;
				return io.WantCaptureMouse ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_RBUTTONDOWN:
				io.MouseDown[1] = true;
				return io.WantCaptureMouse ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_RBUTTONUP:
				io.MouseDown[1] = false;
				return io.WantCaptureMouse ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_MBUTTONDOWN:
				io.MouseDown[2] = true;
				return io.WantCaptureMouse ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_MBUTTONUP:
				io.MouseDown[2] = false;
				return io.WantCaptureMouse ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_MOUSEWHEEL:
				io.MouseWheel += GET_WHEEL_DELTA_WPARAM(wParam) > 0 ? +1.0f : -1.0f;
				return io.WantCaptureMouse ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_MOUSEMOVE:
				io.MousePos.x = (signed short)(lParam);
				io.MousePos.y = (signed short)(lParam >> 16);
				return io.WantCaptureMouse ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_KEYDOWN:
				if (wParam < 256)
					io.KeysDown[wParam] = 1;
				return io.WantCaptureKeyboard ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_KEYUP:
				if (wParam < 256)
					io.KeysDown[wParam] = 0;
				return io.WantCaptureKeyboard ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			case WM_CHAR:
				// You can also use ToAscii()+GetKeyboardState() to retrieve characters.
				if (wParam > 0 && wParam < 0x10000)
					io.AddInputCharacter((unsigned short)wParam);
				return io.WantCaptureKeyboard ? 0 : CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
			}
		}
		return CallWindowProc(OriginalWndProc, hWnd, msg, wParam, lParam);
	}

	Status InstallHooks() {
#ifdef USE_DETOURS
		DetourRestoreAfterWith();
#endif
#ifdef USE_MINHOOK
		MH_Initialize();
#endif

		Hook(54, (void**)&OriginalExecuteCommandLists, HookExecuteCommandLists);
		Hook(140, (void**)&OriginalPresent, HookPresent);
		Hook(145, (void**)&OriginalResizeBuffers, HookResizeBuffers);
		
		return Status::Success;
	}

	Status RemoveHooks() {
		Unhook(54, (void**)&OriginalExecuteCommandLists, HookExecuteCommandLists);
		Unhook(140, (void**)&OriginalPresent, HookPresent);
		Unhook(145, (void**)&OriginalResizeBuffers, HookResizeBuffers);

		if (Window && OriginalWndProc) {
			SetWindowLongPtr(Window, GWLP_WNDPROC, (__int3264)(LONG_PTR)OriginalWndProc);
		}

		ResetState();
		ImGui::DestroyContext();

#ifdef USE_MINHOOK
		MH_Uninitialize();
#endif
		//wait for hooks to finish if in one. maybe not needed, but seemed more stable after adding it.
		Sleep(1000);
		return Status::Success;
	}

}