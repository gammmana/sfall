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

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "..\main.h"
#include "..\FalloutEngine\Fallout2.h"
#include "..\Modules\LoadGameHook.h"
#include "..\Modules\Drugs.h"
#include "..\Modules\HeroAppearance.h"
#include "..\Modules\MainLoopHook.h"
#include "..\Modules\ScriptExtender.h"

#include "..\HRP\Init.h"

#include "Console.h"

namespace sfall
{

static std::ofstream consoleFile;
static long printCount = 0;
static bool messageBoxToDebugLogEnabled = false;
static bool floatingTextToDebugLogEnabled = false;
static bool sneakModeStateInitialized = false;
static bool previousSneakModeState = false;

static constexpr long kConsoleFlushInterval = 20;
static constexpr size_t kMessageWrapWidth = 30;
static constexpr const char* kDebugLogBoxSeparator = "------------------------------\n";
static constexpr size_t kCharacterLogWrapWidth = 78;
static constexpr const char* kCharacterLogBoxSeparator = "------------------------------------------------------------------------------\n";
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

static void PrintWrappedDebugLogLine(const char* line, size_t length, size_t wrapWidth) {
	if (length == 0) {
		fo::func::debug_printf("| \n");
		return;
	}

	size_t offset = 0;
	while (offset < length) {
		size_t chunkLen = length - offset;
		if (chunkLen > wrapWidth) chunkLen = wrapWidth;
		fo::func::debug_printf("| %.*s\n", static_cast<int>(chunkLen), line + offset);
		offset += chunkLen;
	}
}

static void PrintBoxedLinesToDebugLog(const char* title, const std::vector<std::string>& lines) {
	fo::func::debug_printf(kCharacterLogBoxSeparator);
	fo::func::debug_printf("| %s\n", title);
	fo::func::debug_printf(kCharacterLogBoxSeparator);

	for (const auto& line : lines) {
		PrintWrappedDebugLogLine(line.c_str(), line.length(), kCharacterLogWrapWidth);
	}

	fo::func::debug_printf(kCharacterLogBoxSeparator);
}

static void ResetSneakModeTracking() {
	sneakModeStateInitialized = false;
	previousSneakModeState = false;
}

static void PrintSneakModeToDebugLog(bool isSneaking) {
	PrintBoxedLinesToDebugLog("Sneak Mode", {
		std::string("State: ") + (isSneaking ? "On" : "Off")
	});
}

static void PrintSneakModeMessage(bool isSneaking) {
	fo::func::display_print(isSneaking ? "Sneak on." : "Sneak off.");
}

static void MaybeLogSneakModeChange() {
	if (!IsGameLoaded() || !fo::var::obj_dude) {
		ResetSneakModeTracking();
		return;
	}

	// Track the actual player sneak toggle, not the sneak_working success check.
	const bool isSneaking = (fo::func::is_pc_flag(0) != 0);
	if (!sneakModeStateInitialized) {
		previousSneakModeState = isSneaking;
		sneakModeStateInitialized = true;
		return;
	}

	if (previousSneakModeState == isSneaking) return;

	previousSneakModeState = isSneaking;
	PrintSneakModeMessage(isSneaking);
	PrintSneakModeToDebugLog(isSneaking);
}

static const char* GetMessageText(const fo::MessageList* msgList, long msgId, const char* fallback) {
	if (msgList) {
		const char* text = fo::util::GetMessageStr(msgList, msgId);
		if (text && *text) return text;
	}
	return fallback;
}

static long GetGameGlobalVarValue(long gvar) {
	return (gvar >= 0 && gvar < static_cast<long>(fo::var::num_game_global_vars))
	     ? fo::var::game_global_vars[gvar]
	     : 0;
}

static bool IsGlobalVarSet(long gvar) {
	return (GetGameGlobalVarValue(gvar) != 0);
}

static bool IsEngineAddictionGvar(long gvar) {
	for (size_t i = 0; i < 9; i++) {
		if (fo::var::drugInfoList[i].addictGvar == gvar) return true;
	}
	return false;
}

static std::string FormatSimpleFlag(bool isActive, long rawValue = -1) {
	if (!isActive) return "No";
	if (rawValue >= 0) return "Yes (" + std::to_string(rawValue) + ")";
	return "Yes";
}

struct KarmaEntry {
	long gvar;
	long nameMsg;
};

struct GenericReputationEntry {
	long threshold;
	long nameMsg;
};

struct TownReputationEntry {
	long city;
	long gvar;
};

struct AddictionEntry {
	long gvar;
	long msgId;
	const char* fallbackName;
};

struct KillEntry {
	std::string name;
	long count;
};

static constexpr KarmaEntry kFallbackKarmaEntries[] = {
	{0, 1000},
	{3, 1001},
	{2, 1002},
	{1, 1003},
	{11, 1020},
	{319, 1016},
	{588, 1013},
	{589, 1015},
	{591, 1017},
	{6, 1018},
	{232, 1019},
	{590, 1021},
	{231, 1014},
	{592, 1022},
	{593, 1023},
	{594, 1024},
	{449, 1025},
};

static constexpr GenericReputationEntry kFallbackGeneralReputations[] = {
	{1000, 3000},
	{750, 3001},
	{500, 3002},
	{250, 3003},
	{-249, 3004},
	{-500, 3005},
	{-750, 3006},
	{-1000, 3007},
	{INT_MIN, 3008},
};

static constexpr TownReputationEntry kDefaultTownReputationEntries[] = {
	{0, 47},
	{2, 48},
	{1, 49},
	{4, 50},
	{5, 51},
	{3, 52},
	{8, 53},
	{6, 54},
	{7, 55},
	{13, 56},
	{10, 57},
	{11, 59},
	{14, 61},
	{17, 63},
	{19, 64},
	{18, 65},
	{25, 66},
	{9, 294},
	{20, 308},
};

static constexpr AddictionEntry kDefaultAddictions[] = {
	{fo::GVAR_NUKA_COLA_ADDICT, 1004, "Nuka-Cola Addiction"},
	{fo::GVAR_BUFF_OUT_ADDICT, 1005, "Buffout Addiction"},
	{fo::GVAR_MENTATS_ADDICT, 1006, "Mentats Addiction"},
	{fo::GVAR_PSYCHO_ADDICT, 1007, "Psycho Addiction"},
	{fo::GVAR_RADAWAY_ADDICT, 1008, "RadAway Addiction"},
	{fo::GVAR_ALCOHOL_ADDICT, 1009, "Alcohol Addiction"},
	{fo::GVAR_ADDICT_JET, 1010, "Jet Addiction"},
	{fo::GVAR_ADDICT_TRAGIC, 1011, "Tragic Addiction"},
};

static std::vector<KarmaEntry> LoadKarmaEntries() {
	std::vector<KarmaEntry> entries;
	if (auto* file = fo::func::db_fopen("data\\karmavar.txt", "rt")) {
		char line[256];
		while (fo::func::db_fgets(line, sizeof(line), file)) {
			char* cursor = line;
			while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
			if (*cursor == '#' || *cursor == '\0') continue;

			char* token = std::strtok(cursor, " \t,\r\n");
			if (!token) continue;
			KarmaEntry entry{};
			entry.gvar = std::strtol(token, nullptr, 10);

			token = std::strtok(nullptr, " \t,\r\n"); // art num
			if (!token) continue;
			token = std::strtok(nullptr, " \t,\r\n");
			if (!token) continue;
			entry.nameMsg = std::strtol(token, nullptr, 10);
			entries.push_back(entry);
		}
		fo::func::db_fclose(file);
	}

	if (entries.empty()) {
		entries.assign(std::begin(kFallbackKarmaEntries), std::end(kFallbackKarmaEntries));
	}

	return entries;
}

static std::vector<GenericReputationEntry> LoadGeneralReputationEntries() {
	std::vector<GenericReputationEntry> entries;
	if (auto* file = fo::func::db_fopen("data\\genrep.txt", "rt")) {
		char line[256];
		while (fo::func::db_fgets(line, sizeof(line), file)) {
			char* cursor = line;
			while (*cursor && std::isspace(static_cast<unsigned char>(*cursor))) cursor++;
			if (*cursor == '#' || *cursor == '\0') continue;

			char* token = std::strtok(cursor, " \t,\r\n");
			if (!token) continue;
			GenericReputationEntry entry{};
			entry.threshold = std::strtol(token, nullptr, 10);

			token = std::strtok(nullptr, " \t,\r\n");
			if (!token) continue;
			entry.nameMsg = std::strtol(token, nullptr, 10);
			entries.push_back(entry);
		}
		fo::func::db_fclose(file);
	}

	if (entries.empty()) {
		entries.assign(std::begin(kFallbackGeneralReputations), std::end(kFallbackGeneralReputations));
	}

	std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
		return left.threshold > right.threshold;
	});

	return entries;
}

