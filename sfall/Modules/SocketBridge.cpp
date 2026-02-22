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

#include <winsock.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "..\main.h"
#include "..\Config.h"
#include "..\IniReader.h"
#include "..\InputFuncs.h"
#include "..\Logging.h"
#include "..\Utils.h"

#include "LoadGameHook.h"
#include "MainLoopHook.h"

#include "SocketBridge.h"

namespace sfall
{

namespace
{

constexpr int SOCKET_BRIDGE_DEFAULT_PORT = 47111;
constexpr int SOCKET_BRIDGE_DEFAULT_QUEUE_MAX = 128;
constexpr int SOCKET_BRIDGE_DEFAULT_DRAIN = 8;
constexpr int SOCKET_BRIDGE_DEFAULT_RECONNECT_MS = 1000;
constexpr int SOCKET_BRIDGE_DEFAULT_MAX_LINE = 8192;
constexpr int SOCKET_BRIDGE_DEFAULT_KEY = 39;

const std::array<const char*, 7> kBridgeGlobalNames = {
	"MCTILE00",
	"MCOBJ000",
	"MCINVOBJ",
	"MCMODE00",
	"MCINIT00",
	"MCSELT00",
	"MCLOOT00"
};

enum class ArgKind {
	NONE,
	INT,
	FLOAT,
	STRING,
};

struct ArgValue {
	ArgKind kind = ArgKind::NONE;
	long intValue = 0;
	double floatValue = 0.0;
	std::string stringValue;
};

struct ExecResult {
	bool ok = false;
	int status = -1;
	ArgValue value;
	std::string detail;
};

enum class RouteKind {
	DISPATCHER,
	WRAPPER,
};

struct CallConfig {
	std::string name;
	bool enabled = true;
	bool unsafeCall = false;
	int minArgs = 0;
	int maxArgs = 0;
	RouteKind route = RouteKind::DISPATCHER;

	int dispatchKeyScancode = SOCKET_BRIDGE_DEFAULT_KEY;
	std::string setMCMODE00;
	std::string setMCOBJ000;
	std::string setMCINVOBJ;
	std::string setMCSELT00;
	std::string setMCINIT00;

	std::string wrapperName;
	std::unordered_map<int, std::string> argTypes;
};

struct BridgeSettings {
	bool enable = false;
	std::string host = "127.0.0.1";
	int port = SOCKET_BRIDGE_DEFAULT_PORT;
	int reconnectDelayMs = SOCKET_BRIDGE_DEFAULT_RECONNECT_MS;
	int queueMax = SOCKET_BRIDGE_DEFAULT_QUEUE_MAX;
	int drainPerFrame = SOCKET_BRIDGE_DEFAULT_DRAIN;
	int maxLineBytes = SOCKET_BRIDGE_DEFAULT_MAX_LINE;
	bool allowJson = true;
	bool allowCli = true;
	std::string authToken;
	std::string callsFile = "artifacts\\bridge_calls.ini";
	bool emitEvents = true;
	bool logProtocol = false;
};

struct QueuedRequest {
	std::string id;
	CallConfig call;
	std::vector<ArgValue> args;
};

struct ParsedLine {
	enum class Kind {
		INVALID,
		AUTH,
		REQUEST,
	};

	Kind kind = Kind::INVALID;
	std::string id;
	std::string call;
	std::string authToken;
	std::vector<ArgValue> args;
	std::string error;
};

using WrapperFunc = ExecResult(*)(const std::vector<ArgValue>& args);

static BridgeSettings gSettings;
static std::map<std::string, CallConfig, ci_less> gCallMap;
static std::unordered_map<std::string, WrapperFunc> gWrapperMap;

static std::deque<QueuedRequest> gRequestQueue;
static std::deque<std::string> gOutboundQueue;
static std::mutex gRequestMutex;
static std::mutex gOutboundMutex;
static std::mutex gSocketMutex;

static std::thread gListenerThread;
static std::atomic<bool> gRunning(false);
static std::atomic<bool> gModuleEnabled(false);
static std::atomic<bool> gAuthenticated(false);
static std::atomic<bool> gSocketConnected(false);
static SOCKET gSocket = INVALID_SOCKET;
static bool gWsaStarted = false;

static std::string TrimCopy(const std::string& value) {
	size_t start = 0;
	while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
		start++;
	}
	size_t end = value.size();
	while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
		end--;
	}
	return value.substr(start, end - start);
}

static std::string ToLowerCopy(std::string value) {
	std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
	return value;
}

static bool StartsWithNoCase(const std::string& text, const std::string& prefix) {
	if (text.size() < prefix.size()) return false;
	for (size_t i = 0; i < prefix.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(text[i])) != std::tolower(static_cast<unsigned char>(prefix[i]))) {
			return false;
		}
	}
	return true;
}

static bool EndsWithNoCase(const std::string& text, const std::string& suffix) {
	if (text.size() < suffix.size()) return false;
	const size_t offset = text.size() - suffix.size();
	for (size_t i = 0; i < suffix.size(); i++) {
		if (std::tolower(static_cast<unsigned char>(text[offset + i])) != std::tolower(static_cast<unsigned char>(suffix[i]))) {
			return false;
		}
	}
	return true;
}

static bool ParseLongStrict(const std::string& text, long& out) {
	char* end = nullptr;
	errno = 0;
	long value = strtol(text.c_str(), &end, 0);
	if (errno != 0 || end == nullptr) return false;
	while (*end != '\0') {
		if (!std::isspace(static_cast<unsigned char>(*end))) {
			return false;
		}
		end++;
	}
	out = value;
	return true;
}

static int MaxInt(int a, int b) {
	return (a > b) ? a : b;
}

static bool ParseDoubleStrict(const std::string& text, double& out) {
	char* end = nullptr;
	errno = 0;
	double value = strtod(text.c_str(), &end);
	if (errno != 0 || end == nullptr) return false;
	while (*end != '\0') {
		if (!std::isspace(static_cast<unsigned char>(*end))) {
			return false;
		}
		end++;
	}
	out = value;
	return true;
}

static bool IsAllowedBridgeGlobal(const std::string& rawName, std::string& normalized) {
	std::string name = ToLowerCopy(TrimCopy(rawName));
	for (const char* item : kBridgeGlobalNames) {
		std::string expected = ToLowerCopy(item);
		if (name == expected) {
			normalized = item;
			return true;
		}
	}
	return false;
}

