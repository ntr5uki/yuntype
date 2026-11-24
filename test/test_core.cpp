#include "llm_client.h"
#include "pinyin_util.h"
#include "sentence_history.h"
#include "utf8_util.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>

#define CHECK(expr)                                                                                \
    do {                                                                                           \
        if (!(expr)) {                                                                             \
            std::cerr << "CHECK FAILED at " << __FILE__ << ":" << __LINE__ << ": " << #expr        \
                      << "\n";                                                                     \
            std::exit(1);                                                                          \
        }                                                                                          \
    } while (0)

using namespace yuntype;

int main() {
    // 1. UTF-8 utilities
    CHECK(utf8Length("") == 0);
    CHECK(utf8Length("abc") == 3);
    CHECK(utf8Length("你好世界") == 4);
    CHECK(utf8Length("a你b😀") == 4);
    {
        const std::string s = "王小明";
        CHECK(utf8SplitChars(s).size() == 3);
        CHECK(utf8PrevChar(s, s.size()) == 6);
        CHECK(utf8NextChar(s, 0) == 3);
        CHECK(utf8ByteOffsetForChars(s, 2) == 6);
    }

    // 2. Pinyin suffix extraction
    CHECK(extractReplaceablePinyinSuffix("nihao") == "nihao");
    CHECK(extractReplaceablePinyinSuffix("今天，ni mingtian jian") == "ni mingtian jian");
    CHECK(extractReplaceablePinyinSuffix("git commit -m \"xiugai bug") == "xiugai bug");
    CHECK(extractReplaceablePinyinSuffix("12345").empty());

    // 3. Cross-platform SentenceHistory with SurroundingSnapshot + ShadowDraft + persistence
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;

        SurroundingSnapshot snap1{"", 0, 0};
        hist.recordCommit(draft, shadow, snap1, "王小明回来",
                          "[\"wangxiaoming huilai\",\"王小明回来\"]", 1, 1000);
        SurroundingSnapshot snap2{"王小明回来", 15, 15};
        hist.update(draft, snap2, 2);
        // Replace "小" (byte 3..6) with "晓"
        SurroundingSnapshot snapSel{"王小明回来", 3, 6};
        hist.recordCommit(draft, shadow, snapSel, "晓", nullptr, 3, 2000);
        SurroundingSnapshot snapAfter{"王晓明回来", 6, 6};
        hist.update(draft, snapAfter, 4);
        CHECK(hist.corrections(1024, 0) == "「王小明回来」→「王晓明回来」");

        // Save & reload with delivery tracking
        namespace fs = std::filesystem;
        const fs::path tmpFile = fs::temp_directory_path() / "yuntype_core_test_history.json";
        CHECK(hist.saveToFile(tmpFile.string(), 4));

        SentenceHistory loaded;
        CHECK(loaded.loadFromFile(tmpFile.string()));
        CHECK(loaded.corrections(1024, 1).empty());
        CHECK(loaded.corrections(1024, 0) == "「王小明回来」→「王晓明回来」");
        std::error_code ec;
        fs::remove(tmpFile, ec);
    }

    // 4. LlmClient request building & candidate parsing
    {
        LlmConfig cfg;
        cfg.baseUrl = "https://api.deepseek.com/v1";
        cfg.model = "deepseek-chat";
        cfg.thinkingMode = "thinking_disabled";
        CHECK(shouldRequestCandidates(cfg, "fanye"));
        CHECK(!shouldRequestCandidates(cfg, std::string(51, 'a')));
        const std::string body = buildSentenceRequestJson(cfg, "fanye", SentenceContext{});
        CHECK(body.find("\"thinking\":{\"type\":\"disabled\"}") != std::string::npos);
        const auto cands = parseCandidates("[\"翻页\",\"泛业\",\"反页\"]", 5);
        CHECK(cands.size() == 3);
        CHECK(cands[0] == "翻页");
    }

    // 5. Models endpoint normalization & JSON model ID parsing
    {
        CHECK(modelsEndpoint("https://api.deepseek.com/v1") == "https://api.deepseek.com/v1/models");
        CHECK(modelsEndpoint("https://api.deepseek.com/v1/") == "https://api.deepseek.com/v1/models");
        CHECK(modelsEndpoint("https://api.deepseek.com/v1/chat/completions") ==
              "https://api.deepseek.com/v1/models");
        CHECK(modelsEndpoint("https://api.deepseek.com/v1/models") ==
              "https://api.deepseek.com/v1/models");

        const auto m1 = parseModelIdsJson(
            R"({"object":"list","data":[{"id":"Qwen-Flash","owned_by":"x"},{"id":"deepseek-chat"},{"id":"deepseek-chat"}]})");
        CHECK(m1.size() == 2);
        CHECK(m1[0] == "deepseek-chat");
        CHECK(m1[1] == "Qwen-Flash");

        const auto m2 = parseModelIdsJson(R"({"models":[{"name":"llama3"},{"id":"ds4"}]})");
        CHECK(m2.size() == 2);
        CHECK(m2[0] == "ds4");
        CHECK(m2[1] == "llama3");

        const auto m3 = parseModelIdsJson(R"(["zeta","alpha","alpha"])");
        CHECK(m3.size() == 2);
        CHECK(m3[0] == "alpha");
        CHECK(m3[1] == "zeta");
    }

    std::cout << "test_core ok\n";
    return 0;
}