static std::vector<TownReputationEntry> LoadTownReputationEntries() {
	std::vector<TownReputationEntry> entries;
	auto list = IniReader::GetConfigString("Misc", "CityReputationList", "");
	if (!list.empty()) {
		size_t start = 0;
		while (start <= list.length()) {
			size_t end = list.find(',', start);
			const auto token = list.substr(start, (end == std::string::npos) ? std::string::npos : end - start);
			const auto sep = token.find(':');
			if (sep != std::string::npos) {
				TownReputationEntry entry{};
				entry.city = std::strtol(token.substr(0, sep).c_str(), nullptr, 10);
				entry.gvar = std::strtol(token.substr(sep + 1).c_str(), nullptr, 10);
				entries.push_back(entry);
			}
			if (end == std::string::npos) break;
			start = end + 1;
		}
	}

	if (entries.empty()) {
		entries.assign(std::begin(kDefaultTownReputationEntries), std::end(kDefaultTownReputationEntries));
	}

	return entries;
}

static long GetTownReputationMessageId(long townReputation) {
	if (townReputation < -30) return 2006;
	if (townReputation < -15) return 2005;
	if (townReputation < 0) return 2004;
	if (townReputation == 0) return 2003;
	if (townReputation < 15) return 2002;
	if (townReputation < 30) return 2001;
	return 2000;
}