static bool ResolvePtrName(const std::string& ptrExpr, long& out, std::string& error) {
	if (!StartsWithNoCase(ptrExpr, "ptr:")) {
		error = "pointer expression must use ptr:NAME";
		return false;
	}
	std::string gvarName;
	if (!IsAllowedBridgeGlobal(ptrExpr.substr(4), gvarName)) {
		error = "unknown pointer namespace key";
		return false;
	}
	out = GetGlobalVar(gvarName.c_str());
	return true;
}

static bool ArgToLong(const ArgValue& value, long& out, std::string& error) {
	switch (value.kind) {
	case ArgKind::INT:
		out = value.intValue;
		return true;
	case ArgKind::FLOAT:
		out = static_cast<long>(value.floatValue);
		return true;
	case ArgKind::STRING:
		if (StartsWithNoCase(value.stringValue, "ptr:")) {
			return ResolvePtrName(value.stringValue, out, error);
		}
		if (ParseLongStrict(value.stringValue, out)) {
			return true;
		}
		error = "expected integer-compatible argument";
		return false;
	default:
		error = "missing argument value";
		return false;
	}
}

static std::string JsonEscape(const std::string& input) {
	std::string out;
	out.reserve(input.size() + 8);
	for (char c : input) {
		switch (c) {
		case '\\': out += "\\\\"; break;
		case '"': out += "\\\""; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if (static_cast<unsigned char>(c) < 0x20) {
				out += '?';
			} else {
				out += c;
			}
		}
	}
	return out;
}

static void QueueOutboundLine(const std::string& payload) {
	std::lock_guard<std::mutex> lock(gOutboundMutex);
	gOutboundQueue.push_back(payload + "\n");
}

static void EmitEvent(const std::string& eventName, const std::string& detail) {
	if (!gSettings.emitEvents) return;
	std::string msg = "{\"type\":\"event\",\"event\":\"" + JsonEscape(eventName) + "\"";
	if (!detail.empty()) {
		msg += ",\"detail\":\"" + JsonEscape(detail) + "\"";
	}
	msg += "}";
	QueueOutboundLine(msg);
}

static void EmitAck(const std::string& id, const std::string& status, const std::string& error) {
	std::string msg = "{\"type\":\"ack\",\"id\":\"" + JsonEscape(id) + "\",\"status\":\"" + JsonEscape(status) + "\"";
	if (!error.empty()) {
		msg += ",\"error\":\"" + JsonEscape(error) + "\"";
	}
	msg += "}";
	QueueOutboundLine(msg);
}

static std::string BuildResultJson(const std::string& id, const ExecResult& result) {
	std::string msg = "{\"type\":\"result\",\"id\":\"" + JsonEscape(id) + "\",\"ok\":" + std::string(result.ok ? "true" : "false") + ",\"status\":" + std::to_string(result.status);
	if (!result.detail.empty()) {
		msg += ",\"detail\":\"" + JsonEscape(result.detail) + "\"";
	}

	if (result.value.kind == ArgKind::INT) {
		msg += ",\"result_type\":\"int\",\"result\":" + std::to_string(result.value.intValue);
	} else if (result.value.kind == ArgKind::FLOAT) {
		msg += ",\"result_type\":\"float\",\"result\":" + std::to_string(result.value.floatValue);
	} else if (result.value.kind == ArgKind::STRING) {
		msg += ",\"result_type\":\"string\",\"result\":\"" + JsonEscape(result.value.stringValue) + "\"";
	}

	msg += "}";
	return msg;
}

static bool ParseJsonStringAt(const std::string& line, size_t& pos, std::string& out, std::string& error) {
	if (pos >= line.size() || line[pos] != '"') {
		error = "expected string";
		return false;
	}
	pos++;
	out.clear();
	while (pos < line.size()) {
		char c = line[pos++];
		if (c == '"') return true;
		if (c == '\\') {
			if (pos >= line.size()) {
				error = "incomplete escape";
				return false;
			}
			char e = line[pos++];
			switch (e) {
			case '"': out.push_back('"'); break;
			case '\\': out.push_back('\\'); break;
			case '/': out.push_back('/'); break;
			case 'b': out.push_back('\b'); break;
			case 'f': out.push_back('\f'); break;
			case 'n': out.push_back('\n'); break;
			case 'r': out.push_back('\r'); break;
			case 't': out.push_back('\t'); break;
			default:
				error = "unsupported string escape";
				return false;
			}
		} else {
			out.push_back(c);
		}
	}
	error = "unterminated string";
	return false;
}

static void SkipSpaces(const std::string& line, size_t& pos) {
	while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) {
		pos++;
	}
}

static bool ParseJsonBareAt(const std::string& line, size_t& pos, std::string& out) {
	const size_t begin = pos;
	while (pos < line.size()) {
		char c = line[pos];
		if (c == ',' || c == ']' || c == '}') {
			break;
		}
		pos++;
	}
	out = TrimCopy(line.substr(begin, pos - begin));
	return !out.empty();
}

static bool ParseJsonArgsArray(const std::string& line, size_t& pos, std::vector<ArgValue>& args, std::string& error) {
	if (pos >= line.size() || line[pos] != '[') {
		error = "args must be an array";
		return false;
	}
	pos++;
	SkipSpaces(line, pos);
	if (pos < line.size() && line[pos] == ']') {
		pos++;
		return true;
	}

	while (pos < line.size()) {
		SkipSpaces(line, pos);
		ArgValue value;
		if (pos >= line.size()) {
			error = "unexpected end of args";
			return false;
		}

		if (line[pos] == '"') {
			std::string str;
			if (!ParseJsonStringAt(line, pos, str, error)) return false;
			value.kind = ArgKind::STRING;
			value.stringValue = str;
		} else {
			std::string bare;
			if (!ParseJsonBareAt(line, pos, bare)) {
				error = "invalid args token";
				return false;
			}
			if (bare == "true") {
				value.kind = ArgKind::INT;
				value.intValue = 1;
			} else if (bare == "false" || bare == "null") {
				value.kind = ArgKind::INT;
				value.intValue = 0;
			} else {
				double floatValue;
				long intValue;
				if (ParseLongStrict(bare, intValue)) {
					value.kind = ArgKind::INT;
					value.intValue = intValue;
				} else if (ParseDoubleStrict(bare, floatValue)) {
					value.kind = ArgKind::FLOAT;
					value.floatValue = floatValue;
				} else {
					error = "unsupported JSON argument type";
					return false;
				}
			}
		}
		args.push_back(std::move(value));

		SkipSpaces(line, pos);
		if (pos >= line.size()) {
			error = "unterminated args array";
			return false;
		}
		if (line[pos] == ',') {
			pos++;
			continue;
		}
		if (line[pos] == ']') {
			pos++;
			return true;
		}
		error = "invalid args separator";
		return false;
	}
	return false;
}

