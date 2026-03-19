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

#include "..\main.h"
#include "..\FalloutEngine\Fallout2.h"
#include "..\version.h"

#include "..\HRP\Init.h"

#include "ExtraSaveSlots.h"
#include "MainMenu.h"

#include <cstdlib>

namespace sfall
{

#ifdef NDEBUG
static const char* VerString1 = "SFALL " VERSION_STRING;
#else
static const char* VerString1 = "SFALL " VERSION_STRING " Debug Build";
#endif

long MainMenu::mXOffset;
long MainMenu::mYOffset;
long MainMenu::mTextOffset; // sum: x + (y * w)

static long OverrideColour, OverrideColour2;
static bool autoJump2LoadScreenEnabled = false;
static bool autoJump2LoadScreenPending = false;
static bool autoLoadLastSaveOnDeathEnabled = true;
static bool autoLoadLastSaveOnDeathPending = false;
static bool autoLoadLastSaveOnDeathArmed = false;
static bool extraSaveSlotsEnabled = false;
static long autoLoadLastSaveOnDeathPage = 0;
static long autoLoadLastSaveOnDeathSlot = 0;

static bool TryParseSaveSlotDirectory(const char* dirName, long& absoluteSlot) {
	if (_strnicmp(dirName, "slot", 4) != 0) return false;

	char* endPtr = nullptr;
	long slotNumber = std::strtol(dirName + 4, &endPtr, 10);
	if (endPtr == dirName + 4 || *endPtr != '\0') return false;
	if (slotNumber <= 0 || slotNumber > 10000) return false;

	absoluteSlot = slotNumber - 1;
	return true;
}

static bool TryFindLatestSaveSlot(long& page, long& slot) {
	char searchPath[MAX_PATH];
	sprintf_s(searchPath, MAX_PATH, "%s\\savegame\\slot*", fo::var::patches);

	WIN32_FIND_DATAA findData = {};
	HANDLE searchHandle = FindFirstFileA(searchPath, &findData);
	if (searchHandle == INVALID_HANDLE_VALUE) return false;

	bool found = false;
	long latestAbsoluteSlot = 0;
	FILETIME latestWriteTime = {};

	do {
		if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) continue;
		if (findData.cFileName[0] == '.') continue;

		long absoluteSlot = 0;
		if (!TryParseSaveSlotDirectory(findData.cFileName, absoluteSlot)) continue;
		if (!extraSaveSlotsEnabled && absoluteSlot >= 10) continue;

		char savePath[MAX_PATH];
		sprintf_s(savePath, MAX_PATH, "%s\\savegame\\%s\\SAVE.DAT", fo::var::patches, findData.cFileName);

		WIN32_FILE_ATTRIBUTE_DATA saveAttributes = {};
		if (!GetFileAttributesExA(savePath, GetFileExInfoStandard, &saveAttributes)) continue;

		if (!found || CompareFileTime(&saveAttributes.ftLastWriteTime, &latestWriteTime) > 0) {
			found = true;
			latestAbsoluteSlot = absoluteSlot;
			latestWriteTime = saveAttributes.ftLastWriteTime;
		}
	} while (FindNextFileA(searchHandle, &findData));

	FindClose(searchHandle);

	if (!found) return false;

	page = latestAbsoluteSlot - (latestAbsoluteSlot % 10);
	slot = latestAbsoluteSlot % 10;
	return true;
}

long MainMenu::GetInjectedMainMenuInput() {
	if (autoLoadLastSaveOnDeathEnabled && autoLoadLastSaveOnDeathPending) {
		autoLoadLastSaveOnDeathPending = false;
		autoLoadLastSaveOnDeathArmed = false;

		long page = 0;
		long slot = 0;
		if (TryFindLatestSaveSlot(page, slot)) {
			autoLoadLastSaveOnDeathPage = page;
			autoLoadLastSaveOnDeathSlot = slot;
			autoLoadLastSaveOnDeathArmed = true;
			dlog_f("AutoLoadLastSaveOnDeath: loading latest save from page %d slot %d.\n",
				DL_MAIN, (page / 10), slot + 1);
			return 'l';
		}

		dlogr("AutoLoadLastSaveOnDeath: no saves found, staying on the main menu.", DL_MAIN);
	}

	if (autoJump2LoadScreenEnabled && autoJump2LoadScreenPending) {
		autoJump2LoadScreenPending = false;
		dlogr("AutoJump2LoadScreen: injecting Load Game hotkey (MainMenu fallback).", DL_MAIN);
		return 'l';
	}

	return 0;
}

void MainMenu::QueueAutoLoadLastSaveOnDeath() {
	autoLoadLastSaveOnDeathPending = true;
	autoLoadLastSaveOnDeathArmed = false;
}

