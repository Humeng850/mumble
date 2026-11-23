// Copyright 2013-2023 The Mumble Developers. All rights reserved.
// Modified by Humeng850, 2026: added IDXGISwapChain1::Present1 hooking so that
// engines using the DXGI 1.2+ flip model get the overlay drawn.
// Use of this source code is governed by a BSD-style license
// that can be found in the LICENSE file at the root of the
// Mumble source tree or at <https://www.mumble.info/LICENSE>.

/* Copyright (C) 2005-2010, Thorvald Natvig <thorvald@natvig.com>
   Copyright (C) 2011, Kissaki <kissaki@gmx.de>
   Copyright (C) 2011, Nye Liu <nyet@nyet.org>

   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   - Redistributions of source code must retain the above copyright notice,
	 this list of conditions and the following disclaimer.
   - Redistributions in binary form must reproduce the above copyright notice,
	 this list of conditions and the following disclaimer in the documentation
	 and/or other materials provided with the distribution.
   - Neither the name of the Mumble Developers nor the names of its
	 contributors may be used to endorse or promote products derived from this
	 software without specific prior written permission.

   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
   ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
   A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE FOUNDATION OR
   CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
   EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
   PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
   PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF
   LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING
   NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
   SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#include "lib.h"
#include <dxgi.h>
// === 补丁：Present1 需要 ===
#include <d3d11.h>
#include <dxgi1_2.h>

DXGIData *dxgi = nullptr;

static bool bHooked = false;
static HardHook hhPresent;
static HardHook hhResize;
// === 补丁：Present1 ===
static HardHook hhPresent1;
/// 0 = 还没注入，非 0 = 已经注入/正在注入（HardHook::setup 只能调用一次）
static volatile LONG g_present1Injected = 0;
static volatile LONG iPresentCalls       = 0;
static volatile LONG iPresent1Calls      = 0;

typedef HRESULT(__stdcall *CreateDXGIFactory1Type)(REFIID, void **);

typedef HRESULT(__stdcall *PresentType)(IDXGISwapChain *, UINT, UINT);

// === 补丁：Present1（只换 DLL 版，偏移在目标进程内自算）===
typedef HRESULT(__stdcall *Present1Type)(IDXGISwapChain1 *, UINT, UINT, const DXGI_PRESENT_PARAMETERS *);
typedef HRESULT(__stdcall *D3D11CreateDeviceAndSwapChainProbeType)(
	IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL *, UINT, UINT,
	const DXGI_SWAP_CHAIN_DESC *, IDXGISwapChain **, ID3D11Device **, D3D_FEATURE_LEVEL *,
	ID3D11DeviceContext **);
// 前置声明：myPresent 里要用（真实 swapchain 兜底探测）
static void TryHookPresent1FromSwapChain(IDXGISwapChain *pSwapChain);
typedef HRESULT(__stdcall *ResizeBuffersType)(IDXGISwapChain *, UINT, UINT, UINT, DXGI_FORMAT, UINT);

#define HMODREF(mod, func) func##Type p##func = (func##Type) GetProcAddress(mod, #func)

// From d3d10.cpp
extern void presentD3D10(IDXGISwapChain *pSwapChain);

// From d3d11.cpp
extern void presentD3D11(IDXGISwapChain *pSwapChain);

static HRESULT __stdcall myPresent(IDXGISwapChain *pSwapChain, UINT SyncInterval, UINT Flags) {
// Present is called for each frame. Thus, we do not want to always log here.
	{
		LONG n = InterlockedIncrement(&iPresentCalls);
		if (n <= 5) {
			ods("DXGI-P1: Present call #%d", (int) n);
		} else if (n % 600 == 0) {
			ods("DXGI-P1: Present calls = %d (引擎走 Present 路径)", (int) n);
		}
	}
	// 有人调用 Present 说明现在手里就有活着的 swapchain —— 用它兜底探测 Present1
	if (g_present1Injected == 0) {
		TryHookPresent1FromSwapChain(pSwapChain);
	}
#ifdef EXTENDED_OVERLAY_DEBUGOUTPUT
	ods("DXGI: Call to Present; Drawing and then chaining the call to the original logic");
#endif

	// From this D3D-version-independent point, call the present logic for both
	// D3D 10 and 11. Those that apply will be executed adequately, those that
	// do not will (silently) fail.
	presentD3D10(pSwapChain);
	presentD3D11(pSwapChain);

	// TODO: Move logic to HardHook.
	// Call base without active hook in case of no trampoline.
	PresentType oPresent = (PresentType) hhPresent.call;
	hhPresent.restore();
	HRESULT hr = oPresent(pSwapChain, SyncInterval, Flags);
	hhPresent.inject();

	return hr;
}

// From d3d10.cpp
extern void resizeD3D10(IDXGISwapChain *pSwapChain);
// From d3d11.cpp
extern void resizeD3D11(IDXGISwapChain *pSwapChain);

// ================================================================
// 补丁：Present1 支持
//
// 背景：Mumble 的 overlay 只 patch 了 IDXGISwapChain::Present（vtable 槽 8）。
// 使用 DXGI 1.2+ flip model 的引擎（如 EVE Online）每帧走的是
// IDXGISwapChain1::Present1（槽 22），于是 overlay 的绘制回调一次都不会被调用。
//
// 只换 DLL 的约束：不能依赖 Mumble 进程算好偏移经共享内存传过来
// （那需要改 DXGIData 结构，会导致只换 DLL 时读写越界）。
// 所以这里在目标进程内自己算：
//   1) 造一个临时 D3D11 设备 + swapchain，QueryInterface 到 IDXGISwapChain1，
//      取 vtable 槽 22 —— 上游 PrepareDXGI11() 用的就是这套手法，只是跑在 Mumble 进程里；
//   2) 用 GetModuleHandleExW(FROM_ADDRESS) 校验该地址确实落在本进程的 dxgi.dll 内；
//   3) 直接以绝对地址交给 HardHook（同进程内无需算偏移）。
// ================================================================

static HRESULT __stdcall myPresent1(IDXGISwapChain1 *pSwapChain1, UINT SyncInterval, UINT Flags,
									const DXGI_PRESENT_PARAMETERS *pPresentParameters) {
	{
		LONG n = InterlockedIncrement(&iPresent1Calls);
		if (n <= 5) {
			ods("DXGI-P1: Present1 call #%d", (int) n);
		} else if (n % 600 == 0) {
			ods("DXGI-P1: Present1 calls = %d (引擎走 Present1 路径)", (int) n);
		}
	}

	// 和 myPresent 一致：先画，再串回原逻辑
	presentD3D10(reinterpret_cast< IDXGISwapChain * >(pSwapChain1));
	presentD3D11(reinterpret_cast< IDXGISwapChain * >(pSwapChain1));

	Present1Type oPresent1 = (Present1Type) hhPresent1.call;
	hhPresent1.restore();
	HRESULT hr = oPresent1(pSwapChain1, SyncInterval, Flags, pPresentParameters);
	hhPresent1.inject();

	return hr;
}

static void HookPresent1Raw(voidFunc vfPresent1) {
	if (InterlockedCompareExchange(&g_present1Injected, 1, 0) != 0) {
		return; // 已经注入过，绝不重复 setup
	}
	ods("DXGI-P1: Injecting Present1 hook at %p", reinterpret_cast< void * >(vfPresent1));
	hhPresent1.setup(vfPresent1, reinterpret_cast< voidFunc >(myPresent1));
}

/// 用真实 swapchain（已存在）取 Present1 并挂钩子。零设备创建，最安全。
static void TryHookPresent1FromSwapChain(IDXGISwapChain *pSwapChain) {
	if (g_present1Injected != 0 || !pSwapChain) {
		return;
	}
	IDXGISwapChain1 *pSwapChain1 = nullptr;
	if (FAILED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain1),
										  reinterpret_cast< void ** >(&pSwapChain1)))
		|| !pSwapChain1) {
		ods("DXGI-P1: swapchain does not expose IDXGISwapChain1");
		return;
	}
	void ***vtbl1   = (void ***) pSwapChain1;
	void *pPresent1 = (*vtbl1)[22];
	pSwapChain1->Release();
	if (!pPresent1) {
		return;
	}
	HookPresent1Raw(reinterpret_cast< voidFunc >(pPresent1));
}

/// 在目标进程内造一个临时 D3D11 设备 + swapchain，取 Present1 的绝对地址。
/// 失败返回 nullptr（不影响游戏，后续还有 swapchain 兜底路径）。
static void *ProbePresent1InProcess() {
	// 刻意不 LoadLibrary：避免触发我们自己的 LoadLibrary 钩子造成递归。
	// d3d11.dll 只有在游戏自己加载过之后才走这条；没加载就交给兜底路径。
	HMODULE hD3D11 = GetModuleHandleW(L"d3d11.dll");
	if (!hD3D11) {
		return nullptr;
	}
	D3D11CreateDeviceAndSwapChainProbeType pCreate =
		reinterpret_cast< D3D11CreateDeviceAndSwapChainProbeType >(
			GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain"));
	if (!pCreate) {
		ods("DXGI-P1: D3D11CreateDeviceAndSwapChain not found");
		return nullptr;
	}

	HWND hwnd = CreateWindowExW(0, L"STATIC", L"MumblePresent1Probe", WS_POPUP, 0, 0, 64, 64, nullptr,
								nullptr, nullptr, nullptr);
	if (!hwnd) {
		ods("DXGI-P1: probe window creation failed");
		return nullptr;
	}

	DXGI_SWAP_CHAIN_DESC desc;
	ZeroMemory(&desc, sizeof(desc));
	desc.BufferDesc.Width                   = 64;
	desc.BufferDesc.Height                  = 64;
	desc.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
	desc.SampleDesc.Count                   = 1;
	desc.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	desc.BufferCount                        = 2;
	desc.OutputWindow                       = hwnd;
	desc.Windowed                           = TRUE;
	desc.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

	IDXGISwapChain *pSwapChain         = nullptr;
	ID3D11Device *pDevice              = nullptr;
	ID3D11DeviceContext *pContext      = nullptr;
	D3D_FEATURE_LEVEL featureLevel     = D3D_FEATURE_LEVEL_11_0;

	HRESULT hr = pCreate(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
						 &desc, &pSwapChain, &pDevice, &featureLevel, &pContext);
	if (FAILED(hr)) {
		// 无独显 / 远程桌面 / 驱动异常时退回软件光栅化（同样是 dxgi.dll 的实现）
		hr = pCreate(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &desc,
					 &pSwapChain, &pDevice, &featureLevel, &pContext);
		if (FAILED(hr)) {
			ods("DXGI-P1: probe device creation failed (hr=0x%08X)", (unsigned) hr);
		}
	}

	void *pPresent1 = nullptr;
	if (pSwapChain) {
		IDXGISwapChain1 *pSwapChain1 = nullptr;
		if (SUCCEEDED(pSwapChain->QueryInterface(__uuidof(IDXGISwapChain1),
												 reinterpret_cast< void ** >(&pSwapChain1)))
			&& pSwapChain1) {
			void ***vtbl1   = (void ***) pSwapChain1;
			void *candidate = (*vtbl1)[22];
			pSwapChain1->Release();

			if (candidate) {
				// 校验它确实在 dxgi.dll 里，避免拿到别的模块的地址
				HMODULE hOwner = nullptr;
				if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
										   | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
									   reinterpret_cast< LPCWSTR >(candidate), &hOwner)
					&& hOwner && hOwner == GetModuleHandleW(L"dxgi.dll")) {
					pPresent1 = candidate;
				} else {
					ods("DXGI-P1: vtable[22] not inside dxgi.dll (owner=%p)", hOwner);
				}
			}
		} else {
			ods("DXGI-P1: probe swapchain does not expose IDXGISwapChain1");
		}
	}

	if (pContext) {
		pContext->Release();
	}
	if (pDevice) {
		pDevice->Release();
	}
	if (pSwapChain) {
		pSwapChain->Release();
	}
	DestroyWindow(hwnd);
	return pPresent1;
}

static DWORD WINAPI Present1ProbeThread(LPVOID) {
	for (int i = 0; i < 60; ++i) { // 最多约 30 秒
		if (g_present1Injected != 0) {
			return 0;
		}
		void *p = ProbePresent1InProcess();
		if (p) {
			HookPresent1Raw(reinterpret_cast< voidFunc >(p));
			return 0;
		}
		Sleep(500);
	}
	ods("DXGI-P1: gave up probing Present1 (fallback: real swapchain on Present)");
	return 0;
}

/// 进入 hookDXGI 时调用：先同步试一次，不行再起线程等 d3d11.dll 就位。
static void StartPresent1Detection() {
	if (g_present1Injected != 0) {
		return;
	}
	void *p = ProbePresent1InProcess();
	if (p) {
		HookPresent1Raw(reinterpret_cast< voidFunc >(p));
		return;
	}
	HANDLE h = CreateThread(nullptr, 0, Present1ProbeThread, nullptr, 0, nullptr);
	if (h) {
		CloseHandle(h);
	}
	ods("DXGI-P1: d3d11.dll not ready, probing in background");
}

static HRESULT __stdcall myResize(IDXGISwapChain *pSwapChain, UINT BufferCount, UINT Width, UINT Height,
								  DXGI_FORMAT NewFormat, UINT SwapChainFlags) {
#ifdef EXTENDED_OVERLAY_DEBUGOUTPUT
	ods("DXGI: Call to Resize. Forwarding to D3D10 and D3D11 custom implementations before calling original logic ...");
#endif

	resizeD3D10(pSwapChain);
	resizeD3D11(pSwapChain);

	// TODO: Move logic to HardHook.
	// Call base without active hook in case of no trampoline.
	ResizeBuffersType oResize = (ResizeBuffersType) hhResize.call;
	hhResize.restore();
	HRESULT hr = oResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
	hhResize.inject();
	return hr;
}

static void HookPresentRaw(voidFunc vfPresent) {
	ods("DXGI: Injecting Present");
	hhPresent.setup(vfPresent, reinterpret_cast< voidFunc >(myPresent));
}

static void HookResizeRaw(voidFunc vfResize) {
	ods("DXGI: Injecting ResizeBuffers Raw");
	hhResize.setup(vfResize, reinterpret_cast< voidFunc >(myResize));
}

void hookDXGI(HMODULE hDXGI, bool preonly);

void checkDXGIHook(bool preonly) {
	static bool bCheckHookActive = false;
	if (bCheckHookActive) {
		ods("DXGI: Recursion in checkDXGIHook");
		return;
	}

	if (dxgi->offsetPresent == 0 || dxgi->offsetResize == 0)
		return;

	bCheckHookActive = true;

	HMODULE hDXGI = GetModuleHandleW(L"DXGI.DLL");

	if (hDXGI) {
		if (!bHooked) {
			hookDXGI(hDXGI, preonly);
		}
#ifdef EXTENDED_OVERLAY_DEBUGOUTPUT
	} else {
		ods("DXGI: No DXGI.DLL found as loaded. No hooking at this point.");
#endif
	}

	bCheckHookActive = false;
}

/// @param hDXGI must be a valid module handle.
void hookDXGI(HMODULE hDXGI, bool preonly) {
	wchar_t modulename[MODULEFILEPATH_BUFLEN];
	GetModuleFileNameW(nullptr, modulename, ARRAY_NUM_ELEMENTS(modulename));
	ods("DXGI: hookDXGI in App '%ls'", modulename);

	// Add a ref to ourselves; we do NOT want to get unloaded directly from this process.
	HMODULE hTempSelf = nullptr;
	GetModuleHandleEx(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast< LPCTSTR >(&hookDXGI), &hTempSelf);

	bHooked = true;

	// Can we use the prepatch data?
	GetModuleFileNameW(hDXGI, modulename, ARRAY_NUM_ELEMENTS(modulename));
	if (_wcsicmp(dxgi->wcFileName, modulename) == 0) {
		// The module seems to match the one we prepared d3dd for.

		unsigned char *raw = (unsigned char *) hDXGI;
		HookPresentRaw((voidFunc)(raw + dxgi->offsetPresent));
		HookResizeRaw((voidFunc)(raw + dxgi->offsetResize));
		// === 补丁：现代 DXGI 引擎走 Present1，同样要挂 ===
		StartPresent1Detection();

	} else if (!preonly) {
		ods("DXGI: Interface changed, can't rawpatch. Current: %ls ; Previously: %ls", modulename, dxgi->wcFileName);
	} else {
		bHooked = false;
	}
}

// From d3d10.cpp
extern void PrepareDXGI10(IDXGIAdapter1 *pAdapter, bool initializeDXGIData);
// From d3d11.cpp
extern void PrepareDXGI11(IDXGIAdapter1 *pAdapter, bool initializeDXGIData);

/// This function is called by the Mumble client in Mumble's scope
/// mainly to extract the offsets of various functions in the IDXGISwapChain
/// and IDXGIObject interfaces that need to be hooked in target
/// applications. The data is stored in the dxgi shared memory structure.
extern "C" __declspec(dllexport) void __cdecl PrepareDXGI() {
	if (!dxgi)
		return;

	ods("DXGI: Preparing static data for DXGI Injection");

	dxgi->wcFileName[0] = 0;
	dxgi->offsetPresent = 0;
	dxgi->offsetResize  = 0;

	// Make sure this is Vista or greater as quite a number of <=WinXP users have fake DX10 libs installed
	OSVERSIONINFOEXW ovi;
	memset(&ovi, 0, sizeof(ovi));
	ovi.dwOSVersionInfoSize = sizeof(ovi);
	GetVersionExW(reinterpret_cast< OSVERSIONINFOW * >(&ovi));
	if (ovi.dwMajorVersion < 6 || (ovi.dwMajorVersion == 6 && ovi.dwBuildNumber < 6001)) {
		ods("DXGI: No DXGI pre-Vista - skipping prepare");
		return;
	}

	HMODULE hDXGI = LoadLibrary("DXGI.DLL");

	if (hDXGI) {
		GetModuleFileNameW(hDXGI, dxgi->wcFileName, ARRAY_NUM_ELEMENTS(dxgi->wcFileName));

		CreateDXGIFactory1Type pCreateDXGIFactory1 =
			reinterpret_cast< CreateDXGIFactory1Type >(GetProcAddress(hDXGI, "CreateDXGIFactory1"));
		ods("DXGI: Got CreateDXGIFactory1 at %p", pCreateDXGIFactory1);
		if (pCreateDXGIFactory1) {
			IDXGIFactory1 *pFactory;
			HRESULT hr = pCreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **) (&pFactory));
			if (FAILED(hr))
				ods("DXGI: Call to pCreateDXGIFactory1 failed!");
			if (pFactory) {
				IDXGIAdapter1 *pAdapter = nullptr;
				pFactory->EnumAdapters1(0, &pAdapter);

				/// Offsets have to be identified and initialized only once.
				bool initializeDXGIData = !dxgi->offsetPresent && !dxgi->offsetResize;
				PrepareDXGI10(pAdapter, initializeDXGIData);
				initializeDXGIData = !dxgi->offsetPresent && !dxgi->offsetResize;
				PrepareDXGI11(pAdapter, initializeDXGIData);

				pFactory->Release();
			} else {
				FreeLibrary(hDXGI);
			}
		} else {
			FreeLibrary(hDXGI);
		}
	} else {
		FreeLibrary(hDXGI);
	}
}
