#include "win_config.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <shlobj.h>
#include <windows.h>
#endif

namespace yuntype::win {

namespace {

std::filesystem::path utf8ToFsPath(const std::string &path) {
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
}

bool readFileUtf8(const std::string &path, std::string &out) {
    if (path.empty()) {
        return false;
    }
    const std::filesystem::path p = utf8ToFsPath(path);
    std::error_code ec;
    const auto sz = std::filesystem::file_size(p, ec);
    if (ec || sz == 0 || sz > (4U << 20)) {
        return false;
    }
    std::ifstream in(p, std::ios::binary);
    if (!in) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    out = ss.str();
    return true;
}

bool writeFileAtomicUtf8(const std::string &path, std::string_view data) {
    if (path.empty()) {
        return false;
    }
    const std::filesystem::path target = utf8ToFsPath(path);
    std::error_code ec;
    if (const auto parent = target.parent_path(); !parent.empty()) {
        std::filesystem::create_directories(parent, ec);
        if (ec) {
            return false;
        }
    }
#ifdef _WIN32
    const std::string tmpUtf8 = path + ".tmp." + std::to_string(::_getpid());
    const std::filesystem::path tmpPath = utf8ToFsPath(tmpUtf8);
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }
    if (!::MoveFileExW(tmpPath.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
    return true;
#else
    const std::string tmpUtf8 = path + ".tmp";
    const std::filesystem::path tmpPath = utf8ToFsPath(tmpUtf8);
    {
        std::ofstream out(tmpPath, std::ios::binary | std::ios::trunc);
        if (!out) {
            return false;
        }
        out.write(data.data(), static_cast<std::streamsize>(data.size()));
        out.flush();
        if (!out.good()) {
            out.close();
            std::filesystem::remove(tmpPath, ec);
            return false;
        }
    }
    std::filesystem::rename(tmpPath, target, ec);
    if (ec) {
        std::filesystem::remove(tmpPath, ec);
        return false;
    }
    return true;
#endif
}

struct JsonCursor {
    std::string_view s;
    std::size_t pos = 0;

    void skipWs() {
        while (pos < s.size() && std::isspace(static_cast<unsigned char>(s[pos]))) {
            ++pos;
        }
    }

    bool consume(char ch) {
        skipWs();
        if (pos < s.size() && s[pos] == ch) {
            ++pos;
            return true;
        }
        return false;
    }

    static bool parseHex4(std::string_view sv, std::size_t &p, unsigned int &out) {
        if (p + 4 > sv.size()) {
            return false;
        }
        unsigned int cp = 0;
        for (int i = 0; i < 4; ++i) {
            cp <<= 4;
            const char h = sv[p++];
            if (h >= '0' && h <= '9') cp |= static_cast<unsigned int>(h - '0');
            else if (h >= 'a' && h <= 'f') cp |= static_cast<unsigned int>(h - 'a' + 10);
            else if (h >= 'A' && h <= 'F') cp |= static_cast<unsigned int>(h - 'A' + 10);
            else return false;
        }
        out = cp;
        return true;
    }

    static void appendUtf8(std::string &out, unsigned int cp) {
        if (cp <= 0x7F) {
            out.push_back(static_cast<char>(cp));
        } else if (cp <= 0x7FF) {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else if (cp <= 0xFFFF) {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        } else {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool readString(std::string &out) {
        if (!consume('"')) {
            return false;
        }
        out.clear();
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') {
                return true;
            }
            if (c == '\\') {
                if (pos >= s.size()) {
                    return false;
                }
                const char esc = s[pos++];
                switch (esc) {
                case '"':
                case '\\':
                case '/':
                    out.push_back(esc);
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'u': {
                    unsigned int cp = 0;
                    if (!parseHex4(s, pos, cp)) {
                        return false;
                    }
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos + 6 <= s.size() &&
                        s[pos] == '\\' && s[pos + 1] == 'u') {
                        const std::size_t saved = pos;
                        pos += 2;
                        unsigned int low = 0;
                        if (parseHex4(s, pos, low) && low >= 0xDC00 && low <= 0xDFFF) {
                            cp = 0x10000 + (((cp - 0xD800) << 10) | (low - 0xDC00));
                        } else {
                            pos = saved;
                        }
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default:
                    return false;
                }
            } else {
                out.push_back(c);
            }
        }
        return false;
    }

    bool readBool(bool &out) {
        skipWs();
        if (s.substr(pos).starts_with("true")) {
            pos += 4;
            out = true;
            return true;
        }
        if (s.substr(pos).starts_with("false")) {
            pos += 5;
            out = false;
            return true;
        }
        return false;
    }

    bool readInt(int &out) {
        skipWs();
        if (pos >= s.size()) {
            return false;
        }
        bool neg = false;
        if (s[pos] == '-') {
            neg = true;
            ++pos;
        }
        if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') {
            return false;
        }
        long long val = 0;
        while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
            val = val * 10 + (s[pos] - '0');
            ++pos;
        }
        out = static_cast<int>(neg ? -val : val);
        return true;
    }

    bool skipValue(int depth = 0) {
        if (depth > 32) {
            return false;
        }
        skipWs();
        if (pos >= s.size()) {
            return false;
        }
        const char c = s[pos];
        if (c == '"') {
            std::string dummy;
            return readString(dummy);
        }
        if (c == '{') {
            ++pos;
            if (consume('}')) {
                return true;
            }
            while (true) {
                std::string k;
                if (!readString(k) || !consume(':') || !skipValue(depth + 1)) {
                    return false;
                }
                if (consume('}')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
            }
        }
        if (c == '[') {
            ++pos;
            if (consume(']')) {
                return true;
            }
            while (true) {
                if (!skipValue(depth + 1)) {
                    return false;
                }
                if (consume(']')) {
                    return true;
                }
                if (!consume(',')) {
                    return false;
                }
            }
        }
        if (s.substr(pos).starts_with("true")) {
            pos += 4;
            return true;
        }
        if (s.substr(pos).starts_with("false")) {
            pos += 5;
            return true;
        }
        if (s.substr(pos).starts_with("null")) {
            pos += 4;
            return true;
        }
        if (c == '-' || (c >= '0' && c <= '9')) {
            if (s[pos] == '-') ++pos;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
            if (pos < s.size() && s[pos] == '.') {
                ++pos;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
            }
            if (pos < s.size() && (s[pos] == 'e' || s[pos] == 'E')) {
                ++pos;
                if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) ++pos;
                while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') ++pos;
            }
            return true;
        }
        return false;
    }
};

bool parseModelArray(JsonCursor &c, std::vector<ModelProfile> &out) {
    if (!c.consume('[')) {
        return false;
    }
    out.clear();
    if (c.consume(']')) {
        return true;
    }
    while (true) {
        if (!c.consume('{')) {
            return false;
        }
        ModelProfile p;
        if (!c.consume('}')) {
            while (true) {
                std::string k;
                if (!c.readString(k) || !c.consume(':')) {
                    return false;
                }
                if (k == "name") {
                    if (!c.readString(p.name)) return false;
                } else if (k == "base_url") {
                    if (!c.readString(p.baseUrl)) return false;
                } else if (k == "model") {
                    if (!c.readString(p.model)) return false;
                } else if (k == "api_key") {
                    if (!c.readString(p.apiKey)) return false;
                } else if (k == "thinking_mode") {
                    if (!c.readString(p.thinkingMode)) return false;
                } else if (k == "extra_request_json") {
                    if (!c.readString(p.extraRequestJson)) return false;
                } else {
                    if (!c.skipValue()) return false;
                }
                if (c.consume('}')) {
                    break;
                }
                if (!c.consume(',')) {
                    return false;
                }
            }
        }
        if (p.thinkingMode.empty()) {
            p.thinkingMode = "default";
        }
        if (!p.name.empty()) {
            out.push_back(std::move(p));
        }
        if (c.consume(']')) {
            break;
        }
        if (!c.consume(',')) {
            return false;
        }
    }
    return true;
}

void appendModelArrayJson(std::string &out, const std::vector<ModelProfile> &models) {
    out.push_back('[');
    for (std::size_t i = 0; i < models.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        const auto &m = models[i];
        out += "{\"name\":" + yuntype::jsonQuote(m.name);
        out += ",\"base_url\":" + yuntype::jsonQuote(m.baseUrl);
        out += ",\"model\":" + yuntype::jsonQuote(m.model);
        out += ",\"api_key\":" + yuntype::jsonQuote(m.apiKey);
        out += ",\"thinking_mode\":" + yuntype::jsonQuote(m.thinkingMode.empty() ? "default" : m.thinkingMode);
        out += ",\"extra_request_json\":" + yuntype::jsonQuote(m.extraRequestJson);
        out.push_back('}');
    }
    out.push_back(']');
}

void clampConfig(AppConfig &cfg) {
    cfg.historyCount = std::clamp(cfg.historyCount, 1, 300);
    cfg.persistIdleSeconds = std::clamp(cfg.persistIdleSeconds, 1, 3600);
    cfg.persistMaxSeconds = std::clamp(cfg.persistMaxSeconds, 1, 86400);
    cfg.shortCandidateThreshold = std::clamp(cfg.shortCandidateThreshold, 0, 200);
    cfg.candidatePageSize = std::clamp(cfg.candidatePageSize, 2, 10);
    if (cfg.triggerKey.empty()) {
        cfg.triggerKey = "Alt+U";
    }
    if (!cfg.models.empty() && !cfg.findActiveModel()) {
        cfg.activeModel = cfg.models.front().name;
    }
}

#ifdef _WIN32
std::string wideToUtf8Local(std::wstring_view ws) {
    if (ws.empty()) {
        return {};
    }
    const int needed = ::WideCharToMultiByte(
        CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(
        CP_UTF8, 0, ws.data(), static_cast<int>(ws.size()), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring utf8ToWideLocal(std::string_view s) {
    if (s.empty()) {
        return {};
    }
    const int needed = ::MultiByteToWideChar(
        CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), needed);
    return out;
}
#endif

} // namespace

const ModelProfile *AppConfig::findActiveModel() const {
    for (const auto &m : models) {
        if (m.name == activeModel) {
            return &m;
        }
    }
    if (!models.empty()) {
        return &models.front();
    }
    return nullptr;
}

yuntype::LlmConfig AppConfig::toLlmConfig(const std::string &keyFileFallback) const {
    yuntype::LlmConfig out;
    if (const auto *m = findActiveModel()) {
        out.baseUrl = m->baseUrl;
        out.model = m->model;
        out.apiKey = m->apiKey;
        out.thinkingMode = m->thinkingMode.empty() ? "default" : m->thinkingMode;
        out.extraRequestJson = m->extraRequestJson;
    }
    out.keyFile = keyFileFallback;
    out.prompt = sentencePrompt;
    out.shortCandidateThreshold = std::clamp(shortCandidateThreshold, 0, 200);
    out.candidatePageSize = std::clamp(candidatePageSize, 2, 10);
    return out;
}

TriggerSpec parseTriggerKey(std::string_view spec) {
    TriggerSpec out;
    out.modifiers = 0;
    out.vk = 'U';
    std::string keyToken;
    std::size_t pos = 0;
    while (pos < spec.size()) {
        std::size_t plus = spec.find('+', pos);
        if (plus == std::string_view::npos) {
            plus = spec.size();
        }
        std::string tok(spec.substr(pos, plus - pos));
        pos = plus + 1;
        // Trim whitespace
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.front()))) tok.erase(tok.begin());
        while (!tok.empty() && std::isspace(static_cast<unsigned char>(tok.back()))) tok.pop_back();
        std::string lower = tok;
        for (char &ch : lower) {
            ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        }
        if (lower == "alt") {
            out.modifiers |= 1U;
        } else if (lower == "ctrl" || lower == "control") {
            out.modifiers |= 2U;
        } else if (lower == "shift") {
            out.modifiers |= 4U;
        } else if (lower == "win" || lower == "super" || lower == "meta") {
            out.modifiers |= 8U;
        } else if (!lower.empty()) {
            keyToken = lower;
        }
    }
    if (keyToken.size() == 1) {
        const unsigned char c = static_cast<unsigned char>(keyToken[0]);
        if (c >= 'a' && c <= 'z') {
            out.vk = static_cast<unsigned int>('A' + (c - 'a'));
        } else if (c >= '0' && c <= '9') {
            out.vk = static_cast<unsigned int>(c);
        } else if (c == ';') {
            out.vk = 0xBA; // VK_OEM_1
        } else if (c == '`') {
            out.vk = 0xC0; // VK_OEM_3
        } else if (c == '/') {
            out.vk = 0xBF; // VK_OEM_2
        }
    } else if (keyToken == "space") {
        out.vk = 0x20;
    } else if (keyToken == "tab") {
        out.vk = 0x09;
    } else if (keyToken.size() >= 2 && keyToken[0] == 'f') {
        const int fn = std::atoi(keyToken.c_str() + 1);
        if (fn >= 1 && fn <= 12) {
            out.vk = static_cast<unsigned int>(0x70 + (fn - 1)); // VK_F1..VK_F12
        }
    }
    if (out.modifiers == 0 && (out.vk < 0x70 || out.vk > 0x7B)) {
        out.modifiers = 1U; // Safety default: Alt+<key> unless function key
    }
    return out;
}

std::string serializeModelsJson(const AppConfig &cfg) {
    std::string out = "{\"active\":" + yuntype::jsonQuote(cfg.activeModel) + ",\"profiles\":";
    appendModelArrayJson(out, cfg.models);
    out += "}\n";
    return out;
}

bool deserializeModelsJson(std::string_view json, AppConfig &cfg) {
    JsonCursor c{json, 0};
    if (!c.consume('{')) {
        return false;
    }
    std::string active;
    std::vector<ModelProfile> models;
    bool hasField = false;
    if (!c.consume('}')) {
        while (true) {
            std::string k;
            if (!c.readString(k) || !c.consume(':')) {
                return false;
            }
            if (k == "active") {
                if (!c.readString(active)) return false;
                hasField = true;
            } else if (k == "profiles") {
                if (!parseModelArray(c, models)) return false;
                hasField = true;
            } else {
                if (!c.skipValue()) return false;
            }
            if (c.consume('}')) {
                break;
            }
            if (!c.consume(',')) {
                return false;
            }
        }
    }
    if (!hasField) {
        return false;
    }
    cfg.activeModel = std::move(active);
    cfg.models = std::move(models);
    clampConfig(cfg);
    return true;
}

std::string serializeAppConfigJson(const AppConfig &cfg) {
    std::string out = "{";
    out += "\"enabled\":" + std::string(cfg.enabled ? "true" : "false");
    out += ",\"trigger_key\":" + yuntype::jsonQuote(cfg.triggerKey);
    out += ",\"history_enabled\":" + std::string(cfg.historyEnabled ? "true" : "false");
    out += ",\"history_count\":" + std::to_string(cfg.historyCount);
    out += ",\"persist_idle_seconds\":" + std::to_string(cfg.persistIdleSeconds);
    out += ",\"persist_max_seconds\":" + std::to_string(cfg.persistMaxSeconds);
    out += ",\"short_candidate_threshold\":" + std::to_string(cfg.shortCandidateThreshold);
    out += ",\"candidate_page_size\":" + std::to_string(cfg.candidatePageSize);
    out += ",\"sentence_prompt\":" + yuntype::jsonQuote(cfg.sentencePrompt);
    out += ",\"auto_start\":" + std::string(cfg.autoStart ? "true" : "false");
    out += "}\n";
    return out;
}

bool deserializeAppConfigJson(std::string_view json, AppConfig &cfg) {
    JsonCursor c{json, 0};
    if (!c.consume('{')) {
        return false;
    }
    if (!c.consume('}')) {
        while (true) {
            std::string k;
            if (!c.readString(k) || !c.consume(':')) {
                return false;
            }
            if (k == "enabled") {
                if (!c.readBool(cfg.enabled)) return false;
            } else if (k == "trigger_key") {
                if (!c.readString(cfg.triggerKey)) return false;
            } else if (k == "history_enabled") {
                if (!c.readBool(cfg.historyEnabled)) return false;
            } else if (k == "history_count") {
                if (!c.readInt(cfg.historyCount)) return false;
            } else if (k == "persist_idle_seconds") {
                if (!c.readInt(cfg.persistIdleSeconds)) return false;
            } else if (k == "persist_max_seconds") {
                if (!c.readInt(cfg.persistMaxSeconds)) return false;
            } else if (k == "short_candidate_threshold") {
                if (!c.readInt(cfg.shortCandidateThreshold)) return false;
            } else if (k == "candidate_page_size") {
                if (!c.readInt(cfg.candidatePageSize)) return false;
            } else if (k == "sentence_prompt") {
                if (!c.readString(cfg.sentencePrompt)) return false;
            } else if (k == "auto_start") {
                if (!c.readBool(cfg.autoStart)) return false;
            } else {
                if (!c.skipValue()) return false;
            }
            if (c.consume('}')) {
                break;
            }
            if (!c.consume(',')) {
                return false;
            }
        }
    }
    clampConfig(cfg);
    return true;
}

std::string serializeSettingsStateJson(const AppConfig &cfg, std::size_t historyBytes) {
    std::string out = "{";
    out += "\"enabled\":" + std::string(cfg.enabled ? "true" : "false");
    out += ",\"trigger_key\":" + yuntype::jsonQuote(cfg.triggerKey);
    out += ",\"history_enabled\":" + std::string(cfg.historyEnabled ? "true" : "false");
    out += ",\"history_count\":" + std::to_string(cfg.historyCount);
    out += ",\"persist_idle_seconds\":" + std::to_string(cfg.persistIdleSeconds);
    out += ",\"persist_max_seconds\":" + std::to_string(cfg.persistMaxSeconds);
    out += ",\"short_candidate_threshold\":" + std::to_string(cfg.shortCandidateThreshold);
    out += ",\"candidate_page_size\":" + std::to_string(cfg.candidatePageSize);
    out += ",\"sentence_prompt\":" + yuntype::jsonQuote(cfg.sentencePrompt);
    out += ",\"auto_start\":" + std::string(cfg.autoStart ? "true" : "false");
    out += ",\"active\":" + yuntype::jsonQuote(cfg.activeModel);
    out += ",\"profiles\":";
    appendModelArrayJson(out, cfg.models);
    out += ",\"history_bytes\":" + std::to_string(historyBytes);
    out += "}";
    return out;
}

bool applySettingsStateJson(std::string_view json, AppConfig &cfg) {
    JsonCursor c{json, 0};
    if (!c.consume('{')) {
        return false;
    }
    AppConfig updated = cfg;
    if (!c.consume('}')) {
        while (true) {
            std::string k;
            if (!c.readString(k) || !c.consume(':')) {
                return false;
            }
            if (k == "enabled") {
                if (!c.readBool(updated.enabled)) return false;
            } else if (k == "trigger_key") {
                if (!c.readString(updated.triggerKey)) return false;
            } else if (k == "history_enabled") {
                if (!c.readBool(updated.historyEnabled)) return false;
            } else if (k == "history_count") {
                if (!c.readInt(updated.historyCount)) return false;
            } else if (k == "persist_idle_seconds") {
                if (!c.readInt(updated.persistIdleSeconds)) return false;
            } else if (k == "persist_max_seconds") {
                if (!c.readInt(updated.persistMaxSeconds)) return false;
            } else if (k == "short_candidate_threshold") {
                if (!c.readInt(updated.shortCandidateThreshold)) return false;
            } else if (k == "candidate_page_size") {
                if (!c.readInt(updated.candidatePageSize)) return false;
            } else if (k == "sentence_prompt") {
                if (!c.readString(updated.sentencePrompt)) return false;
            } else if (k == "auto_start") {
                if (!c.readBool(updated.autoStart)) return false;
            } else if (k == "active") {
                if (!c.readString(updated.activeModel)) return false;
            } else if (k == "profiles") {
                if (!parseModelArray(c, updated.models)) return false;
            } else {
                if (!c.skipValue()) return false;
            }
            if (c.consume('}')) {
                break;
            }
            if (!c.consume(',')) {
                return false;
            }
        }
    }
    clampConfig(updated);
    cfg = std::move(updated);
    return true;
}

std::string defaultConfigDir() {
#ifdef _WIN32
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &raw)) && raw) {
        std::string base = wideToUtf8Local(raw);
        ::CoTaskMemFree(raw);
        return base + "\\yuntype";
    }
    return "yuntype_config";