static bool FindJsonKeyValue(const std::string& line, const std::string& key, size_t& valuePos) {
	const std::string pattern = "\"" + key + "\"";
	size_t start = 0;
	while (true) {
		size_t keyPos = line.find(pattern, start);
		if (keyPos == std::string::npos) return false;
		size_t colonPos = line.find(':', keyPos + pattern.size());
		if (colonPos == std::string::npos) return false;
		valuePos = colonPos + 1;
		return true;
	}
}

static bool ParseJsonLine(const std::string& line, ParsedLine& parsed) {
	parsed = ParsedLine();
	std::string error;

	size_t authPos = 0;
	if (FindJsonKeyValue(line, "auth", authPos)) {
		SkipSpaces(line, authPos);
		std::string token;
		if (authPos < line.size() && line[authPos] == '"' && ParseJsonStringAt(line, authPos, token, error)) {
			parsed.kind = ParsedLine::Kind::AUTH;
			parsed.authToken = token;
			return true;
		}
	}
	if (FindJsonKeyValue(line, "type", authPos)) {
		SkipSpaces(line, authPos);
		std::string typeValue;
		if (authPos < line.size() && line[authPos] == '"' && ParseJsonStringAt(line, authPos, typeValue, error)) {
			if (ToLowerCopy(typeValue) == "auth") {
				size_t tokenPos = 0;
				if (!FindJsonKeyValue(line, "token", tokenPos)) {
					parsed.error = "missing auth token";
					return false;
				}
				SkipSpaces(line, tokenPos);
				std::string token;
				if (tokenPos < line.size() && line[tokenPos] == '"' && ParseJsonStringAt(line, tokenPos, token, error)) {
					parsed.kind = ParsedLine::Kind::AUTH;
					parsed.authToken = token;
					return true;
				}
				parsed.error = "invalid auth token";
				return false;
			}
		}
	}

	size_t idPos = 0;
	size_t callPos = 0;
	if (!FindJsonKeyValue(line, "id", idPos)) {
		parsed.error = "missing id";
		return false;
	}
	if (!FindJsonKeyValue(line, "call", callPos)) {
		parsed.error = "missing call";
		return false;
	}

	SkipSpaces(line, idPos);
	SkipSpaces(line, callPos);

	if (idPos < line.size() && line[idPos] == '"') {
		if (!ParseJsonStringAt(line, idPos, parsed.id, parsed.error)) return false;
	} else {
		std::string idBare;
		if (!ParseJsonBareAt(line, idPos, idBare)) {
			parsed.error = "invalid id";
			return false;
		}
		parsed.id = idBare;
	}
	if (callPos >= line.size() || line[callPos] != '"' || !ParseJsonStringAt(line, callPos, parsed.call, parsed.error)) {
		parsed.error = "invalid call";
		return false;
	}

	size_t argsPos = 0;
	if (FindJsonKeyValue(line, "args", argsPos)) {
		SkipSpaces(line, argsPos);
		if (!ParseJsonArgsArray(line, argsPos, parsed.args, parsed.error)) {
			return false;
		}
	}

	parsed.kind = ParsedLine::Kind::REQUEST;
	return true;
}

