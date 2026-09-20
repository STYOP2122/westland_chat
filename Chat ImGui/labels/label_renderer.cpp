#include "label_renderer.hpp"

#include "game.hpp"
#include "../snippets.hpp"

#include <imgui.h>
#include <imgui_impl_dx9.h>
#include <imgui_impl_win32.h>

#include <sampapi/0.3.7-R3-1/CFonts.h>
#include <sampapi/0.3.7-R3-1/CNetGame.h>
#include <sampapi/sampapi.h>

#include <ShlObj.h>

#include <Windows.h>

#include <cctype>
#include <cfloat>
#include <cstdlib>
#include <string>
#include <vector>

namespace westland_labels {
namespace {

constexpr float kLabelPickupCenterYOffset = 34.0f;
constexpr int kSampLittleFontFallbackHeight = 10;
constexpr std::size_t kMaxLabelTextLength = 4096;
constexpr std::size_t kMaxQueuedLabels = 512;

struct QueuedLabel {
	sampapi::CRect rect{};
	D3DCOLOR default_color = 0;
	std::string text;
};

struct TextSegment {
	std::string utf8;
	ImU32 color = IM_COL32(255, 255, 255, 255);
};

ImFont* g_label_font = nullptr;
float g_label_font_size = 0.0f;
bool g_imgui_ready = false;
bool g_custom_render_ready = false;
int g_label_draw_depth = 0;
int g_target_font_height = 0;

std::vector<QueuedLabel> g_queued_labels;
std::vector<QueuedLabel> g_frame_labels;

void render_labels_impl();
void ensure_imgui_device_objects();

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

bool is_valid_label_text(const char* text) {
	if (!is_readable_pointer(text)) {
		return false;
	}

	std::size_t length = 0;
	while (length < kMaxLabelTextLength) {
		if (text[length] == '\0') {
			return length > 0;
		}
		++length;
	}

	return false;
}

bool is_samp_in_game() {
	auto* net_game = sampapi::v037r3::RefNetGame();
	if (!is_readable_pointer(net_game, 0x120)) {
		return false;
	}

	if (net_game->GetState() != sampapi::v037r3::CNetGame::GAME_MODE_CONNECTED) {
		return false;
	}

	auto* player_pool = net_game->GetPlayerPool();
	if (!is_readable_pointer(player_pool, 0x40)) {
		return false;
	}

	auto* local_player = player_pool->GetLocalPlayer();
	if (!is_readable_pointer(local_player, 0x60)) {
		return false;
	}

	return local_player->m_bIsActive != FALSE && local_player->m_pPed != nullptr;
}

int samp_little_font_height(sampapi::v037r3::CFonts* fonts) {
	if (is_readable_pointer(fonts, 0x28) && fonts->m_nLittleCharHeight > 0) {
		return fonts->m_nLittleCharHeight;
	}

	return kSampLittleFontFallbackHeight;
}

ImVec4 d3d_color_to_imvec4(D3DCOLOR color) {
	const float a = static_cast<float>((color >> 24) & 0xFF) / 255.0f;
	const float r = static_cast<float>((color >> 16) & 0xFF) / 255.0f;
	const float g = static_cast<float>((color >> 8) & 0xFF) / 255.0f;
	const float b = static_cast<float>(color & 0xFF) / 255.0f;
	return ImVec4(r, g, b, a > 0.0f ? a : 1.0f);
}

ImU32 imvec4_to_imu32(const ImVec4& color) {
	return ImGui::ColorConvertFloat4ToU32(color);
}

bool try_parse_color_tag(const char* text, std::size_t length, std::size_t index, ImVec4& out_color, std::size_t& tag_length) {
	if (index + 7 >= length || text[index] != '{') {
		return false;
	}

	for (std::size_t i = 1; i <= 6; ++i) {
		if (!std::isxdigit(static_cast<unsigned char>(text[index + i]))) {
			return false;
		}
	}

	if (text[index + 7] != '}') {
		return false;
	}

	char hex[7] = {};
	for (int i = 0; i < 6; ++i) {
		hex[i] = text[index + 1 + i];
	}

	const unsigned long rgb = std::strtoul(hex, nullptr, 16);
	out_color.x = static_cast<float>((rgb >> 16) & 0xFF) / 255.0f;
	out_color.y = static_cast<float>((rgb >> 8) & 0xFF) / 255.0f;
	out_color.z = static_cast<float>(rgb & 0xFF) / 255.0f;
	out_color.w = 1.0f;
	tag_length = 8;
	return true;
}

std::vector<TextSegment> parse_colored_text(const std::string& cp1251, D3DCOLOR default_color) {
	std::vector<TextSegment> segments;
	ImVec4 current = d3d_color_to_imvec4(default_color);

	std::size_t index = 0;
	while (index < cp1251.size()) {
		std::size_t tag_length = 0;
		if (try_parse_color_tag(cp1251.c_str(), cp1251.size(), index, current, tag_length)) {
			index += tag_length;
			continue;
		}

		const std::size_t start = index;
		while (index < cp1251.size()) {
			std::size_t next_tag = 0;
			if (try_parse_color_tag(cp1251.c_str(), cp1251.size(), index, current, next_tag)) {
				break;
			}
			++index;
		}

		if (index > start) {
			TextSegment segment;
			segment.utf8 = cp1251_to_utf8(cp1251.substr(start, index - start));
			if (!segment.utf8.empty()) {
				segment.color = imvec4_to_imu32(current);
				segments.push_back(std::move(segment));
			}
		}
	}

	return segments;
}

std::string resolve_label_font_path() {
	char fonts_dir[MAX_PATH] = {};
	if (!SHGetSpecialFolderPathA(nullptr, fonts_dir, CSIDL_FONTS, FALSE)) {
		return {};
	}

	const char* candidates[] = {
		"segoeuib.ttf",
		"segoeui.ttf",
		"arialbd.ttf",
		"arial.ttf",
	};

	for (const char* candidate : candidates) {
		const std::string path = std::string(fonts_dir) + "\\" + candidate;
		if (GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
			return path;
		}
	}

	return {};
}

void rebuild_label_font(int pixel_height) {
	const std::string font_path = resolve_label_font_path();
	if (font_path.empty() || ImGui::GetCurrentContext() == nullptr) {
		return;
	}

	if (g_label_font != nullptr && g_target_font_height == pixel_height) {
		return;
	}

	auto& io = ImGui::GetIO();

	g_label_font = nullptr;
	g_target_font_height = pixel_height;
	g_label_font_size = static_cast<float>(pixel_height);

	ImFontGlyphRangesBuilder builder;
	builder.AddRanges(io.Fonts->GetGlyphRangesCyrillic());
	builder.AddRanges(io.Fonts->GetGlyphRangesDefault());
	builder.AddText(u8"\u2116\u00AB\u00BB\u2013\u2014");
	ImVector<ImWchar> ranges;
	builder.BuildRanges(&ranges);

	g_label_font = io.Fonts->AddFontFromFileTTF(
		font_path.c_str(),
		g_label_font_size,
		nullptr,
		ranges.Data);
	io.Fonts->Build();

	ensure_imgui_device_objects();
}

bool is_device_ready_for_render(IDirect3DDevice9* device) {
	if (!device) {
		return false;
	}

	const HRESULT cooperative = device->TestCooperativeLevel();
	if (cooperative == D3DERR_DEVICELOST || cooperative == D3DERR_DEVICENOTRESET) {
		return false;
	}

	return SUCCEEDED(cooperative);
}

bool can_use_imgui() {
	if (ImGui::GetCurrentContext() == nullptr) {
		return false;
	}

	ImGuiIO& io = ImGui::GetIO();
	return io.Fonts->IsBuilt() && ImGui::GetFont() != nullptr;
}

bool can_replace_with_imgui() {
	return ImGui::GetCurrentContext() != nullptr;
}

void ensure_imgui_device_objects() {
	if (ImGui::GetCurrentContext() == nullptr) {
		return;
	}

	ImGuiIO& io = ImGui::GetIO();
	if (!io.Fonts->IsBuilt()) {
		io.Fonts->Build();
	}

	ImGui_ImplDX9_CreateDeviceObjects();
	g_imgui_ready = true;
}

void prepare_imgui_input() {
	ImGuiIO& io = ImGui::GetIO();
	io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
	io.MouseDrawCursor = false;
	io.WantCaptureMouse = false;
	io.WantCaptureKeyboard = false;
}

bool ensure_label_font(sampapi::v037r3::CFonts* fonts) {
	if (g_label_font != nullptr) {
		return true;
	}

	const int target_height = samp_little_font_height(fonts);
	rebuild_label_font(target_height);

	return g_label_font != nullptr;
}

void render_queued_labels_in_current_frame() {
	if (g_queued_labels.empty() || !g_label_font) {
		return;
	}

	g_frame_labels.swap(g_queued_labels);
	g_queued_labels.clear();

	render_labels_impl();
	g_frame_labels.clear();
}

float measure_segments_width(const std::vector<TextSegment>& segments) {
	if (!g_label_font) {
		return 0.0f;
	}

	float width = 0.0f;
	for (const auto& segment : segments) {
		width += g_label_font->CalcTextSizeA(g_label_font_size, FLT_MAX, 0.0f, segment.utf8.c_str()).x;
	}

	return width;
}

void draw_text_outlined(
	ImDrawList* draw_list,
	ImFont* font,
	const ImVec2& pos,
	ImU32 color,
	const char* text) {
	constexpr ImU32 kOutline = IM_COL32(0, 0, 0, 255);
	static const ImVec2 kOffsets[] = {
		{0.0f, -1.0f},
		{0.0f, 1.0f},
		{-1.0f, 0.0f},
		{1.0f, 0.0f},
		{-1.0f, -1.0f},
		{1.0f, -1.0f},
		{-1.0f, 1.0f},
		{1.0f, 1.0f},
	};

	for (const ImVec2& offset : kOffsets) {
		draw_list->AddText(font, g_label_font_size, ImVec2(pos.x + offset.x, pos.y + offset.y), kOutline, text);
	}

	draw_list->AddText(font, g_label_font_size, pos, color, text);
}

void draw_colored_line(ImDrawList* draw_list, float center_x, float y, const std::string& line_cp1251, D3DCOLOR default_color) {
	const auto segments = parse_colored_text(line_cp1251, default_color);
	if (segments.empty() || !g_label_font) {
		return;
	}

	const float line_width = measure_segments_width(segments);
	float x = center_x - line_width * 0.5f;

	for (const auto& segment : segments) {
		const ImVec2 pos(x, y);
		draw_text_outlined(draw_list, g_label_font, pos, segment.color, segment.utf8.c_str());
		x += g_label_font->CalcTextSizeA(g_label_font_size, FLT_MAX, 0.0f, segment.utf8.c_str()).x;
	}
}

void render_labels_impl() {
	if (g_frame_labels.empty() || !g_label_font) {
		return;
	}

	ImDrawList* draw_list = ImGui::GetBackgroundDrawList();
	const float line_height = g_label_font->CalcTextSizeA(g_label_font_size, FLT_MAX, 0.0f, "Ay").y;

	for (const QueuedLabel& label : g_frame_labels) {
		std::vector<std::string> lines;
		std::string current;

		for (char ch : label.text) {
			if (ch == '\n') {
				lines.push_back(current);
				current.clear();
			}
			else {
				current.push_back(ch);
			}
		}
		lines.push_back(current);

		if (lines.empty()) {
			continue;
		}

		const float center_x = static_cast<float>(label.rect.left);
		const float block_height = line_height * static_cast<float>(lines.size());
		const float anchor_y = static_cast<float>(label.rect.top) + kLabelPickupCenterYOffset;
		float y = anchor_y - block_height * 0.5f;

		for (const std::string& line : lines) {
			if (!line.empty()) {
				draw_colored_line(draw_list, center_x, y, line, label.default_color);
			}
			y += line_height;
		}
	}
}

} // namespace

void renderer_shutdown() {
	g_frame_labels.clear();
	g_queued_labels.clear();
	g_label_font = nullptr;
	g_target_font_height = 0;
	g_label_draw_depth = 0;
	g_imgui_ready = false;
	g_custom_render_ready = false;
}

void renderer_on_device_lost() {
	g_frame_labels.clear();
	g_queued_labels.clear();
	g_label_font = nullptr;
	g_target_font_height = 0;
	g_custom_render_ready = false;
}

void renderer_on_imgui_initialized() {
	if (!can_replace_with_imgui()) {
		return;
	}

	prepare_imgui_input();
	ensure_imgui_device_objects();
	rebuild_label_font(kSampLittleFontFallbackHeight);

	g_custom_render_ready = g_label_font != nullptr;
}

void renderer_render_in_chat_frame() {
	if (!renderer_can_replace_labels() || !can_replace_with_imgui()) {
		return;
	}

	if (!ensure_label_font(sampapi::v037r3::RefFontRenderer())) {
		return;
	}

	g_custom_render_ready = true;
	render_queued_labels_in_current_frame();
}

void renderer_on_present(IDirect3DDevice9* device) {
	if (g_queued_labels.empty()) {
		return;
	}

	if (!is_device_ready_for_render(device) || !renderer_can_replace_labels() || !can_replace_with_imgui()) {
		return;
	}

	if (!ensure_label_font(sampapi::v037r3::RefFontRenderer())) {
		return;
	}

	__try {
		prepare_imgui_input();
		ImGui_ImplDX9_NewFrame();
		ImGui_ImplWin32_NewFrame();
		ImGui::NewFrame();

		render_queued_labels_in_current_frame();

		ImGui::EndFrame();
		ImGui::Render();
		ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
		g_custom_render_ready = true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) {
		renderer_on_device_lost();
	}
}

bool renderer_is_custom_ready() {
	return g_custom_render_ready && can_replace_with_imgui() && g_label_font != nullptr;
}

bool renderer_can_replace_labels() {
	if (g_game.is_on_pause()) {
		return false;
	}

	if (is_samp_in_game()) {
		return true;
	}

	return GetModuleHandleA("samp.dll") != nullptr;
}

void renderer_enter_label_draw() {
	++g_label_draw_depth;
}

void renderer_leave_label_draw() {
	if (g_label_draw_depth > 0) {
		--g_label_draw_depth;
	}
}

bool renderer_should_replace_little_text() {
	return g_label_draw_depth > 0 && renderer_can_replace_labels();
}

bool renderer_queue_little_text(
	sampapi::v037r3::CFonts* fonts,
	const char* text,
	const sampapi::CRect& rect,
	int format,
	D3DCOLOR color,
	BOOL shadow) {
	(void)fonts;
	(void)format;
	(void)shadow;

	if (!renderer_can_replace_labels() || !is_valid_label_text(text)) {
		return false;
	}

	if (!can_replace_with_imgui()) {
		return false;
	}

	if (g_queued_labels.size() >= kMaxQueuedLabels) {
		return false;
	}

	QueuedLabel label;
	label.rect = rect;
	label.default_color = color;
	label.text.assign(text);

	if ((label.default_color & 0xFF000000) == 0) {
		label.default_color |= 0xFF000000;
	}

	g_queued_labels.push_back(std::move(label));
	return true;
}

} // namespace westland_labels
