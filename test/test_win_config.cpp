#include "../windows/src/win_config.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::cerr << "CHECK FAILED at " << __FILE__ << ":" << __LINE__ << ": " << #expr        \
                      << "\n";                                                                     \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

using namespace yuntype::win;

int main() {
    // 1. Trigger hotkey parser
    {
        auto t1 = parseTriggerKey("Alt+U");
        CHECK(t1.modifiers == 1U && t1.vk == 'U');
        auto tCtrlU = parseTriggerKey("Ctrl+U");
        CHECK(tCtrlU.modifiers == 2U && tCtrlU.vk == 'U');
        auto tControlU = parseTriggerKey("Control+U");
        CHECK(tControlU.modifiers == 2U && tControlU.vk == 'U');
        auto tAltS = parseTriggerKey("Alt+S");
        CHECK(tAltS.modifiers == 1U && tAltS.vk == 'S');
        auto tCtrlShiftS = parseTriggerKey("Ctrl+Shift+S");
        CHECK(tCtrlShiftS.modifiers == (2U | 4U) && tCtrlShiftS.vk == 'S');
        auto t2 = parseTriggerKey("Ctrl+Shift+Space");
        CHECK(t2.modifiers == (2U | 4U) && t2.vk == 0x20);
        auto t3 = parseTriggerKey("F9");
        CHECK(t3.modifiers == 0U && t3.vk == 0x78);
    }

    // 2. Save & reload models.json + config.json
    namespace fs = std::filesystem;
    const fs::path tmpDir = fs::temp_directory_path() / "yuntype_win_config_test";
    std::error_code ec;
    fs::remove_all(tmpDir, ec);
    fs::create_directories(tmpDir, ec);

    AppConfig cfg;
    cfg.enabled = true;
    cfg.triggerKey = "Ctrl+Alt+Y";
    cfg.historyEnabled = true;
    cfg.historyCount = 80;
    cfg.shortCandidateThreshold = 35;
    cfg.candidatePageSize = 7;
    cfg.sentencePrompt = "保持简洁口语";
    cfg.autoStart = true;
    cfg.activeModel = "DS4";
    cfg.models = {
        ModelProfile{"DS4", "https://api.deepseek.com/v1", "deepseek-chat", "sk-secret",
                     "thinking_disabled", ""},
        ModelProfile{"Custom", "http://127.0.0.1:8080/v1", "qwen3", "", "custom",
                     "{\"enable_thinking\":false}"},
    };
    CHECK(saveConfigToDir(tmpDir.string(), cfg));

    AppConfig loaded;
    CHECK(loadConfigFromDir(tmpDir.string(), loaded));
    CHECK(loaded.triggerKey == "Ctrl+Alt+Y");
    CHECK(loaded.historyCount == 80);
    CHECK(loaded.shortCandidateThreshold == 35);
    CHECK(loaded.candidatePageSize == 7);
    CHECK(loaded.sentencePrompt == "保持简洁口语");
    CHECK(loaded.autoStart == true);
    CHECK(loaded.activeModel == "DS4");
    CHECK(loaded.models.size() == 2);
    CHECK(loaded.models[0].apiKey == "sk-secret");
    CHECK(loaded.models[0].thinkingMode == "thinking_disabled");
    CHECK(loaded.models[1].extraRequestJson == "{\"enable_thinking\":false}");

    // 3. Bridge into core::LlmConfig
    const auto llmCfg = loaded.toLlmConfig();
    CHECK(llmCfg.isReady());
    CHECK(llmCfg.apiKey == "sk-secret");
    CHECK(llmCfg.thinkingMode == "thinking_disabled");
    CHECK(llmCfg.shortCandidateThreshold == 35);
    CHECK(llmCfg.candidatePageSize == 7);
    CHECK(yuntype::extraRequestFieldsFragment(llmCfg) == ",\"thinking\":{\"type\":\"disabled\"}");

    // 4. Settings UI JSON round-trip
    const std::string stateJson = serializeSettingsStateJson(loaded, 4096);
    CHECK(stateJson.find("\"history_bytes\":4096") != std::string::npos);
    AppConfig applied;
    CHECK(applySettingsStateJson(stateJson, applied));
    CHECK(applied.activeModel == "DS4");
    CHECK(applied.models.size() == 2);
    CHECK(applied.historyCount == 80);

    fs::remove_all(tmpDir, ec);
    std::cout << "test_win_config ok\n";
    return 0;
}
