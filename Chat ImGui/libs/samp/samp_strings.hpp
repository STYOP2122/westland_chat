#pragma once

#include "samp.hpp"
#include "snippets.hpp"

struct samp_string_patch_t
{
	uintptr_t	offset;
	size_t		maxLength;
	const char*	utf8Text;
};

// Offsets for SA-MP 0.3.7 R3-1 (samp.dll entry point 0xCC4D0)
inline const samp_string_patch_t samp_connection_string_patches_r3[] =
{
	{ 0xE596C, 46, u8"{FFFFFF}Входим в Westland Project.." },
	{ 0xE599C, 22, u8"Подключение к серверам.." },
	{ 0xE5A3C, 35, u8"Сервер принял подключение." },
	{ 0xE5A80, 29, u8"Сервер разорвал соединение." },
	{ 0xE5AB8, 37, u8"Сервер не отвечает. Повтор..." },
	{ 0xE5B58, 45, u8"Связь потеряна. Переподключение..." },
	{ 0xE5B98, 30, u8"Связь с сервером установлена!" },
	{ 0xE6060, 26, u8"Приятной игры!" },
	{ 0xE6ACC, 26, u8"Связь с сервером потеряна." },
	{ 0xE6D20, 51, u8"Таймаут ожидания данных от сервера." },
};

inline bool sampPatchString(uintptr_t offset, std::string_view cp1251Text, size_t maxLength)
{
	if (cp1251Text.size() > maxLength)
		return false;

	auto* address = reinterpret_cast<char*>(sampGetBase() + offset);
	if (!address)
		return false;

	const size_t patchSize = maxLength + 1;

	DWORD oldProtection = 0;
	if (!VirtualProtect(address, patchSize, PAGE_EXECUTE_READWRITE, &oldProtection))
		return false;

	memset(address, 0, patchSize);
	memcpy(address, cp1251Text.data(), cp1251Text.size());
	VirtualProtect(address, patchSize, oldProtection, &oldProtection);
	return true;
}

inline bool sampIsStartedMessage(std::string_view cp1251Text)
{
	const auto startedRu = utf8_to_cp1251(u8"Запущен");
	return cp1251Text.find("SA-MP") != std::string_view::npos
		&& (cp1251Text.find("Started") != std::string_view::npos
			|| (!startedRu.empty() && cp1251Text.find(startedRu) != std::string_view::npos)
			|| cp1251Text.find(utf8_to_cp1251(u8"Westland Project")) != std::string_view::npos);
}

inline bool sampIsConnectingMessage(std::string_view cp1251Text)
{
	if (cp1251Text.rfind("Connecting to ", 0) == 0)
		return true;

	const auto connectingRu = utf8_to_cp1251(u8"Подключение к ");
	if (!connectingRu.empty() && cp1251Text.rfind(connectingRu, 0) == 0)
	{
		const auto serversMsg = utf8_to_cp1251(u8"Подключение к серверам..");
		return cp1251Text != serversMsg;
	}

	return false;
}

inline bool sampIsConnectedToServerMessage(std::string_view cp1251Text)
{
	if (cp1251Text.rfind("Connected to ", 0) == 0)
		return true;

	const auto connectedRu = utf8_to_cp1251(u8"Подключены к ");
	return !connectedRu.empty() && cp1251Text.rfind(connectedRu, 0) == 0;
}

inline bool sampShouldSuppressConnectionMessage(std::string_view cp1251Text)
{
	return sampIsConnectedToServerMessage(cp1251Text);
}

inline std::string sampTranslateConnectionMessage(std::string_view cp1251Text)
{
	if (sampIsStartedMessage(cp1251Text))
		return utf8_to_cp1251(u8"{FFFFFF}Входим в Westland Project..");

	if (sampIsConnectingMessage(cp1251Text))
		return utf8_to_cp1251(u8"Подключение к серверам..");

	if (cp1251Text == "Connected. Joining the game..."
		|| cp1251Text == utf8_to_cp1251(u8"Подключились. Вход в игру..."))
		return utf8_to_cp1251(u8"Приятной игры!");

	return {};
}

inline void sampApplyRussianConnectionStrings()
{
	if (sampGetVersion() != SAMP_037_R3_1)
		return;

	for (const auto& patch : samp_connection_string_patches_r3)
	{
		const auto text = utf8_to_cp1251(patch.utf8Text);
		if (!text.empty())
			sampPatchString(patch.offset, text, patch.maxLength);
	}
}