static bool TokenizeCli(const std::string& line, std::vector<std::string>& tokens, std::string& error) {
	tokens.clear();
	std::string current;
	bool inQuotes = false;
	bool escaping = false;

	for (char c : line) {
		if (escaping) {
			current.push_back(c);
			escaping = false;
			continue;
		}
		if (c == '\\') {
			escaping = true;
			continue;
		}
		if (c == '"') {
			inQuotes = !inQuotes;
			continue;
		}
		if (!inQuotes && std::isspace(static_cast<unsigned char>(c))) {
			if (!current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
			continue;
		}
		current.push_back(c);
	}

	if (escaping) {
		error = "dangling escape in CLI payload";
		return false;
	}
	if (inQuotes) {
		error = "unterminated quote in CLI payload";
		return false;
	}
	if (!current.empty()) {
		tokens.push_back(current);
	}
	return true;
}

static ArgValue ParseCliArgument(const std::string& token) {
	ArgValue arg;
	if (StartsWithNoCase(token, "ptr:")) {
		arg.kind = ArgKind::STRING;
		arg.stringValue = token;
		return arg;
	}

	long longValue;
	if (ParseLongStrict(token, longValue)) {
		arg.kind = ArgKind::INT;
		arg.intValue = longValue;
		return arg;
	}

	double doubleValue;
	if (ParseDoubleStrict(token, doubleValue)) {
		arg.kind = ArgKind::FLOAT;
		arg.floatValue = doubleValue;
		return arg;
	}

	arg.kind = ArgKind::STRING;
	arg.stringValue = token;
	return arg;
}

static bool ParseCliLine(const std::string& line, ParsedLine& parsed) {
	parsed = ParsedLine();
	std::vector<std::string> tokens;
	std::string error;
	if (!TokenizeCli(line, tokens, error)) {
		parsed.error = error;
		return false;
	}
	if (tokens.empty()) {
		parsed.error = "empty line";
		return false;
	}

	if (ToLowerCopy(tokens[0]) == "auth") {
		if (tokens.size() < 2) {
			parsed.error = "missing auth token";
			return false;
		}
		parsed.kind = ParsedLine::Kind::AUTH;
		parsed.authToken = tokens[1];
		return true;
	}

	size_t offset = 0;
	if (ToLowerCopy(tokens[0]) == "req") {
		offset = 1;
	}
	if (tokens.size() < (offset + 2)) {
		parsed.error = "request format: req <id> <call> [args...]";
		return false;
	}

	parsed.kind = ParsedLine::Kind::REQUEST;
	parsed.id = tokens[offset + 0];
	parsed.call = tokens[offset + 1];
	for (size_t i = offset + 2; i < tokens.size(); i++) {
		parsed.args.push_back(ParseCliArgument(tokens[i]));
	}
	return true;
}

static bool ParseIncomingLine(const std::string& line, ParsedLine& parsed) {
	const std::string trimmed = TrimCopy(line);
	if (trimmed.empty()) {
		parsed.error = "empty line";
		return false;
	}

	if (gSettings.allowJson && !trimmed.empty() && trimmed.front() == '{') {
		if (ParseJsonLine(trimmed, parsed)) {
			return true;
		}
		if (!gSettings.allowCli) {
			return false;
		}
	}

	if (gSettings.allowCli) {
		return ParseCliLine(trimmed, parsed);
	}

	parsed.error = "unsupported payload format";
	return false;
}

static std::string GetSectionString(const Config::Section& section, const char* key, const char* fallback = "") {
	auto it = section.find(key);
	if (it == section.end()) return fallback;
	return TrimCopy(it->second);
}

static int GetSectionInt(const Config::Section& section, const char* key, int fallback) {
	auto it = section.find(key);
	if (it == section.end()) return fallback;
	const long value = StrToLong(it->second.c_str(), 0);
	return static_cast<int>(value);
}

static void LoadBuiltInCallMap() {
	gCallMap.clear();

	{
		CallConfig call;
		call.name = "bridge_action";
		call.route = RouteKind::DISPATCHER;
		call.dispatchKeyScancode = SOCKET_BRIDGE_DEFAULT_KEY;
		call.minArgs = 1;
		call.maxArgs = 1;
		call.setMCMODE00 = "arg:0";
		call.argTypes[0] = "int";
		gCallMap[call.name] = std::move(call);
	}
	{
		CallConfig call;
		call.name = "bridge_action_use_selected";
		call.route = RouteKind::DISPATCHER;
		call.dispatchKeyScancode = SOCKET_BRIDGE_DEFAULT_KEY;
		call.minArgs = 0;
		call.maxArgs = 0;
		call.setMCMODE00 = "1";
		gCallMap[call.name] = std::move(call);
	}
	{
		CallConfig call;
		call.name = "bridge_action_loot_open";
		call.route = RouteKind::DISPATCHER;
		call.dispatchKeyScancode = SOCKET_BRIDGE_DEFAULT_KEY;
		call.minArgs = 0;
		call.maxArgs = 0;
		call.setMCMODE00 = "2";
		gCallMap[call.name] = std::move(call);
	}
	{
		CallConfig call;
		call.name = "bridge_action_close";
		call.route = RouteKind::DISPATCHER;
		call.dispatchKeyScancode = SOCKET_BRIDGE_DEFAULT_KEY;
		call.minArgs = 0;
		call.maxArgs = 0;
		call.setMCMODE00 = "4";
		gCallMap[call.name] = std::move(call);
	}
	{
		CallConfig call;
		call.name = "bridge_action_talk";
		call.route = RouteKind::DISPATCHER;
		call.dispatchKeyScancode = SOCKET_BRIDGE_DEFAULT_KEY;
		call.minArgs = 0;
		call.maxArgs = 0;
		call.setMCMODE00 = "5";
		gCallMap[call.name] = std::move(call);
	}
	{
		CallConfig call;
		call.name = "tap_key";
		call.route = RouteKind::WRAPPER;
		call.wrapperName = "tap_key";
		call.minArgs = 1;
		call.maxArgs = 1;
		call.argTypes[0] = "int";
		gCallMap[call.name] = std::move(call);
	}
	{
		CallConfig call;
		call.name = "key_down";
		call.route = RouteKind::WRAPPER;
		call.wrapperName = "key_down";
		call.minArgs = 1;
		call.maxArgs = 1;
		call.argTypes[0] = "int";
		gCallMap[call.name] = std::move(call);
	}
	{
		CallConfig call;
		call.name = "set_global_int8";
		call.route = RouteKind::WRAPPER;
		call.wrapperName = "set_global_int8";
		call.unsafeCall = true;
		call.minArgs = 2;
		call.maxArgs = 2;
		call.argTypes[0] = "string";
		call.argTypes[1] = "int";
		gCallMap[call.name] = std::move(call);
	}
	{
		CallConfig call;
		call.name = "get_global_int8";
		call.route = RouteKind::WRAPPER;
		call.wrapperName = "get_global_int8";
		call.minArgs = 1;
		call.maxArgs = 1;
		call.argTypes[0] = "string";
		gCallMap[call.name] = std::move(call);
	}
}

static bool LoadCallMap() {
	gCallMap.clear();

	Config config;
	if (!config.read(gSettings.callsFile.c_str(), false)) {
		dlog_f("SocketBridge: failed to read call map file '%s'; using built-in defaults.", DL_INIT, gSettings.callsFile.c_str());
		LoadBuiltInCallMap();
		dlog_f("SocketBridge: loaded %d built-in call definitions.", DL_INIT, static_cast<int>(gCallMap.size()));
		return !gCallMap.empty();
	}

	for (const auto& sectionPair : config.data()) {
		const std::string& sectionName = sectionPair.first;
		if (!StartsWithNoCase(sectionName, "Call.")) {
			continue;
		}

		CallConfig call;
		call.name = TrimCopy(sectionName.substr(5));
		if (call.name.empty()) continue;

		const auto& section = sectionPair.second;
		call.enabled = GetSectionInt(section, "Enabled", 1) != 0;
		call.unsafeCall = GetSectionInt(section, "Unsafe", 0) != 0;
		call.minArgs = MaxInt(0, GetSectionInt(section, "MinArgs", 0));
		call.maxArgs = MaxInt(call.minArgs, GetSectionInt(section, "MaxArgs", call.minArgs));

		std::string route = ToLowerCopy(GetSectionString(section, "Route", "dispatcher"));
		if (route == "wrapper") {
			call.route = RouteKind::WRAPPER;
		} else {
			call.route = RouteKind::DISPATCHER;
		}

		call.dispatchKeyScancode = GetSectionInt(section, "DispatchKeyScancode", SOCKET_BRIDGE_DEFAULT_KEY);
		if (call.dispatchKeyScancode <= 0) call.dispatchKeyScancode = SOCKET_BRIDGE_DEFAULT_KEY;

		call.setMCMODE00 = GetSectionString(section, "Set_MCMODE00");
		call.setMCOBJ000 = GetSectionString(section, "Set_MCOBJ000");
		call.setMCINVOBJ = GetSectionString(section, "Set_MCINVOBJ");
		call.setMCSELT00 = GetSectionString(section, "Set_MCSELT00");
		call.setMCINIT00 = GetSectionString(section, "Set_MCINIT00");
		call.wrapperName = GetSectionString(section, "WrapperName");

		int maxArgTypeIndex = -1;
		for (const auto& keyValue : section) {
			const std::string& key = keyValue.first;
			if (!StartsWithNoCase(key, "Arg") || !EndsWithNoCase(key, "Type")) continue;
			const std::string middle = key.substr(3, key.size() - 3 - 4);
			long index;
			if (!ParseLongStrict(middle, index)) continue;
			if (index < 0 || index > 64) continue;
			const int argIndex = static_cast<int>(index);
			call.argTypes[argIndex] = ToLowerCopy(TrimCopy(keyValue.second));
			if (argIndex > maxArgTypeIndex) maxArgTypeIndex = argIndex;
		}

		if (call.maxArgs == call.minArgs && maxArgTypeIndex >= 0 && call.maxArgs < (maxArgTypeIndex + 1)) {
			call.maxArgs = maxArgTypeIndex + 1;
		}

		if (call.route == RouteKind::WRAPPER && call.wrapperName.empty()) {
			dlog_f("SocketBridge: skipping [Call.%s], wrapper route without WrapperName.", DL_INIT, call.name.c_str());
			continue;
		}

		gCallMap[call.name] = std::move(call);
	}

	if (gCallMap.empty()) {
		dlog_f("SocketBridge: no [Call.*] sections found in %s; using built-in defaults.", DL_INIT, gSettings.callsFile.c_str());
		LoadBuiltInCallMap();
		dlog_f("SocketBridge: loaded %d built-in call definitions.", DL_INIT, static_cast<int>(gCallMap.size()));
		return !gCallMap.empty();
	}

	dlog_f("SocketBridge: loaded %d call definitions from %s.", DL_INIT, static_cast<int>(gCallMap.size()), gSettings.callsFile.c_str());
	return true;
}

static bool ValidateArgs(const CallConfig& call, const std::vector<ArgValue>& args, std::string& error) {
	if (static_cast<int>(args.size()) < call.minArgs || static_cast<int>(args.size()) > call.maxArgs) {
		error = "invalid argument count";
		return false;
	}

	for (size_t i = 0; i < args.size(); i++) {
		const ArgValue& arg = args[i];
		if (arg.kind == ArgKind::STRING && StartsWithNoCase(arg.stringValue, "ptr:")) {
			long resolved = 0;
			if (!ResolvePtrName(arg.stringValue, resolved, error)) {
				error = "arg" + std::to_string(static_cast<int>(i)) + " " + error;
				return false;
			}
		}
	}

	for (const auto& typedArg : call.argTypes) {
		const int index = typedArg.first;
		if (index < 0 || index >= static_cast<int>(args.size())) continue;
		const std::string typeName = ToLowerCopy(typedArg.second);
		const ArgValue& arg = args[index];

		if (typeName == "int") {
			if (arg.kind != ArgKind::INT) {
				error = "arg" + std::to_string(index) + " expects int";
				return false;
			}
		} else if (typeName == "float") {
			if (arg.kind != ArgKind::INT && arg.kind != ArgKind::FLOAT) {
				error = "arg" + std::to_string(index) + " expects float";
				return false;
			}
		} else if (typeName == "string") {
			if (arg.kind != ArgKind::STRING) {
				error = "arg" + std::to_string(index) + " expects string";
				return false;
			}
		} else if (typeName == "ptr") {
			if (arg.kind != ArgKind::STRING || !StartsWithNoCase(arg.stringValue, "ptr:")) {
				error = "arg" + std::to_string(index) + " expects ptr:NAME";
				return false;
			}
			long resolved = 0;
			if (!ResolvePtrName(arg.stringValue, resolved, error)) {
				error = "arg" + std::to_string(index) + " " + error;
				return false;
			}
		}
	}
	return true;
}

static bool ResolveExpression(const std::string& expr, const std::vector<ArgValue>& args, long& out, std::string& error) {
	const std::string source = TrimCopy(expr);
	if (source.empty()) {
		error = "empty expression";
		return false;
	}

	if (StartsWithNoCase(source, "ptr:")) {
		return ResolvePtrName(source, out, error);
	}

	if (StartsWithNoCase(source, "arg:")) {
		long index;
		if (!ParseLongStrict(source.substr(4), index) || index < 0 || index >= static_cast<long>(args.size())) {
			error = "arg index out of bounds";
			return false;
		}
		return ArgToLong(args[static_cast<size_t>(index)], out, error);
	}

	if (StartsWithNoCase(source, "arg") && source.size() > 3) {
		long index;
		if (!ParseLongStrict(source.substr(3), index) || index < 0 || index >= static_cast<long>(args.size())) {
			error = "arg index out of bounds";
			return false;
		}
		return ArgToLong(args[static_cast<size_t>(index)], out, error);
	}

	if (ParseLongStrict(source, out)) {
		return true;
	}

	error = "unsupported expression: " + source;
	return false;
}

static bool SetBridgeGlobalFromExpr(const char* gvar, const std::string& expr, const std::vector<ArgValue>& args, std::string& error) {
	if (expr.empty()) return true;
	long value;
	if (!ResolveExpression(expr, args, value, error)) {
		error = std::string(gvar) + " -> " + error;
		return false;
	}
	if (SetGlobalVar(gvar, value) != 0) {
		error = std::string("failed to set global ") + gvar;
		return false;
	}
	return true;
}

static ExecResult ExecuteDispatcherCall(const CallConfig& call, const std::vector<ArgValue>& args) {
	ExecResult result;
	result.value.kind = ArgKind::INT;
	result.value.intValue = -1;

	if (!IsGameLoaded()) {
		result.status = -20;
		result.detail = "game is not loaded";
		return result;
	}

	std::string error;
	if (!SetBridgeGlobalFromExpr("MCMODE00", call.setMCMODE00, args, error) ||
		!SetBridgeGlobalFromExpr("MCOBJ000", call.setMCOBJ000, args, error) ||
		!SetBridgeGlobalFromExpr("MCINVOBJ", call.setMCINVOBJ, args, error) ||
		!SetBridgeGlobalFromExpr("MCSELT00", call.setMCSELT00, args, error) ||
		!SetBridgeGlobalFromExpr("MCINIT00", call.setMCINIT00, args, error)) {
		result.status = -21;
		result.detail = error;
		return result;
	}

	TapKey(static_cast<DWORD>(call.dispatchKeyScancode));
	result.ok = true;
	result.status = 0;
	result.value.intValue = 0;
	result.detail = "dispatched";
	return result;
}

static ExecResult WrapperTapKey(const std::vector<ArgValue>& args) {
	ExecResult result;
	result.value.kind = ArgKind::INT;
	result.value.intValue = -1;
	long scancode;
	std::string error;
	if (args.empty() || !ArgToLong(args[0], scancode, error)) {
		result.status = -30;
		result.detail = "tap_key expects one integer argument";
		return result;
	}
	TapKey(static_cast<DWORD>(scancode));
	result.ok = true;
	result.status = 0;
	result.value.intValue = 0;
	result.detail = "key tapped";
	return result;
}

static ExecResult WrapperSetGlobalInt8(const std::vector<ArgValue>& args) {
	ExecResult result;
	result.value.kind = ArgKind::INT;
	result.value.intValue = -1;
	if (args.size() < 2 || args[0].kind != ArgKind::STRING) {
		result.status = -31;
		result.detail = "set_global_int8 expects (string,int)";
		return result;
	}
	long value;
	std::string error;
	if (!ArgToLong(args[1], value, error)) {
		result.status = -31;
		result.detail = "set_global_int8 invalid value";
		return result;
	}
	if (SetGlobalVar(args[0].stringValue.c_str(), value) != 0) {
		result.status = -31;
		result.detail = "global name must be exactly 8 chars";
		return result;
	}
	result.ok = true;
	result.status = 0;
	result.value.intValue = value;
	result.detail = "global set";
	return result;
}

static ExecResult WrapperGetGlobalInt8(const std::vector<ArgValue>& args) {
	ExecResult result;
	result.value.kind = ArgKind::INT;
	result.value.intValue = 0;
	if (args.size() < 1 || args[0].kind != ArgKind::STRING) {
		result.status = -32;
		result.detail = "get_global_int8 expects (string)";
		return result;
	}
	result.ok = true;
	result.status = 0;
	result.value.intValue = GetGlobalVar(args[0].stringValue.c_str());
	return result;
}

static ExecResult WrapperKeyDown(const std::vector<ArgValue>& args) {
	ExecResult result;
	result.value.kind = ArgKind::INT;
	result.value.intValue = 0;
	if (args.size() < 1) {
		result.status = -33;
		result.detail = "key_down expects one argument";
		return result;
	}
	long key;
	std::string error;
	if (!ArgToLong(args[0], key, error)) {
		result.status = -33;
		result.detail = "key_down invalid key argument";
		return result;
	}
	result.ok = true;
	result.status = 0;
	result.value.intValue = KeyDown(static_cast<DWORD>(key)) ? 1 : 0;
	return result;
}

static ExecResult ExecuteWrapperCall(const CallConfig& call, const std::vector<ArgValue>& args) {
	ExecResult result;
	result.value.kind = ArgKind::INT;
	result.value.intValue = -1;

	const std::string key = ToLowerCopy(call.wrapperName);
	auto wrapperIt = gWrapperMap.find(key);
	if (wrapperIt == gWrapperMap.end()) {
		result.status = -40;
		result.detail = "wrapper is not registered";
		return result;
	}
	return wrapperIt->second(args);
}

static ExecResult ExecuteRequest(const QueuedRequest& request) {
	if (request.call.route == RouteKind::DISPATCHER) {
		return ExecuteDispatcherCall(request.call, request.args);
	}
	return ExecuteWrapperCall(request.call, request.args);
}

static void DrainRequestsOnMainThread() {
	if (!gModuleEnabled || !gRunning) return;

	const int drainLimit = MaxInt(1, gSettings.drainPerFrame);
	for (int i = 0; i < drainLimit; i++) {
		QueuedRequest request;
		{
			std::lock_guard<std::mutex> lock(gRequestMutex);
			if (gRequestQueue.empty()) {
				break;
			}
			request = std::move(gRequestQueue.front());
			gRequestQueue.pop_front();
		}

		ExecResult result = ExecuteRequest(request);
		QueueOutboundLine(BuildResultJson(request.id, result));
		if (gSettings.emitEvents) {
			EmitEvent("call_completed", request.call.name + ":" + (result.ok ? "ok" : "error"));
		}
	}
}

static bool SendAll(SOCKET sock, const std::string& payload) {
	const char* data = payload.c_str();
	int left = static_cast<int>(payload.size());
	while (left > 0) {
		const int sent = send(sock, data, left, 0);
		if (sent == SOCKET_ERROR || sent == 0) {
			return false;
		}
		left -= sent;
		data += sent;
	}
	return true;
}

static bool FlushOutboundQueue(SOCKET sock) {
	while (true) {
		std::string payload;
		{
			std::lock_guard<std::mutex> lock(gOutboundMutex);
			if (gOutboundQueue.empty()) {
				return true;
			}
			payload = std::move(gOutboundQueue.front());
			gOutboundQueue.pop_front();
		}
		if (gSettings.logProtocol) {
			dlog_f("SocketBridge TX: %s", DL_INIT, payload.c_str());
		}
		if (!SendAll(sock, payload)) {
			return false;
		}
	}
}

static void SleepInterruptible(int milliseconds) {
	const int slice = 50;
	int remaining = MaxInt(0, milliseconds);
	while (gRunning && remaining > 0) {
		Sleep(static_cast<DWORD>((remaining > slice) ? slice : remaining));
		remaining -= slice;
	}
}

static void CloseSocketInternal() {
	std::lock_guard<std::mutex> lock(gSocketMutex);
	if (gSocket != INVALID_SOCKET) {
		closesocket(gSocket);
		gSocket = INVALID_SOCKET;
	}
	gSocketConnected = false;
}

static SOCKET ConnectToServer() {
	in_addr hostAddr;
	hostAddr.s_addr = inet_addr(gSettings.host.c_str());
	if (hostAddr.s_addr == INADDR_NONE) {
		hostent* hostEntry = gethostbyname(gSettings.host.c_str());
		if (!hostEntry || !hostEntry->h_addr_list || !hostEntry->h_addr_list[0]) {
			return INVALID_SOCKET;
		}
		memcpy(&hostAddr, hostEntry->h_addr_list[0], sizeof(hostAddr));
	}

	sockaddr_in addr;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_port = htons(static_cast<u_short>(gSettings.port));
	addr.sin_addr = hostAddr;

	SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_SOCKET) {
		return INVALID_SOCKET;
	}

	u_long nonBlocking = 1;
	if (ioctlsocket(sock, FIONBIO, &nonBlocking) == SOCKET_ERROR) {
		closesocket(sock);
		return INVALID_SOCKET;
	}

	int rc = connect(sock, reinterpret_cast<const sockaddr*>(&addr), sizeof(addr));
	if (rc == SOCKET_ERROR) {
		int err = WSAGetLastError();
		if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS && err != WSAEINVAL) {
			closesocket(sock);
			return INVALID_SOCKET;
		}

		fd_set wfds;
		FD_ZERO(&wfds);
		FD_SET(sock, &wfds);
		timeval timeout;
		timeout.tv_sec = 1;
		timeout.tv_usec = 0;
		rc = select(0, nullptr, &wfds, nullptr, &timeout);
		if (rc <= 0 || !FD_ISSET(sock, &wfds)) {
			closesocket(sock);
			return INVALID_SOCKET;
		}
		int soError = 0;
		int len = sizeof(soError);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
		if (soError != 0) {
			closesocket(sock);
			return INVALID_SOCKET;
		}
	}

	nonBlocking = 0;
	ioctlsocket(sock, FIONBIO, &nonBlocking);
	return sock;
}

