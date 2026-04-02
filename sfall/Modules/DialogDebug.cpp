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

#include <stdint.h>
#include <stdio.h>
#include <string>

#include "..\Logging.h"
#include "..\FalloutEngine\Fallout2.h"

#include "LoadGameHook.h"
#include "DialogDebug.h"

namespace sfall
{

namespace
{

typedef void (__cdecl *TickerProc)();

constexpr DWORD kFuncTickersAdd = 0x4C8D74;
constexpr DWORD kFuncTickersRemove = 0x4C8DC4;
constexpr DWORD kFuncScrGetMsgStr = 0x4A6C50;

constexpr DWORD kVarDialogStateFix = 0x5186D4;
constexpr DWORD kVarReplyWindow = 0x5186E4;
constexpr DWORD kVarOptionsWindow = 0x5186E8;
constexpr DWORD kVarSpeakerIsPartyMember = 0x51884C;
constexpr DWORD kVarHeadFid = 0x518850;
constexpr DWORD kVarDialogSid = 0x518854;
constexpr DWORD kVarDialogSubWindowLength = 0x518918;
constexpr DWORD kVarReviewEntriesLength = 0x5186DC;
constexpr DWORD kVarReviewEntries = 0x58ECE0;
constexpr DWORD kVarDialogButtons = 0x58F470;
constexpr DWORD kVarReplyMessageListId = 0x58F4D8;
constexpr DWORD kVarReplyMessageId = 0x58F4DC;
constexpr DWORD kVarReplyOverflow = 0x58F4E0;
constexpr DWORD kVarReplyText = 0x58F4E4;
constexpr DWORD kVarOptionEntries = 0x58FF70;

constexpr int kDialogReplyTextLeft = 5;
constexpr int kDialogReplyTextTop = 10;
constexpr int kDialogReplyTextRight = 374;
constexpr int kDialogReplyTextBottom = 58;

constexpr int kDialogOptionsTextLeft = 5;
constexpr int kDialogOptionsTextTop = 5;
constexpr int kDialogOptionsTextRight = 388;
constexpr int kDialogOptionsTextBottom = 112;

constexpr int kBarterButtonX = 593;
constexpr int kBarterButtonY = 41;
constexpr int kBarterButtonW = 14;
constexpr int kBarterButtonH = 14;

constexpr int kReviewButtonX = 13;
constexpr int kReviewButtonY = 154;
constexpr int kReviewButtonW = 51;
constexpr int kReviewButtonH = 29;

constexpr int kCombatControlButtonX = 593;
constexpr int kCombatControlButtonY = 116;
constexpr int kCombatControlButtonW = 14;
constexpr int kCombatControlButtonH = 14;

constexpr uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr uint64_t kFnvPrime = 1099511628211ull;

#pragma pack(push, 1)
struct GameDialogReviewEntry {
	int replyMessageListId;
	int replyMessageId;
	char* replyText;
	int optionMessageListId;
	int optionMessageId;
	char* optionText;
};

struct GameDialogOptionEntry {
	int messageListId;
	int messageId;
	int reaction;
	int proc;
	int btn;
	int top;
	char text[900];
	int bottom;
};
#pragma pack(pop)

static_assert(sizeof(GameDialogReviewEntry) == 24, "Unexpected GameDialogReviewEntry size.");
static_assert(sizeof(GameDialogOptionEntry) == 928, "Unexpected GameDialogOptionEntry size.");
static_assert(kVarOptionEntries > kVarReplyText, "Unexpected dialog reply text layout.");

constexpr size_t kReplyTextMaxLength = static_cast<size_t>(kVarOptionEntries - kVarReplyText);
constexpr size_t kOptionTextMaxLength = sizeof(GameDialogOptionEntry::text);
constexpr size_t kReviewTextMaxLength = kReplyTextMaxLength;
constexpr size_t kSpeakerNameMaxLength = 256;

template <typename T>
T& Raw(DWORD addr) {
	return *reinterpret_cast<T*>(addr);
}

struct WindowSnapshot {
	bool valid = false;
	long x = 0;
	long y = 0;
	long width = 0;
	long height = 0;
};

static bool tickerRegistered = false;
static bool dialogNodeSeen = false;
static uint32_t dialogSessionCounter = 0;
static uint32_t dialogNodeCounter = 0;
static uint64_t lastDialogSignature = 0;

struct BoundedTextView {
	const unsigned char* bytes = nullptr;
	size_t length = 0;
	bool terminated = false;
};

static void __stdcall TickersAdd(TickerProc proc) {
	__asm {
		mov  eax, proc;
		call kFuncTickersAdd;
	}
}

static void __stdcall TickersRemove(TickerProc proc) {
	__asm {
		mov  eax, proc;
		call kFuncTickersRemove;
	}
}

static char* __stdcall ScrGetMsgStr(int messageListId, int messageId) {
	__asm {
		mov  edx, messageId;
		mov  eax, messageListId;
		call kFuncScrGetMsgStr;
	}
}

static WindowSnapshot GetWindowSnapshot(int windowId) {
	WindowSnapshot snapshot;
	if (windowId == -1) return snapshot;

	fo::Window* win = fo::func::GNW_find(windowId);
	if (!win) return snapshot;

	snapshot.valid = true;
	snapshot.x = win->rect.x;
	snapshot.y = win->rect.y;
	snapshot.width = win->width;
	snapshot.height = win->height;
	return snapshot;
}

// Dialog strings are engine-owned, so cap every read to known storage bounds.
static BoundedTextView GetBoundedTextView(const char* text, size_t maxLen) {
	BoundedTextView view;
	if (!text) return view;

	view.bytes = reinterpret_cast<const unsigned char*>(text);
	while (view.length < maxLen && view.bytes[view.length] != '\0') {
		view.length++;
	}
	view.terminated = (view.length < maxLen && view.bytes[view.length] == '\0');
	return view;
}

static std::string EscapeForLog(const char* text, size_t maxLen) {
	if (!text) return "<null>";

	const BoundedTextView view = GetBoundedTextView(text, maxLen);
	std::string out;
	out.reserve(view.length + 16);

	for (size_t i = 0; i < view.length; i++) {
		switch (view.bytes[i]) {
		case '\\':
			out += "\\\\";
			break;
		case '\r':
			out += "\\r";
			break;
		case '\n':
			out += "\\n";
			break;
		case '\t':
			out += "\\t";
			break;
		case '"':
			out += "\\\"";
			break;
		default:
			if (view.bytes[i] >= 0x20 && view.bytes[i] <= 0x7E) {
				out.push_back(static_cast<char>(view.bytes[i]));
			} else {
				char buf[5];
				sprintf_s(buf, "\\x%02X", view.bytes[i]);
				out += buf;
			}
			break;
		}
	}

	if (!view.terminated) out += "<truncated>";

	return out;
}

static std::string FormatRawReviewPointer(const char* text) {
	char buf[32];
	sprintf_s(buf, "<raw_review_ptr 0x%08X>", reinterpret_cast<DWORD>(text));
	return buf;
}

static const char* ReactionToString(int reaction) {
	switch (reaction) {
	case 49: return "good";
	case 50: return "neutral";
	case 51: return "bad";
	default: return "unknown";
	}
}

static const char* ResolveReviewText(const GameDialogReviewEntry& entry, bool option) {
	const int messageListId = option ? entry.optionMessageListId : entry.replyMessageListId;
	const int messageId = option ? entry.optionMessageId : entry.replyMessageId;

	if (messageListId == -3) return nullptr;
	if (messageListId == -4) return nullptr;

	return ScrGetMsgStr(messageListId, messageId);
}

static bool ReviewTextUsesRawPointer(const GameDialogReviewEntry& entry, bool option) {
	return (option ? entry.optionMessageListId : entry.replyMessageListId) == -4;
}

static const char* GetReviewRawTextPointer(const GameDialogReviewEntry& entry, bool option) {
	return option ? entry.optionText : entry.replyText;
}

static bool IsDialogSessionActive() {
	return (Raw<int>(kVarDialogStateFix) != 0);
}

static bool IsDialogueNodeVisible() {
	return (
		IsDialogSessionActive() &&
		fo::var::dialogue_state == 1 &&
		Raw<int>(kVarReplyWindow) != -1 &&
		Raw<int>(kVarOptionsWindow) != -1 &&
		Raw<int>(kVarReplyMessageListId) != -1 &&
		fo::var::gdNumOptions > 0
	);
}

static void ResetDialogTracking() {
	dialogNodeSeen = false;
	dialogNodeCounter = 0;
	lastDialogSignature = 0;
}

static uint64_t HashBytes(uint64_t hash, const void* data, size_t size) {
	const unsigned char* bytes = reinterpret_cast<const unsigned char*>(data);
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= kFnvPrime;
	}
	return hash;
}

template <typename T>
static uint64_t HashPod(uint64_t hash, const T& value) {
	return HashBytes(hash, &value, sizeof(value));
}

static uint64_t HashCString(uint64_t hash, const char* text, size_t maxLen) {
	if (!text) {
		const unsigned char marker = 0xFF;
		return HashBytes(hash, &marker, sizeof(marker));
	}

	const BoundedTextView view = GetBoundedTextView(text, maxLen);
	if (view.length > 0) {
		hash = HashBytes(hash, view.bytes, view.length);
	}

	const unsigned char terminator = view.terminated ? 0x00 : 0xFE;
	return HashBytes(hash, &terminator, sizeof(terminator));
}

static uint64_t ComputeDialogSignature() {
	uint64_t hash = kFnvOffsetBasis;

	hash = HashPod(hash, Raw<int>(kVarReviewEntriesLength));
	hash = HashPod(hash, Raw<int>(kVarReplyMessageListId));
	hash = HashPod(hash, Raw<int>(kVarReplyMessageId));
	hash = HashCString(hash, reinterpret_cast<const char*>(kVarReplyText), kReplyTextMaxLength);
	hash = HashPod(hash, fo::var::gdNumOptions);
	hash = HashPod(hash, fo::var::dialogue_state);
	hash = HashPod(hash, fo::var::dialogue_switch_mode);

	const int optionCount = static_cast<int>(fo::var::gdNumOptions);
	GameDialogOptionEntry* options = reinterpret_cast<GameDialogOptionEntry*>(kVarOptionEntries);
	for (int i = 0; i < optionCount; i++) {
		const GameDialogOptionEntry& option = options[i];
		hash = HashPod(hash, option.messageListId);
		hash = HashPod(hash, option.messageId);
		hash = HashPod(hash, option.reaction);
		hash = HashPod(hash, option.proc);
		hash = HashPod(hash, option.top);
		hash = HashPod(hash, option.bottom);
		hash = HashCString(hash, option.text, kOptionTextMaxLength);
	}

	return hash;
}

static void LogButtonRect(const char* label, bool present, const WindowSnapshot& dialogWindow,
	int x, int y, int width, int height) {
	if (!present || !dialogWindow.valid) {
		fo::func::debug_printf("[DIALOG_DUMP] %s present=0\n", label);
		return;
	}

	fo::func::debug_printf(
		"[DIALOG_DUMP] %s present=1 rect=(%ld,%ld)-(%ld,%ld)\n",
		label,
		dialogWindow.x + x,
		dialogWindow.y + y,
		dialogWindow.x + x + width - 1,
		dialogWindow.y + y + height - 1
	);
}

static void LogDialogSnapshot(bool isOpenEvent) {
	const WindowSnapshot backgroundWindow = GetWindowSnapshot(fo::var::dialogueBackWindow);
	const WindowSnapshot dialogWindow = GetWindowSnapshot(fo::var::getInt(FO_VAR_dialogueWindow));
	const WindowSnapshot replyWindow = GetWindowSnapshot(Raw<int>(kVarReplyWindow));
	const WindowSnapshot optionsWindow = GetWindowSnapshot(Raw<int>(kVarOptionsWindow));

	fo::GameObject* speaker = fo::var::dialog_target;
	const char* speakerName = (speaker) ? fo::func::object_name(speaker) : nullptr;
	const int speakerPid = (speaker) ? speaker->protoId : -1;
	const long speakerTile = (speaker) ? speaker->tile : -1;
	const long speakerElevation = (speaker) ? speaker->elevation : -1;

	const int reviewEntriesLength = Raw<int>(kVarReviewEntriesLength);
	const GameDialogReviewEntry* reviewEntries = reinterpret_cast<const GameDialogReviewEntry*>(kVarReviewEntries);
	const GameDialogReviewEntry* lastReview = (reviewEntriesLength > 0) ? &reviewEntries[reviewEntriesLength - 1] : nullptr;

	const char* previousChoiceText = (lastReview) ? ResolveReviewText(*lastReview, true) : nullptr;
	const bool previousChoiceUsesRawPointer = (lastReview) ? ReviewTextUsesRawPointer(*lastReview, true) : false;
	const char* previousChoiceRawText = (lastReview && previousChoiceUsesRawPointer)
		? GetReviewRawTextPointer(*lastReview, true)
		: nullptr;
	const int previousChoiceList = (lastReview) ? lastReview->optionMessageListId : -3;
	const int previousChoiceMessage = (lastReview) ? lastReview->optionMessageId : -3;

	const int* dialogButtons = reinterpret_cast<const int*>(kVarDialogButtons);

	fo::func::debug_printf(
		"\n[DIALOG_DUMP] type=%s session=%u node=%u state=%d switch=%d review_entries=%d options=%d\n",
		(isOpenEvent) ? "open" : "node",
		dialogSessionCounter,
		dialogNodeCounter,
		fo::var::dialogue_state,
		fo::var::dialogue_switch_mode,
		reviewEntriesLength,
		fo::var::gdNumOptions
	);

	fo::func::debug_printf(
		"[DIALOG_DUMP] speaker ptr=0x%08X name=\"%s\" pid=%d tile=%ld elev=%ld party=%d sid=%d head_fid=%d caps=%d barter_mod=%d\n",
		reinterpret_cast<DWORD>(speaker),
		EscapeForLog(speakerName, kSpeakerNameMaxLength).c_str(),
		speakerPid,
		speakerTile,
		speakerElevation,
		Raw<bool>(kVarSpeakerIsPartyMember) ? 1 : 0,
		Raw<int>(kVarDialogSid),
		Raw<int>(kVarHeadFid),
		fo::func::item_caps_total(fo::var::obj_dude),
		fo::var::gdBarterMod
	);

	fo::func::debug_printf(
		"[DIALOG_DUMP] background_window valid=%d rect=(%ld,%ld %ldx%ld) dialogue_window valid=%d rect=(%ld,%ld %ldx%ld) subwin_len=%d\n",
		backgroundWindow.valid ? 1 : 0,
		backgroundWindow.x,
		backgroundWindow.y,
		backgroundWindow.width,
		backgroundWindow.height,
		dialogWindow.valid ? 1 : 0,
		dialogWindow.x,
		dialogWindow.y,
		dialogWindow.width,
		dialogWindow.height,
		Raw<int>(kVarDialogSubWindowLength)
	);

	fo::func::debug_printf(
		"[DIALOG_DUMP] reply_window valid=%d rect=(%ld,%ld %ldx%ld) text_rect=(%ld,%ld)-(%ld,%ld) overflow=%d\n",
		replyWindow.valid ? 1 : 0,
		replyWindow.x,
		replyWindow.y,
		replyWindow.width,
		replyWindow.height,
		replyWindow.valid ? replyWindow.x + kDialogReplyTextLeft : 0,
		replyWindow.valid ? replyWindow.y + kDialogReplyTextTop : 0,
		replyWindow.valid ? replyWindow.x + kDialogReplyTextRight : 0,
		replyWindow.valid ? replyWindow.y + kDialogReplyTextBottom - 1 : 0,
		Raw<int>(kVarReplyOverflow)
	);

	fo::func::debug_printf(
		"[DIALOG_DUMP] options_window valid=%d rect=(%ld,%ld %ldx%ld) text_rect=(%ld,%ld)-(%ld,%ld)\n",
		optionsWindow.valid ? 1 : 0,
		optionsWindow.x,
		optionsWindow.y,
		optionsWindow.width,
		optionsWindow.height,
		optionsWindow.valid ? optionsWindow.x + kDialogOptionsTextLeft : 0,
		optionsWindow.valid ? optionsWindow.y + kDialogOptionsTextTop : 0,
		optionsWindow.valid ? optionsWindow.x + kDialogOptionsTextRight : 0,
		optionsWindow.valid ? optionsWindow.y + kDialogOptionsTextBottom - 1 : 0
	);

	LogButtonRect("barter_button", dialogButtons[0] != -1, dialogWindow, kBarterButtonX, kBarterButtonY, kBarterButtonW, kBarterButtonH);
	LogButtonRect("review_button", dialogButtons[1] != -1, dialogWindow, kReviewButtonX, kReviewButtonY, kReviewButtonW, kReviewButtonH);
	LogButtonRect("combat_control_button", dialogButtons[2] != -1, dialogWindow, kCombatControlButtonX, kCombatControlButtonY, kCombatControlButtonW, kCombatControlButtonH);

	fo::func::debug_printf(
		"[DIALOG_DUMP] npc_message list=%d msg=%d text=\"%s\"\n",
		Raw<int>(kVarReplyMessageListId),
		Raw<int>(kVarReplyMessageId),
		EscapeForLog(reinterpret_cast<const char*>(kVarReplyText), kReplyTextMaxLength).c_str()
	);

	if (previousChoiceList == -3) {
		fo::func::debug_printf("[DIALOG_DUMP] previous_choice none\n");
	} else {
		const std::string previousChoiceLogText = previousChoiceUsesRawPointer
			? FormatRawReviewPointer(previousChoiceRawText)
			: EscapeForLog(previousChoiceText, kReviewTextMaxLength);
		fo::func::debug_printf(
			"[DIALOG_DUMP] previous_choice list=%d msg=%d text=\"%s\"\n",
			previousChoiceList,
			previousChoiceMessage,
			previousChoiceLogText.c_str()
		);
	}

	const int optionCount = static_cast<int>(fo::var::gdNumOptions);
	const GameDialogOptionEntry* options = reinterpret_cast<const GameDialogOptionEntry*>(kVarOptionEntries);
	for (int i = 0; i < optionCount; i++) {
		const GameDialogOptionEntry& option = options[i];
		fo::func::debug_printf(
			"[DIALOG_DUMP] option index=%d hotkey=%d reaction=%s list=%d msg=%d proc=%d text_rect=(%ld,%ld)-(%ld,%ld) text=\"%s\"\n",
			i + 1,
			i + 1,
			ReactionToString(option.reaction),
			option.messageListId,
			option.messageId,
			option.proc,
			optionsWindow.valid ? optionsWindow.x + kDialogOptionsTextLeft : 0,
			optionsWindow.valid ? optionsWindow.y + option.top : 0,
			optionsWindow.valid ? optionsWindow.x + kDialogOptionsTextRight : 0,
			optionsWindow.valid ? optionsWindow.y + option.bottom - 1 : 0,
			EscapeForLog(option.text, kOptionTextMaxLength).c_str()
		);
	}

	fo::func::debug_printf("[DIALOG_DUMP] end\n");
}

static void __cdecl DialogStateTicker() {
	if (!IsDialogSessionActive()) {
		ResetDialogTracking();
		return;
	}

	if (!IsDialogueNodeVisible()) return;

	const uint64_t signature = ComputeDialogSignature();
	if (dialogNodeSeen && signature == lastDialogSignature) return;

	const bool isOpenEvent = !dialogNodeSeen;
	if (isOpenEvent) {
		dialogSessionCounter++;
		dialogNodeCounter = 0;
	}

	dialogNodeSeen = true;
	dialogNodeCounter++;
	lastDialogSignature = signature;

	LogDialogSnapshot(isOpenEvent);
}

static void EnsureTickerRegistered() {
	TickersAdd(DialogStateTicker);
	tickerRegistered = true;
}

static void RemoveTicker() {
	if (!tickerRegistered) return;
	TickersRemove(DialogStateTicker);
	tickerRegistered = false;
}

}

void DialogDebug::init() {
	dlogr("Enabling dialogue window debug logger.", DL_INIT);

	LoadGameHook::OnAfterGameInit() += EnsureTickerRegistered;
	LoadGameHook::OnGameReset() += []() {
		EnsureTickerRegistered();
		ResetDialogTracking();
	};
	LoadGameHook::OnBeforeGameClose() += []() {
		RemoveTicker();
		ResetDialogTracking();
	};
}

}