static bool LoadEditorMessageList(fo::MessageList& msgList) {
	return (fo::func::message_load(&msgList, "game\\editor.msg") == 1)
	    || (fo::func::message_load(&msgList, "editor.msg") == 1);
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

void Console::LogCharacterScreen() {
	auto* dude = fo::var::obj_dude;
	if (!dude) return;

	fo::MessageList editorMsg;
	const fo::MessageList* msgList = nullptr;
	if (LoadEditorMessageList(editorMsg)) msgList = &editorMsg;

	auto msg = [&](long msgId, const char* fallback) {
		return GetMessageText(msgList, msgId, fallback);
	};

	auto addFormattedLine = [](std::vector<std::string>& lines, const char* fmt, ...) {
		char buffer[512];
		va_list args;
		va_start(args, fmt);
		std::vsnprintf(buffer, sizeof(buffer), fmt, args);
		va_end(args);
		lines.emplace_back(buffer);
	};

	const char* dudeName = fo::func::critter_name(dude);
	if (!dudeName || !*dudeName) dudeName = "<unnamed>";

	const long level = fo::func::stat_pc_get(fo::PCSTAT_level);
	const long experience = fo::func::stat_pc_get(fo::PCSTAT_experience);
	const long nextLevelExp = fo::func::stat_pc_min_exp();
	const long skillPoints = fo::func::stat_pc_get(fo::PCSTAT_unspent_skill_points);
	const long gender = fo::func::stat_level(dude, fo::STAT_gender);
	const long age = fo::func::stat_level(dude, fo::STAT_age);

	std::vector<std::string> summary;
	addFormattedLine(summary, "%s: %s", msg(642, "Name"), dudeName);
	addFormattedLine(summary, "%s: %ld", msg(643, "Age"), age);
	addFormattedLine(summary, "%s: %s", msg(644, "Gender"), msg(645 + gender, (gender == fo::GENDER_FEMALE) ? "Female" : "Male"));
	if (HeroAppearance::appModEnabled) {
		addFormattedLine(summary, "Appearance Race Index: %ld", GetGlobalVar("HAp_Race"));
		addFormattedLine(summary, "Appearance Style Index: %ld", GetGlobalVar("HApStyle"));
	}
	addFormattedLine(summary, "%s: %ld", msg(647, "Level"), level);
	addFormattedLine(summary, "%s: %ld", msg(648, "Experience"), experience);
	addFormattedLine(summary, "%s: %ld", msg(649, "Next Level"), nextLevelExp);
	addFormattedLine(summary, "Skill Points: %ld", skillPoints);
	summary.emplace_back("");
	summary.emplace_back("SPECIAL");

	for (long stat = fo::STAT_st; stat <= fo::STAT_lu; stat++) {
		const long value = fo::func::stat_level(dude, stat);
		const char* statName = fo::var::stat_data[stat].name ? fo::var::stat_data[stat].name : "Stat";
		const char* description = fo::func::stat_level_description((value > 10) ? 10 : value);
		if (description && *description) {
			addFormattedLine(summary, "%-12s %2ld  %s", statName, value, description);
		} else {
			addFormattedLine(summary, "%-12s %2ld", statName, value);
		}
	}

	summary.emplace_back("");
	summary.emplace_back("Status");
	addFormattedLine(summary, "%-21s %ld/%ld", msg(300, "Hit Points"), dude->critter.health, fo::func::stat_level(dude, fo::STAT_max_hit_points));
	addFormattedLine(summary, "%-21s %s", msg(312, "Poisoned"), FormatSimpleFlag(dude->critter.poison != 0, dude->critter.poison).c_str());
	addFormattedLine(summary, "%-21s %s", msg(313, "Radiated"), FormatSimpleFlag(dude->critter.rads != 0, dude->critter.rads).c_str());
	addFormattedLine(summary, "%-21s %s", msg(314, "Eye Damage"), FormatSimpleFlag((dude->critter.damageFlags & fo::DamageFlag::DAM_BLIND) != 0).c_str());
	addFormattedLine(summary, "%-21s %s", msg(315, "Crippled Right Arm"), FormatSimpleFlag((dude->critter.damageFlags & fo::DamageFlag::DAM_CRIP_ARM_RIGHT) != 0).c_str());
	addFormattedLine(summary, "%-21s %s", msg(316, "Crippled Left Arm"), FormatSimpleFlag((dude->critter.damageFlags & fo::DamageFlag::DAM_CRIP_ARM_LEFT) != 0).c_str());
	addFormattedLine(summary, "%-21s %s", msg(317, "Crippled Right Leg"), FormatSimpleFlag((dude->critter.damageFlags & fo::DamageFlag::DAM_CRIP_LEG_RIGHT) != 0).c_str());
	addFormattedLine(summary, "%-21s %s", msg(318, "Crippled Left Leg"), FormatSimpleFlag((dude->critter.damageFlags & fo::DamageFlag::DAM_CRIP_LEG_LEFT) != 0).c_str());

	summary.emplace_back("");
	summary.emplace_back("Derived");
	long meleeDamage = fo::func::stat_level(dude, fo::STAT_melee_dmg);
	if (IniReader::GetConfigInt("Misc", "DisplayBonusDamage", 0) == 0) {
		meleeDamage -= (fo::func::perk_level(dude, fo::PERK_bonus_hth_damage) << 1);
	}
	addFormattedLine(summary, "%-21s %ld", msg(302, "Armor Class"), fo::func::stat_level(dude, fo::STAT_ac));
	addFormattedLine(summary, "%-21s %ld", msg(301, "Action Points"), fo::func::stat_level(dude, fo::STAT_max_move_points));
	addFormattedLine(summary, "%-21s %ld", msg(311, "Carry Weight"), fo::func::stat_level(dude, fo::STAT_carry_amt));
	addFormattedLine(summary, "%-21s %ld", msg(304, "Melee Damage"), meleeDamage);
	addFormattedLine(summary, "%-21s %ld%%", msg(305, "Damage Resistance"), fo::func::stat_level(dude, fo::STAT_dmg_resist));
	addFormattedLine(summary, "%-21s %ld%%", msg(306, "Poison Resistance"), fo::func::stat_level(dude, fo::STAT_poison_resist));
	addFormattedLine(summary, "%-21s %ld%%", msg(307, "Radiation Resistance"), fo::func::stat_level(dude, fo::STAT_rad_resist));
	addFormattedLine(summary, "%-21s %ld", msg(308, "Sequence"), fo::func::stat_level(dude, fo::STAT_sequence));
	addFormattedLine(summary, "%-21s %ld", msg(309, "Healing Rate"), fo::func::stat_level(dude, fo::STAT_heal_rate));
	addFormattedLine(summary, "%-21s %ld%%", msg(310, "Critical Chance"), fo::func::stat_level(dude, fo::STAT_crit_chance));
	PrintBoxedLinesToDebugLog("Character Screen: Summary", summary);

	std::vector<std::string> skills;
	for (long skill = 0; skill < fo::SKILL_count; skill++) {
		const char* skillName = fo::var::skill_data[skill].name ? fo::var::skill_data[skill].name : "Skill";
		addFormattedLine(skills, "%c %-24s %3ld%%", (fo::func::skill_is_tagged(skill) != 0) ? '*' : ' ', skillName, fo::func::skill_level(dude, skill));
	}
	PrintBoxedLinesToDebugLog("Character Screen: Skills", skills);

	std::vector<std::string> perksAndTraits;
	perksAndTraits.emplace_back("Traits");
	bool hasTraits = false;
	for (size_t index = 0; index < 2; index++) {
		const long trait = fo::var::pc_trait[index];
		if (trait >= 0 && trait < fo::TRAIT_count) {
			hasTraits = true;
			addFormattedLine(perksAndTraits, "  %s", fo::var::trait_data[trait].name ? fo::var::trait_data[trait].name : "Trait");
		}
	}
	if (!hasTraits) perksAndTraits.emplace_back("  (none)");

	perksAndTraits.emplace_back("");
	perksAndTraits.emplace_back("Perks");
	bool hasPerks = false;
	for (long perk = 0; perk < fo::PERK_count; perk++) {
		const long rank = fo::func::perk_level(dude, perk);
		if (rank <= 0) continue;
		hasPerks = true;
		const char* perkName = fo::var::perk_data[perk].name ? fo::var::perk_data[perk].name : "Perk";
		if (rank == 1) {
			addFormattedLine(perksAndTraits, "  %s", perkName);
		} else {
			addFormattedLine(perksAndTraits, "  %s (%ld)", perkName, rank);
		}
	}
	if (!hasPerks) perksAndTraits.emplace_back("  (none)");
	PrintBoxedLinesToDebugLog("Character Screen: Traits & Perks", perksAndTraits);

	std::vector<std::string> reputation;
	const auto karmaEntries = LoadKarmaEntries();
	const auto reputationEntries = LoadGeneralReputationEntries();
	const long playerReputation = GetGameGlobalVarValue(fo::GVAR_PLAYER_REPUTATION);

	const char* reputationTitle = "Unknown";
	for (const auto& entry : reputationEntries) {
		if (playerReputation >= entry.threshold) {
			reputationTitle = msg(entry.nameMsg, "Unknown");
			break;
		}
	}
	addFormattedLine(reputation, "%s: %ld (%s)", msg(125, "Reputation"), playerReputation, reputationTitle);

	bool hasKarmaEntries = false;
	for (const auto& entry : karmaEntries) {
		if (entry.gvar == fo::GVAR_PLAYER_REPUTATION) continue;
		if (!IsGlobalVarSet(entry.gvar)) continue;
		if (!hasKarmaEntries) {
			reputation.emplace_back("");
			reputation.emplace_back(msg(652, "Karma"));
			hasKarmaEntries = true;
		}
		addFormattedLine(reputation, "  %s", msg(entry.nameMsg, "Karma Entry"));
	}
	if (!hasKarmaEntries) {
		reputation.emplace_back("");
		reputation.emplace_back(msg(652, "Karma"));
		reputation.emplace_back("  (none)");
	}

	reputation.emplace_back("");
	reputation.emplace_back(msg(657, "Reputation"));
	bool hasTownReputations = false;
	for (const auto& entry : LoadTownReputationEntries()) {
		if (!fo::func::wmAreaIsKnown(entry.city)) continue;
		char townName[64] = {};
		fo::func::wmGetAreaIdxName(entry.city, townName);
		if (townName[0] == '\0') std::snprintf(townName, sizeof(townName), "City %ld", entry.city);
		addFormattedLine(reputation, "  %s: %s", townName, msg(GetTownReputationMessageId(GetGameGlobalVarValue(entry.gvar)), "Neutral"));
		hasTownReputations = true;
	}
	if (!hasTownReputations) reputation.emplace_back("  (none)");

	reputation.emplace_back("");
	reputation.emplace_back(msg(656, "Addictions"));
	bool hasAddictions = false;
	std::vector<long> seenAddictionGvars;
	for (const auto& entry : kDefaultAddictions) {
		if (!IsGlobalVarSet(entry.gvar)) continue;
		hasAddictions = true;
		seenAddictionGvars.push_back(entry.gvar);
		addFormattedLine(reputation, "  %s", msg(entry.msgId, entry.fallbackName));
	}
	if (drugs) {
		for (long index = 0, count = Drugs::GetDrugCount(); index < count; index++) {
			const long gvar = drugs[index].gvarID;
			if (gvar <= 0 || !IsGlobalVarSet(gvar)) continue;
			if (IsEngineAddictionGvar(gvar)) continue;
			if (std::find(seenAddictionGvars.begin(), seenAddictionGvars.end(), gvar) != seenAddictionGvars.end()) continue;
			seenAddictionGvars.push_back(gvar);
			hasAddictions = true;

			const char* fallback = fo::func::proto_get_msg_info(drugs[index].drugPid, 0);
			if (!fallback || !*fallback) fallback = "Custom Addiction";
			const long msgId = drugs[index].msgID;
			addFormattedLine(reputation, "  %s", (msgId > 0) ? msg(msgId, fallback) : fallback);
		}
	}
	if (!hasAddictions) reputation.emplace_back("  (none)");
	PrintBoxedLinesToDebugLog("Character Screen: Karma & Reputation", reputation);

	std::vector<KillEntry> killEntries;
	for (long killType = 0; killType < fo::KILL_TYPE_count; killType++) {
		const long count = fo::func::critter_kill_count(killType);
		if (count <= 0) continue;
		const char* killName = fo::func::critter_kill_name(killType);
		killEntries.push_back({(killName && *killName) ? killName : "Kill Type", count});
	}
	std::sort(killEntries.begin(), killEntries.end(), [](const auto& left, const auto& right) {
		if (left.count != right.count) return left.count > right.count;
		return left.name < right.name;
	});

	std::vector<std::string> kills;
	if (killEntries.empty()) {
		kills.emplace_back("(none)");
	} else {
		for (const auto& entry : killEntries) {
			addFormattedLine(kills, "%-24s %ld", entry.name.c_str(), entry.count);
		}
	}
	PrintBoxedLinesToDebugLog("Character Screen: Kills", kills);

	if (msgList) fo::func::message_exit(&editorMsg);
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

	LoadGameHook::OnGameReset() += ResetSneakModeTracking;
	LoadGameHook::OnGameExit() += ResetSneakModeTracking;
	MainLoopHook::OnMainLoop() += MaybeLogSneakModeChange;
	MainLoopHook::OnCombatLoop() += MaybeLogSneakModeChange;
}

void Console::exit() {
	if (consoleFile.is_open()) consoleFile.close();
	messageBoxToDebugLogEnabled = false;
	floatingTextToDebugLogEnabled = false;
	ResetSneakModeTracking();
}

}