static bool HandleParsedLine(const ParsedLine& parsed) {
	if (parsed.kind == ParsedLine::Kind::AUTH) {
		if (gSettings.authToken.empty()) {
			gAuthenticated = true;
			QueueOutboundLine("{\"type\":\"auth\",\"ok\":true,\"detail\":\"auth not required\"}");
			return true;
		}
		if (parsed.authToken == gSettings.authToken) {
			gAuthenticated = true;
			QueueOutboundLine("{\"type\":\"auth\",\"ok\":true}");
			EmitEvent("authenticated", "ok");
			return true;
		}
		QueueOutboundLine("{\"type\":\"auth\",\"ok\":false,\"error\":\"invalid token\"}");
		return false;
	}

	if (parsed.kind != ParsedLine::Kind::REQUEST) {
		EmitAck("", "rejected", parsed.error.empty() ? "invalid payload" : parsed.error);
		return false;
	}

	if (!gAuthenticated) {
		EmitAck(parsed.id, "rejected", "unauthenticated");
		return false;
	}

	auto callIt = gCallMap.find(parsed.call);
	if (callIt == gCallMap.end()) {
		EmitAck(parsed.id, "rejected", "unknown call");
		return false;
	}

	const CallConfig& call = callIt->second;
	if (!call.enabled) {
		EmitAck(parsed.id, "rejected", "call disabled");
		return false;
	}
	if (call.unsafeCall && IniReader::GetIntDefaultConfig("Debugging", "AllowUnsafeScripting", 0) != 1) {
		EmitAck(parsed.id, "rejected", "unsafe call requires AllowUnsafeScripting=1");
		return false;
	}

	std::string error;
	if (!ValidateArgs(call, parsed.args, error)) {
		EmitAck(parsed.id, "rejected", error);
		return false;
	}

	{
		std::lock_guard<std::mutex> lock(gRequestMutex);
		if (static_cast<int>(gRequestQueue.size()) >= gSettings.queueMax) {
			EmitAck(parsed.id, "rejected", "queue full");
			return false;
		}
		QueuedRequest request;
		request.id = parsed.id;
		request.call = call;
		request.args = parsed.args;
		gRequestQueue.push_back(std::move(request));
	}

	EmitAck(parsed.id, "queued", "");
	return true;
}