#else
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/fcitx5-yuntype";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/fcitx5-yuntype";
    }
    return ".";
#endif
}

std::string defaultDataDir() {
#ifdef _WIN32
    PWSTR raw = nullptr;
    if (SUCCEEDED(::SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &raw)) && raw) {
        std::string base = wideToUtf8Local(raw);
        ::CoTaskMemFree(raw);
        return base + "\\yuntype";
    }
    return "yuntype_data";
#else
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/fcitx5-yuntype";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/share/fcitx5-yuntype";
    }
    return ".";
#endif
}

bool loadConfigFromDir(const std::string &configDir, AppConfig &cfg) {
    const std::string sep =
#ifdef _WIN32
        "\\";
#else
        "/";
#endif
    bool loadedAny = false;
    std::string raw;
    if (readFileUtf8(configDir + sep + "config.json", raw)) {
        loadedAny |= deserializeAppConfigJson(raw, cfg);
    }
    if (readFileUtf8(configDir + sep + "models.json", raw)) {
        loadedAny |= deserializeModelsJson(raw, cfg);
    }
    if (cfg.models.empty()) {
        cfg.models.push_back(ModelProfile{
            "DeepSeek",
            "https://api.deepseek.com/v1",
            "deepseek-chat",
            "",
            "default",
            "",
        });
        cfg.activeModel = "DeepSeek";
    }
    clampConfig(cfg);
    return loadedAny;
}

