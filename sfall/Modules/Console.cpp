/*
 *    sfall
 *    Copyright (C) 2008-2025  The sfall team
 *
 *    This program is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    This program is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <cstdlib>
#include <fstream>
#include <string>

#include "..\main.h"
#include "..\FalloutEngine\Fallout2.h"
#include "..\Modules\LoadGameHook.h"

#include "..\HRP\Init.h"

#include "Console.h"

namespace sfall
{

static std::ofstream consoleFile;
static long printCount = 0;
static bool messageBoxToDebugLogEnabled = false;
static bool floatingTextToDebugLogEnabled = false;

static constexpr long kConsoleFlushInterval = 20;
static constexpr size_t kMessageWrapWidth = 30;
static constexpr const char* kDebugLogBoxSeparator = "------------------------------\n";
static constexpr const char* kFloatingTextToDebugLogInherit = "__inherit__";
static constexpr const char* kUnknownSpeakerName = "<unknown>";

static void PrintWrappedMessageLine(const char* line, size_t length) {
	if (length == 0) {
		fo::func::debug_printf("| \n");
		return;
	}

	size_t offset = 0;
	while (offset < length) {
		size_t chunkLen = length - offset;
		if (chunkLen > kMessageWrapWidth) chunkLen = kMessageWrapWidth;
		fo::func::debug_printf("| %.*s\n", static_cast<int>(chunkLen), line + offset);
		offset += chunkLen;
	}
}

static void PrintSplitMessageToDebugLog(const char* msg) {
	const char* lineStart = msg;
	const char* cursor = msg;
	while (*cursor) {
		if (*cursor == '\n') {
			PrintWrappedMessageLine(lineStart, static_cast<size_t>(cursor - lineStart));
			cursor++;
			lineStart = cursor;
			continue;
		}

		if (*cursor == '\\' && cursor[1] == 'n') {
			PrintWrappedMessageLine(lineStart, static_cast<size_t>(cursor - lineStart));
			cursor += 2;
			lineStart = cursor;
			continue;
		}

		cursor++;
	}

	PrintWrappedMessageLine(lineStart, static_cast<size_t>(cursor - lineStart));
}

static void PrintBoxHeaderToDebugLog(const char* title) {
	fo::func::debug_printf(kDebugLogBoxSeparator);
	fo::func::debug_printf("| %s\n", title);
	fo::func::debug_printf(kDebugLogBoxSeparator);
}

static void PrintBoxedMessageToDebugLog(const char* title, const char* msg, const char* speaker = nullptr) {
	if (!msg || *msg == '\0') return;

	PrintBoxHeaderToDebugLog(title);

	if (speaker && *speaker) {
		std::string speakerLine("Speaker: ");
		speakerLine += speaker;
		PrintSplitMessageToDebugLog(speakerLine.c_str());
	}

	PrintSplitMessageToDebugLog(msg);
	fo::func::debug_printf(kDebugLogBoxSeparator);
}

static void PrintMessageToDebugLog(const char* msg) {
	if (!messageBoxToDebugLogEnabled || !msg || *msg == '\0') return;

	PrintBoxedMessageToDebugLog("Msg_Box Newline", msg);
}

static void __fastcall ConsoleOutputPrint(const char* msg) {
	if (!msg) return;

	if (consoleFile.is_open()) {
		consoleFile << msg << '\n';

		if (++printCount >= kConsoleFlushInterval) {
			printCount = 0;
			consoleFile.flush();
		}
	}

	PrintMessageToDebugLog(msg);
}

static __declspec(naked) void display_print_hack() {
	static const DWORD display_print_Ret = 0x431871;
	__asm {
		push ebx;
		push ecx;
		push edx;
		push esi;
		push edi;
		mov  ebx, eax;
		mov  ecx, eax;
		call ConsoleOutputPrint;
		mov  eax, ebx;
		jmp  display_print_Ret;
	}
}

static __declspec(naked) void action_use_skill_on_text_object_hook() {
	__asm {
		pushad;
		mov  ecx, eax;
		call Console::PrintFloatToDebugLog;
		popad;
		jmp  fo::funcoffs::text_object_create_;
	}
}

static __declspec(naked) void op_float_msg_text_object_hook() {
	__asm {
		pushad;
		mov  ecx, eax;
		call Console::PrintFloatToDebugLog;
		popad;
		jmp  fo::funcoffs::text_object_create_;
	}
}

static __declspec(naked) void partyMemberCopyLevelInfo_text_object_hook() {
	__asm {
		pushad;
		mov  ecx, eax;
		call Console::PrintFloatToDebugLog;
		popad;
		jmp  fo::funcoffs::text_object_create_;
	}
}

void Console::PrintFile(const char* msg) {
	ConsoleOutputPrint(msg);
}

void __fastcall Console::PrintFloatToDebugLog(fo::GameObject* object, const char* msg) {
	if (!floatingTextToDebugLogEnabled || !msg || *msg == '\0') return;

	const char* speaker = kUnknownSpeakerName;
	if (object) {
		const char* objectName = fo::func::object_name(object);
		if (objectName && *objectName) speaker = objectName;
	}

	PrintBoxedMessageToDebugLog("Floating Text", msg, speaker);
}

void Console::init() {
	messageBoxToDebugLogEnabled = (IniReader::GetIntDefaultConfig("Debugging", "MessageBoxToDebugLog", 0) != 0);
	const auto floatingTextToDebugLogSetting =
		IniReader::GetStringDefaultConfig("Debugging", "FloatingTextToDebugLog", kFloatingTextToDebugLogInherit);
	floatingTextToDebugLogEnabled =
		(floatingTextToDebugLogSetting == kFloatingTextToDebugLogInherit)
		? messageBoxToDebugLogEnabled
		: (std::atoi(floatingTextToDebugLogSetting.c_str()) != 0);

	auto path = IniReader::GetConfigString("Misc", "ConsoleOutputPath", "");
	if (!path.empty()) {
		consoleFile.open(path);
		if (consoleFile.is_open()) {
			LoadGameHook::OnGameReset() += []() {
				printCount = 0;
				consoleFile.flush();
			};
		}
	}

	if (!HRP::Setting::IsEnabled() && (consoleFile.is_open() || messageBoxToDebugLogEnabled)) {
		MakeJump(0x43186C, display_print_hack);
	}

	if (floatingTextToDebugLogEnabled) {
		HookCall(0x412871, action_use_skill_on_text_object_hook);
		HookCall(0x45947E, op_float_msg_text_object_hook);
		HookCall(0x495E34, partyMemberCopyLevelInfo_text_object_hook);
	}
}

void Console::exit() {
	if (consoleFile.is_open()) consoleFile.close();
	messageBoxToDebugLogEnabled = false;
	floatingTextToDebugLogEnabled = false;
}

}