static void ListenerThreadMain() {
	std::string inputBuffer;

	while (gRunning) {
		SOCKET active = INVALID_SOCKET;
		{
			std::lock_guard<std::mutex> lock(gSocketMutex);
			active = gSocket;
		}

		if (active == INVALID_SOCKET) {
			SOCKET sock = ConnectToServer();
			if (sock == INVALID_SOCKET) {
				SleepInterruptible(gSettings.reconnectDelayMs);
				continue;
			}
			{
				std::lock_guard<std::mutex> lock(gSocketMutex);
				gSocket = sock;
			}
			gSocketConnected = true;
			gAuthenticated = gSettings.authToken.empty();
			EmitEvent("connected", gSettings.host + ":" + std::to_string(gSettings.port));
			dlog_f("SocketBridge: connected to %s:%d", DL_INIT, gSettings.host.c_str(), gSettings.port);
		}
		if (active == INVALID_SOCKET) continue;

		if (!FlushOutboundQueue(active)) {
			dlogr("SocketBridge: send failed, reconnecting.", DL_INIT);
			EmitEvent("disconnected", "send failed");
			CloseSocketInternal();
			SleepInterruptible(gSettings.reconnectDelayMs);
			continue;
		}

		fd_set readSet;
		FD_ZERO(&readSet);
		FD_SET(active, &readSet);
		timeval timeout;
		timeout.tv_sec = 0;
		timeout.tv_usec = 120000;

		int rc = select(0, &readSet, nullptr, nullptr, &timeout);
		if (rc == SOCKET_ERROR) {
			dlogr("SocketBridge: select failed, reconnecting.", DL_INIT);
			EmitEvent("disconnected", "select failed");
			CloseSocketInternal();
			SleepInterruptible(gSettings.reconnectDelayMs);
			continue;
		}
		if (rc == 0 || !FD_ISSET(active, &readSet)) {
			continue;
		}

		char buffer[2048];
		int received = recv(active, buffer, sizeof(buffer), 0);
		if (received <= 0) {
			dlogr("SocketBridge: recv returned <= 0, reconnecting.", DL_INIT);
			EmitEvent("disconnected", "recv failed");
			CloseSocketInternal();
			SleepInterruptible(gSettings.reconnectDelayMs);
			continue;
		}

		inputBuffer.append(buffer, received);
		while (true) {
			size_t lineEnd = inputBuffer.find('\n');
			if (lineEnd == std::string::npos) break;

			std::string line = inputBuffer.substr(0, lineEnd);
			inputBuffer.erase(0, lineEnd + 1);
			if (!line.empty() && line.back() == '\r') line.pop_back();

			if (static_cast<int>(line.size()) > gSettings.maxLineBytes) {
				EmitAck("", "rejected", "line too long");
				continue;
			}
			if (gSettings.logProtocol) {
				dlog_f("SocketBridge RX: %s", DL_INIT, line.c_str());
			}

			ParsedLine parsed;
			if (!ParseIncomingLine(line, parsed)) {
				EmitAck(parsed.id, "rejected", parsed.error.empty() ? "invalid payload" : parsed.error);
				continue;
			}
			HandleParsedLine(parsed);
		}
	}

	CloseSocketInternal();
}