bool saveConfigToDir(const std::string &configDir, const AppConfig &cfg) {
    const std::string sep =
#ifdef _WIN32
        "\\";
#else
        "/";
#endif
    const bool ok1 = writeFileAtomicUtf8(configDir + sep + "config.json", serializeAppConfigJson(cfg));
    const bool ok2 = writeFileAtomicUtf8(configDir + sep + "models.json", serializeModelsJson(cfg));
    return ok1 && ok2;
}

bool syncAutoStartRegistry(bool enable, const std::string &exePathUtf8) {
#ifdef _WIN32
    HKEY hKey = nullptr;
    if (::RegOpenKeyExW(HKEY_CURRENT_USER,
                        L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                        0, KEY_SET_VALUE, &hKey) != ERROR_SUCCESS) {
        return false;
    }
    LONG res = ERROR_SUCCESS;
    if (enable && !exePathUtf8.empty()) {
        const std::wstring cmd = L"\"" + utf8ToWideLocal(exePathUtf8) + L"\"";
        res = ::RegSetValueExW(
            hKey, L"YunType", 0, REG_SZ,
            reinterpret_cast<const BYTE *>(cmd.c_str()),
            static_cast<DWORD>((cmd.size() + 1) * sizeof(wchar_t)));
    } else {
        res = ::RegDeleteValueW(hKey, L"YunType");
        if (res == ERROR_FILE_NOT_FOUND) {
            res = ERROR_SUCCESS;
        }
    }
    ::RegCloseKey(hKey);
    return res == ERROR_SUCCESS;
#else
    (void)enable;
    (void)exePathUtf8;
    return true;
#endif
}

} // namespace yuntype::win