long MainMenu::OverrideLoadGameMode(long mode) {
	if (!autoLoadLastSaveOnDeathArmed) return mode;

	autoLoadLastSaveOnDeathArmed = false;
	ExtraSaveSlots::SetSaveSlot(autoLoadLastSaveOnDeathPage, autoLoadLastSaveOnDeathSlot);
	fo::var::quick_done = 1;

	dlog_f("AutoLoadLastSaveOnDeath: forcing quick load from page %d slot %d.\n",
		DL_MAIN, (autoLoadLastSaveOnDeathPage / 10), autoLoadLastSaveOnDeathSlot + 1);

	return 2; // LOAD_SAVE_MODE_QUICK
}

static __declspec(naked) void MainMenuHookButtonYOffset() {
	static const DWORD MainMenuButtonYHookRet = 0x48184A;
	__asm {
		xor edi, edi;
		xor esi, esi;
		mov ebp, MainMenu::mYOffset;
		jmp MainMenuButtonYHookRet;
	}
}

static __declspec(naked) void MainMenuHookTextYOffset() {
	__asm {
		add eax, MainMenu::mTextOffset;
		jmp dword ptr ds:[FO_VAR_text_to_buf];
	}
}

static void __fastcall main_menu_create_hook_print_text(long xPos, const char* text, long yPos, long color) {
	long winId = fo::var::main_window;
	if (!HRP::Setting::ExternalEnabled() && HRP::Setting::IsEnabled()) {
		fo::Window* win = fo::util::GetWindow(winId);
		yPos = ((yPos - 460) - 20) + win->height;
		xPos = ((xPos - 615) - 25) + win->width;
	}
	if (OverrideColour) color = OverrideColour;

	long fWidth = fo::util::GetTextWidth(text);
	fo::func::win_print(winId, text, fWidth, xPos, yPos - 12, color); // fallout print

	long sWidth = fo::util::GetTextWidth(VerString1);
	fo::func::win_print(winId, VerString1, sWidth, xPos + fWidth - sWidth, yPos, color); // sfall print
}

static long __stdcall main_menu_loop_hook() {
	long input = MainMenu::GetInjectedMainMenuInput();
	if (input) return input;
	return fo::func::get_input();
}

void MainMenu::init() {
	autoJump2LoadScreenEnabled = (IniReader::GetConfigInt("Misc", "AutoJump2LoadScreen", 0) != 0);
	autoJump2LoadScreenPending = autoJump2LoadScreenEnabled;
	autoLoadLastSaveOnDeathEnabled = (IniReader::GetConfigInt("Misc", "AutoLoadLastSaveOnDeath", 1) != 0);
	autoLoadLastSaveOnDeathPending = false;
	autoLoadLastSaveOnDeathArmed = false;
	extraSaveSlotsEnabled = (IniReader::GetConfigInt("Misc", "ExtraSaveSlots", 0) != 0);

	int offset;
	if (offset = IniReader::GetConfigInt("Misc", "MainMenuCreditsOffsetX", 0)) {
		SafeWrite32(0x481753, 15 + offset);
	}
	if (offset = IniReader::GetConfigInt("Misc", "MainMenuCreditsOffsetY", 0)) {
		SafeWrite32(0x48175C, 460 + offset);
	}
	if (offset = IniReader::GetConfigInt("Misc", "MainMenuOffsetX", 0)) {
		mXOffset = offset;
	}
	if (offset = IniReader::GetConfigInt("Misc", "MainMenuOffsetY", 0)) {
		mYOffset = offset;
	}
	if (!HRP::Setting::IsEnabled()) {
		if (mXOffset) SafeWrite32(0x48187C, 30 + mXOffset); // button
		if (mYOffset) MakeJump(0x481844, MainMenuHookButtonYOffset);

		mTextOffset = mXOffset + (mYOffset * 640);
	if (mTextOffset) MakeCall(0x481933, MainMenuHookTextYOffset, 1);
	}

	HookCall(0x4817AB, main_menu_create_hook_print_text);
	if (HRP::Setting::ExternalEnabled()) {
		// WinProc::init is skipped for external HRP, so install input hook here.
		HookCall(0x481B2A, main_menu_loop_hook);
	}

	OverrideColour = IniReader::GetConfigInt("Misc", "MainMenuFontColour", 0);
	if (OverrideColour & 0xFF) {
		OverrideColour &= 0x00FF00FF;
		OverrideColour |= 0x06000000;
		unsigned char flags = static_cast<unsigned char>((OverrideColour & 0xFF0000) >> 16);
		if (!(flags & 1)) SafeWrite32(0x481748, (DWORD)&OverrideColour);
	}
	OverrideColour2 = IniReader::GetConfigInt("Misc", "MainMenuBigFontColour", 0) & 0xFF;
	if (OverrideColour2) SafeWrite32(0x481906, (DWORD)&OverrideColour2);
}

}