static void LoadSettings() {
	gSettings.enable = (IniReader::GetIntDefaultConfig("SocketBridge", "Enable", 0) != 0);
	gSettings.host = TrimCopy(IniReader::GetStringDefaultConfig("SocketBridge", "ServerHost", "127.0.0.1"));
	if (gSettings.host.empty()) gSettings.host = "127.0.0.1";
	{
		const std::string hostLower = ToLowerCopy(gSettings.host);
		if (hostLower != "127.0.0.1" && hostLower != "localhost") {
			dlog_f("SocketBridge: non-localhost host '%s' is not allowed; forcing 127.0.0.1.", DL_INIT, gSettings.host.c_str());
			gSettings.host = "127.0.0.1";
		}
	}
	gSettings.port = clamp(IniReader::GetIntDefaultConfig("SocketBridge", "ServerPort", SOCKET_BRIDGE_DEFAULT_PORT), 1, 65535);
	gSettings.reconnectDelayMs = clamp(IniReader::GetIntDefaultConfig("SocketBridge", "ReconnectDelayMs", SOCKET_BRIDGE_DEFAULT_RECONNECT_MS), 100, 60000);
	gSettings.queueMax = clamp(IniReader::GetIntDefaultConfig("SocketBridge", "QueueMax", SOCKET_BRIDGE_DEFAULT_QUEUE_MAX), 1, 4096);
	gSettings.drainPerFrame = clamp(IniReader::GetIntDefaultConfig("SocketBridge", "DrainPerFrame", SOCKET_BRIDGE_DEFAULT_DRAIN), 1, 256);
	gSettings.maxLineBytes = clamp(IniReader::GetIntDefaultConfig("SocketBridge", "MaxLineBytes", SOCKET_BRIDGE_DEFAULT_MAX_LINE), 128, 1024 * 1024);
	gSettings.allowJson = (IniReader::GetIntDefaultConfig("SocketBridge", "AllowJson", 1) != 0);
	gSettings.allowCli = (IniReader::GetIntDefaultConfig("SocketBridge", "AllowCli", 1) != 0);
	gSettings.authToken = TrimCopy(IniReader::GetStringDefaultConfig("SocketBridge", "AuthToken", ""));
	gSettings.callsFile = TrimCopy(IniReader::GetStringDefaultConfig("SocketBridge", "CallsFile", "artifacts\\bridge_calls.ini"));
	if (gSettings.callsFile.empty()) gSettings.callsFile = "artifacts\\bridge_calls.ini";
	gSettings.emitEvents = (IniReader::GetIntDefaultConfig("SocketBridge", "EmitEvents", 1) != 0);
	gSettings.logProtocol = (IniReader::GetIntDefaultConfig("SocketBridge", "LogProtocol", 0) != 0);
}

