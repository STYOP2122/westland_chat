#include "hooks.hpp"
#include "label_renderer.hpp"

#include <kthook/kthook.hpp>

#include <sampapi/0.3.7-R3-1/CLabel.h>
#include <sampapi/0.3.7-R3-1/CFonts.h>
#include <sampapi/CVector.h>
#include <sampapi/CRect.h>
#include <sampapi/sampapi.h>

#include <d3d9.h>

#include <Windows.h>

namespace westland_labels {
namespace {

using CLabelDrawFn = void(__thiscall*)(
	sampapi::v037r3::CLabel*,
	sampapi::CVector*,
	const char*,
	D3DCOLOR,
	BOOL,
	bool);

using DrawLittleTextFn = void(__thiscall*)(
	sampapi::v037r3::CFonts*,
	ID3DXSprite*,
	const char*,
	sampapi::CRect,
	int,
	D3DCOLOR,
	BOOL);

using PresentFn = HRESULT(__stdcall*)(IDirect3DDevice9*, const RECT*, const RECT*, HWND, const RGNDATA*);

kthook::kthook_simple<CLabelDrawFn> g_label_draw_hook;
kthook::kthook_simple<DrawLittleTextFn> g_draw_little_text_hook;
kthook::kthook_simple<PresentFn> g_present_hook;

bool g_hooks_installed = false;
bool g_render_hooks_installed = false;

constexpr int kD3D9PresentIndex = 17;

bool is_readable_pointer(const void* ptr, std::size_t size = sizeof(void*)) {
	if (!ptr) {
		return false;
	}

	MEMORY_BASIC_INFORMATION info{};
	if (VirtualQuery(ptr, &info, sizeof(info)) == 0) {
		return false;
	}

	if (info.State != MEM_COMMIT) {
		return false;
	}

	const DWORD protect = info.Protect & 0xFF;
	if (protect == PAGE_NOACCESS || protect == PAGE_GUARD) {
		return false;
	}

	const auto addr = reinterpret_cast<std::uintptr_t>(ptr);
	const auto region_end = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
	return addr + size <= region_end;
}

IDirect3DDevice9* get_game_device() {
	auto* device = *reinterpret_cast<IDirect3DDevice9**>(0xC97C28U);
	if (!is_readable_pointer(device)) {
		return nullptr;
	}

	return device;
}

bool is_device_usable(IDirect3DDevice9* device) {
	if (!device) {
		return false;
	}

	const HRESULT cooperative = device->TestCooperativeLevel();
	return cooperative != D3DERR_DEVICELOST && cooperative != D3DERR_DEVICENOTRESET && SUCCEEDED(cooperative);
}

HRESULT __stdcall present_hooked(
	const kthook::kthook_simple<PresentFn>& hook,
	IDirect3DDevice9* device,
	const RECT* src_rect,
	const RECT* dst_rect,
	HWND window,
	const RGNDATA* dirty_region) {
	if (is_device_usable(device)) {
		__try {
			renderer_on_present(device);
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
			renderer_on_device_lost();
		}
	}
	else {
		renderer_on_device_lost();
	}

	return hook.get_trampoline()(device, src_rect, dst_rect, window, dirty_region);
}

void label_draw_hooked(
	const kthook::kthook_simple<CLabelDrawFn>& hook,
	sampapi::v037r3::CLabel* self,
	sampapi::CVector* position,
	const char* text,
	D3DCOLOR color,
	BOOL shadow,
	bool no_obstacles) {
	const bool replace = renderer_can_replace_labels();
	if (replace) {
		renderer_enter_label_draw();
	}

	__try {
		hook.get_trampoline()(self, position, text, color, shadow, no_obstacles);
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
	}

	if (replace) {
		renderer_leave_label_draw();
	}
}

void draw_little_text_hooked(
	const kthook::kthook_simple<DrawLittleTextFn>& hook,
	sampapi::v037r3::CFonts* self,
	ID3DXSprite* sprite,
	const char* text,
	sampapi::CRect rect,
	int format,
	D3DCOLOR color,
	BOOL shadow) {
	if (renderer_should_replace_little_text()) {
		__try {
			if (renderer_queue_little_text(self, text, rect, format, color, shadow)) {
				return;
			}
		}
		__except (EXCEPTION_EXECUTE_HANDLER) {
		}
	}

	hook.get_trampoline()(self, sprite, text, rect, format, color, shadow);
}

bool install_render_hooks() {
	if (g_render_hooks_installed) {
		return true;
	}

	auto* device = get_game_device();
	if (!device) {
		return false;
	}

	void** vtable = *reinterpret_cast<void***>(device);
	if (!is_readable_pointer(vtable) || !is_readable_pointer(vtable[kD3D9PresentIndex])) {
		return false;
	}

	g_present_hook.set_dest(vtable[kD3D9PresentIndex]);
	g_present_hook.set_cb(&present_hooked);
	if (!g_present_hook.install()) {
		return false;
	}

	g_render_hooks_installed = true;
	return true;
}

bool install_label_hooks() {
	if (g_hooks_installed) {
		return true;
	}

	if (!GetModuleHandleA("samp.dll")) {
		return false;
	}

	const auto label_draw = reinterpret_cast<CLabelDrawFn>(sampapi::GetAddress(0x6B520));
	g_label_draw_hook.set_dest(label_draw);
	g_label_draw_hook.set_cb(&label_draw_hooked);
	if (!g_label_draw_hook.install()) {
		return false;
	}

	const auto draw_little_text = reinterpret_cast<DrawLittleTextFn>(sampapi::GetAddress(0x6AD70));
	g_draw_little_text_hook.set_dest(draw_little_text);
	g_draw_little_text_hook.set_cb(&draw_little_text_hooked);
	if (!g_draw_little_text_hook.install()) {
		g_label_draw_hook.remove();
		return false;
	}

	g_hooks_installed = true;
	return true;
}

} // namespace

bool hooks_install() {
	const bool labels = install_label_hooks();
	const bool render = install_render_hooks();
	return labels && render;
}

void hooks_remove() {
	g_present_hook.remove();
	g_draw_little_text_hook.remove();
	g_label_draw_hook.remove();
	g_render_hooks_installed = false;
	g_hooks_installed = false;
	renderer_shutdown();
}

} // namespace westland_labels
