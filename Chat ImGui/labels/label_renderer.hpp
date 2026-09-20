#pragma once

#include <Windows.h>
#include <d3d9.h>

#include <sampapi/CRect.h>

namespace sampapi::v037r3 {
class CFonts;
}

namespace westland_labels {

void renderer_shutdown();
void renderer_on_device_lost();
void renderer_on_imgui_initialized();
void renderer_render_in_chat_frame();
void renderer_on_present(IDirect3DDevice9* device);

bool renderer_is_custom_ready();
bool renderer_can_replace_labels();

void renderer_enter_label_draw();
void renderer_leave_label_draw();
bool renderer_should_replace_little_text();

bool renderer_queue_little_text(
	sampapi::v037r3::CFonts* fonts,
	const char* text,
	const sampapi::CRect& rect,
	int format,
	D3DCOLOR color,
	BOOL shadow);

} // namespace westland_labels
