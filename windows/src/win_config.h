#ifndef YUNTYPE_WIN_CONFIG_H
#define YUNTYPE_WIN_CONFIG_H

#include "../../core/llm_client.h"

#include <string>
#include <string_view>
#include <vector>

namespace yuntype::win {

struct ModelProfile {
    std::string name;
    std::string baseUrl;
    std::string model;
    std::string apiKey;
    std::string thinkingMode = "default";
    std::string extraRequestJson;
};

struct AppConfig {
    bool enabled = true;
    std::string triggerKey = "Alt+U";
    bool historyEnabled = true;
    int historyCount = 30;
    int persistIdleSeconds = 30;
    int persistMaxSeconds = 300;
    int shortCandidateThreshold = 50;
    int candidatePageSize = 5;
    std::string sentencePrompt;
    bool autoStart = false;

    std::string activeModel;
    std::vector<ModelProfile> models;

    const ModelProfile *findActiveModel() const;
    yuntype::LlmConfig toLlmConfig(const std::string &keyFileFallback = {}) const;
};

struct TriggerSpec {
    unsigned int modifiers = 0; // Bitmask: 1=Alt, 2=Ctrl, 4=Shift, 8=Win
    unsigned int vk = 'U';
};

// Parse human-readable trigger key like "Alt+U", "Ctrl+Shift+Space", "F9".
TriggerSpec parseTriggerKey(std::string_view spec);

// JSON serialization / deserialization (compatible with Linux models.json schema).
std::string serializeModelsJson(const AppConfig &cfg);
bool deserializeModelsJson(std::string_view json, AppConfig &cfg);

std::string serializeAppConfigJson(const AppConfig &cfg);
bool deserializeAppConfigJson(std::string_view json, AppConfig &cfg);

// Combined state JSON for the local Settings Web UI API.
std::string serializeSettingsStateJson(const AppConfig &cfg, std::size_t historyBytes);
bool applySettingsStateJson(std::string_view json, AppConfig &cfg);

// Filesystem persistence helpers (UTF-8 paths).
std::string defaultConfigDir();
std::string defaultDataDir();
bool loadConfigFromDir(const std::string &configDir, AppConfig &cfg);
bool saveConfigToDir(const std::string &configDir, const AppConfig &cfg);

// Windows HKCU\Software\Microsoft\Windows\CurrentVersion\Run helper (no-op on non-Windows).
bool syncAutoStartRegistry(bool enable, const std::string &exePathUtf8);

} // namespace yuntype::win

#endif