static void InitWrapperRegistry() {
	gWrapperMap.clear();
	gWrapperMap.emplace("tap_key", WrapperTapKey);
	gWrapperMap.emplace("set_global_int8", WrapperSetGlobalInt8);
	gWrapperMap.emplace("get_global_int8", WrapperGetGlobalInt8);
	gWrapperMap.emplace("key_down", WrapperKeyDown);
}

} // namespace

void SocketBridge::init() {
	LoadSettings();
	if (!isDebug) {
		dlogr("SocketBridge: disabled (requires [Debugging] Enable=1).", DL_INIT);
		return;
	}
	if (!gSettings.enable) {
		dlogr("SocketBridge: disabled by config.", DL_INIT);
		return;
	}
	if (!gSettings.allowJson && !gSettings.allowCli) {
		dlogr("SocketBridge: disabled (both AllowJson and AllowCli are 0).", DL_INIT);
		return;
	}

	if (!LoadCallMap()) {
		dlogr("SocketBridge: disabled (failed to load call map).", DL_INIT);
		return;
	}
	InitWrapperRegistry();

	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
		dlogr("SocketBridge: WSAStartup failed.", DL_MAIN);
		return;
	}
	gWsaStarted = true;

	gRunning = true;
	gModuleEnabled = true;
	gAuthenticated = gSettings.authToken.empty();
	gListenerThread = std::thread(ListenerThreadMain);

	MainLoopHook::OnMainLoop() += DrainRequestsOnMainThread;
	MainLoopHook::OnCombatLoop() += DrainRequestsOnMainThread;

	dlog_f("SocketBridge: enabled host=%s port=%d calls=%s", DL_INIT, gSettings.host.c_str(), gSettings.port, gSettings.callsFile.c_str());
}

void SocketBridge::exit() {
	gModuleEnabled = false;
	if (!gRunning) return;

	gRunning = false;
	CloseSocketInternal();
	if (gListenerThread.joinable()) {
		gListenerThread.join();
	}
	if (gWsaStarted) {
		WSACleanup();
		gWsaStarted = false;
	}

	{
		std::lock_guard<std::mutex> reqLock(gRequestMutex);
		gRequestQueue.clear();
	}
	{
		std::lock_guard<std::mutex> outLock(gOutboundMutex);
		gOutboundQueue.clear();
	}

	dlogr("SocketBridge: stopped.", DL_INIT);
}

} // namespace sfall
