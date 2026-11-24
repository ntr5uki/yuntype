#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include "llm_client.h"
#include "yuntype.h"
#include "sentence_history.h"

using namespace fcitx5_yuntype;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ << ": " << #cond   \
                      << "\n";                                                                \
            std::abort();                                                                     \
        }                                                                                     \
    } while (0)

namespace {

void testPinyinSuffixExtraction() {
    CHECK(extractReplaceablePinyinSuffix("ni mingtian you shijian ma") ==
          "ni mingtian you shijian ma");
    CHECK(extractReplaceablePinyinSuffix("git commit -m \"xiufu zhengju shuru") ==
          "xiufu zhengju shuru");
    CHECK(extractReplaceablePinyinSuffix("echo 'women mingtian jian") ==
          "women mingtian jian");
    CHECK(extractReplaceablePinyinSuffix("今天天气不错，ni mingtian you shijian ma") ==
          "ni mingtian you shijian ma");
    CHECK(extractReplaceablePinyinSuffix("第一行\nwomen mingtian jian") ==
          "women mingtian jian");
    CHECK(extractReplaceablePinyinSuffix("你好，世界。") == "");
    CHECK(extractReplaceablePinyinSuffix("你好   ") == "");
    CHECK(extractReplaceablePinyinSuffix("12345 + 678") == "");
    CHECK(extractReplaceablePinyinSuffix("") == "");
}

void testJsonAndSseHelpers() {
    CHECK(jsonQuote("a\"b\nc") == "\"a\\\"b\\nc\"");
    auto [u, a] = parseTurnPair("[\"ni hao\",\"你好。\"]");
    CHECK(u == "ni hao");
    CHECK(a == "你好。");

    CHECK(extractSseDelta("data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}") == "你好");
    CHECK(extractSseDelta("data: [DONE]") == "");
    CHECK(stripThinkAndClean("<think>思考中</think>  我们明天见。 ", true) == "我们明天见。");
    CHECK(stripThinkAndClean("<think>还没结束", false) == "");

    // parseCandidates unit tests
    {
        auto cands = parseCandidates("[\"小明\", \"晓明\", \"校名\", \"小明\", \"\"]", 5);
        CHECK(cands.size() == 3);
        CHECK(cands[0] == "小明" && cands[1] == "晓明" && cands[2] == "校名");

        auto capped = parseCandidates("[\"a\",\"b\",\"c\",\"d\"]", 2);
        CHECK(capped.size() == 2 && capped[0] == "a" && capped[1] == "b");

        auto singleArr = parseCandidates("[\n  \"你好\"\n]", 5);
        CHECK(singleArr.size() == 1 && singleArr[0] == "你好");

        auto escaped = parseCandidates("[\"\\\"引用\\\"\", \"\\u4f60\\u597d\"]", 5);
        CHECK(escaped.size() == 2 && escaped[0] == "\"引用\"" && escaped[1] == "你好");

        auto plain = parseCandidates("你明天有时间吗？", 5);
        CHECK(plain.size() == 1 && plain[0] == "你明天有时间吗？");

        auto notJsonArr = parseCandidates("[引用]", 5);
        CHECK(notJsonArr.size() == 1 && notJsonArr[0] == "[引用]");

        auto trailingComma = parseCandidates("[\"你好\", \"拟好\",]", 5);
        CHECK(trailingComma.size() == 2 && trailingComma[0] == "你好" && trailingComma[1] == "拟好");

        CHECK(stripThinkAndClean("```[\"你好\",\"拟好\"]```", true) == "[\"你好\",\"拟好\"]");
    }
}

void testRequestBuilding() {
    LlmConfig cfg;
    cfg.baseUrl = "https://api.deepseek.com/v1";
    cfg.model = "deepseek-chat";
    cfg.prompt = "自定义提示词";
    CHECK(cfg.shortCandidateThreshold == 50);
    CHECK(cfg.historyBudget() == 128 * 1024);

    // Pinyin length threshold check
    CHECK(shouldRequestCandidates(cfg, "fanye"));
    CHECK(shouldRequestCandidates(cfg, "  nihao  "));
    CHECK(shouldRequestCandidates(cfg, std::string(50, 'a')));
    CHECK(!shouldRequestCandidates(cfg, std::string(51, 'a')));
    CHECK(!shouldRequestCandidates(cfg, "   "));

    LlmConfig longModeCfg = cfg;
    longModeCfg.baseUrl = "http://127.0.0.1:8080/v1";
    longModeCfg.shortCandidateThreshold = 0;
    CHECK(longModeCfg.isLoopback());
    CHECK(longModeCfg.historyBudget() == 128 * 1024);
    CHECK(!shouldRequestCandidates(longModeCfg, "fanye"));

    // Prefix cache invariance for full-sentence requests (turns replayed byte-for-byte)
    SentenceContext ctx1;
    ctx1.replayMode = true;
    std::string user1;
    std::string body1 = buildSentenceRequestJson(longModeCfg, "wangxiaoming huilai", ctx1, &user1);
    CHECK(user1 == "wangxiaoming huilai");
    CHECK(body1.find("此前的用户消息和模型回复均为输入法历史") != std::string::npos);
    CHECK(body1.find("候选词生成引擎") == std::string::npos);

    SentenceContext ctx2;
    ctx2.replayMode = true;
    ctx2.turns = "[" + jsonQuote(user1) + "," + jsonQuote("王小明回来") + "]\n";
    ctx2.corrections = "「王小明回来」→「王晓明回来」";
    std::string user2;
    std::string body2 = buildSentenceRequestJson(longModeCfg, "women mingtian jian", ctx2, &user2);
    // Everything up to the end of user1's message in body1 must be an exact byte-for-byte prefix of body2!
    const std::string prefix1 = body1.substr(0, body1.size() - 2); // strip "]}"
    CHECK(body2.starts_with(prefix1 + ",{\"role\":\"assistant\",\"content\":\"王小明回来\"}"));
    // Corrections must precede pinyin in user JSON
    CHECK(user2.find("\"corrections\"") < user2.find("\"pinyin\""));

    // Long sentence (> threshold) also uses full-sentence replay even when threshold > 0
    SentenceContext ctx3;
    ctx3.summary = "项目青禾";
    ctx3.turns = "[\"wangxiaoming\",\"王小明回来\"]\n";
    ctx3.corrections = "「王小明回来」→「王晓明回来」";
    ctx3.replayMode = true;

    std::string longPinyin = "jintian tianqi zhen bucuo women yiqi qu gongyuan sanbu ba"; // 57 chars > 50
    std::string userMsg;
    std::string bodyLong = buildSentenceRequestJson(cfg, longPinyin, ctx3, &userMsg);
    CHECK(bodyLong.find("\"model\":\"deepseek-chat\"") != std::string::npos);
    CHECK(bodyLong.find("自定义提示词") != std::string::npos);
    CHECK(bodyLong.find("项目青禾") != std::string::npos);
    CHECK(bodyLong.find("wangxiaoming") != std::string::npos);
    CHECK(bodyLong.find("王小明回来") != std::string::npos);
    CHECK(bodyLong.find("候选词生成引擎") == std::string::npos);
    CHECK(userMsg.find(longPinyin) != std::string::npos);
    CHECK(userMsg.find("王晓明回来") != std::string::npos);

    // Short pinyin (<= threshold) uses dedicated candidate prompt without replaying plain-text turns or summary
    LlmConfig candCfg = cfg;
    candCfg.shortCandidateThreshold = 50;
    candCfg.candidatePageSize = 5;
    std::string candUser;
    std::string bodyCand = buildSentenceRequestJson(candCfg, "fanye", ctx3, &candUser);
    CHECK(bodyCand.find("候选词生成引擎") != std::string::npos);
    CHECK(bodyCand.find("尽量生成 5 个") != std::string::npos);
    CHECK(bodyCand.find("项目青禾") == std::string::npos);
    CHECK(bodyCand.find("自定义提示词") != std::string::npos);
    CHECK(bodyCand.find("\"role\":\"assistant\"") == std::string::npos);
    CHECK(candUser.find("王晓明回来") != std::string::npos);
    CHECK(candUser.find("fanye") != std::string::npos);

    // Multi-turn "load more candidates" must be an exact byte-for-byte prefix extension of Round 1!
    const std::string round1Resp = "[\"翻页\",\"泛业\",\"反页\",\"番野\",\"翻野\"]";
    const std::string sessionTurns1 = "[" + jsonQuote(candUser) + "," + jsonQuote(round1Resp) + "]\n";
    std::string moreUser1;
    std::string bodyMore1 = buildMoreCandidatesRequestJson(candCfg, sessionTurns1, &moreUser1);
    const std::string candPrefix1 = bodyCand.substr(0, bodyCand.size() - 2); // strip "]}"
    CHECK(bodyMore1.starts_with(candPrefix1 + ",{\"role\":\"assistant\",\"content\":" + jsonQuote(round1Resp) + "}"));
    CHECK(moreUser1.find("继续给出 5 个新的候选") != std::string::npos);

    // Round 3 must also be an exact byte-for-byte prefix extension of Round 2!
    const std::string round2Resp = "[\"繁叶\",\"番页\",\"范爷\",\"翻阅\",\"泛野\"]";
    const std::string sessionTurns2 =
        sessionTurns1 + "[" + jsonQuote(moreUser1) + "," + jsonQuote(round2Resp) + "]\n";
    std::string moreUser2;
    std::string bodyMore2 = buildMoreCandidatesRequestJson(candCfg, sessionTurns2, &moreUser2);
    const std::string candPrefix2 = bodyMore1.substr(0, bodyMore1.size() - 2);
    CHECK(bodyMore2.starts_with(candPrefix2 + ",{\"role\":\"assistant\",\"content\":" + jsonQuote(round2Resp) + "}"));

    // Thinking mode presets & custom extra JSON validation / injection
    {
        LlmConfig tCfg = cfg;
        CHECK(extraRequestFieldsFragment(tCfg).empty());

        tCfg.thinkingMode = "reasoning_effort_none";
        CHECK(extraRequestFieldsFragment(tCfg) == ",\"reasoning_effort\":\"none\"");
        std::string b1 = buildSentenceRequestJson(tCfg, "fanye", SentenceContext{});
        CHECK(b1.find("\"stream\":true,\"reasoning_effort\":\"none\",\"messages\":") != std::string::npos);

        tCfg.thinkingMode = "enable_thinking_false";
        CHECK(extraRequestFieldsFragment(tCfg) == ",\"enable_thinking\":false");
        std::string bMore = buildMoreCandidatesRequestJson(tCfg, sessionTurns1);
        CHECK(bMore.find("\"stream\":true,\"enable_thinking\":false,\"messages\":") != std::string::npos);

        tCfg.thinkingMode = "thinking_disabled";
        CHECK(extraRequestFieldsFragment(tCfg) == ",\"thinking\":{\"type\":\"disabled\"}");

        tCfg.thinkingMode = "chat_template_enable_thinking_false";
        CHECK(extraRequestFieldsFragment(tCfg) == ",\"chat_template_kwargs\":{\"enable_thinking\":false}");

        tCfg.thinkingMode = "think_false";
        CHECK(extraRequestFieldsFragment(tCfg) == ",\"think\":false");

        tCfg.thinkingMode = "custom";
        tCfg.extraRequestJson =
            "{\"reasoning\":{\"enabled\":false},\"temperature\":0.1,\"model\":\"evil\",\"max_tokens\":100}";
        CHECK(extraRequestFieldsFragment(tCfg, false) ==
              ",\"reasoning\":{\"enabled\":false},\"temperature\":0.1,\"max_tokens\":100");
        // Compaction skips duplicate max_tokens
        std::string bComp = buildCompactionRequestJson(tCfg, "batch", "prev");
        CHECK(bComp.find("\"max_tokens\":768,\"reasoning\":{\"enabled\":false},\"temperature\":0.1,\"messages\":") !=
              std::string::npos);
        CHECK(bComp.find("\"max_tokens\":100") == std::string::npos);

        // Invalid custom JSON safely falls back to empty fragment
        tCfg.extraRequestJson = "{invalid json";
        CHECK(extraRequestFieldsFragment(tCfg).empty());
    }
}

class FakeInputContext : public fcitx::InputContext {
public:
    FakeInputContext(fcitx::InputContextManager &manager, bool enableSurrounding = false)
        : fcitx::InputContext(manager, enableSurrounding ? "fake-gui" : "fake-terminal") {
        fcitx::CapabilityFlags flags{fcitx::CapabilityFlag::Preedit};
        if (enableSurrounding) {
            flags |= fcitx::CapabilityFlag::SurroundingText;
        }
        setCapabilityFlags(flags);
        created();
    }
    ~FakeInputContext() override { destroy(); }

    const char *frontend() const override { return "fake"; }

    void syncSurrounding() {
        const auto chars = static_cast<unsigned>(fcitx::utf8::length(terminalBuffer));
        surroundingText().setText(terminalBuffer, chars, chars);
        updateSurroundingText();
    }

    void setSelection(unsigned anchorChar, unsigned cursorChar) {
        surroundingText().setText(terminalBuffer, cursorChar, anchorChar);
        updateSurroundingText();
    }

    void commitStringImpl(const std::string &text) override {
        commits.push_back(text);
        if (capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
            surroundingText().isValid() &&
            surroundingText().cursor() != surroundingText().anchor()) {
            const auto &s = surroundingText();
            const unsigned startChar = std::min(s.cursor(), s.anchor());
            const unsigned endChar = std::max(s.cursor(), s.anchor());
            const auto totalChars = fcitx::utf8::length(terminalBuffer);
            if (endChar <= totalChars) {
                const std::size_t startByte =
                    startChar == 0 ? 0 : fcitx::utf8::ncharByteLength(terminalBuffer.begin(), startChar);
                const std::size_t endByte =
                    endChar == 0 ? 0 : fcitx::utf8::ncharByteLength(terminalBuffer.begin(), endChar);
                terminalBuffer.replace(startByte, endByte - startByte, text);
                syncSurrounding();
                return;
            }
        }
        terminalBuffer += text;
        if (capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
            syncSurrounding();
        }
    }

    void deleteSurroundingTextImpl(int offset, unsigned int size) override {
        surroundingDeletes.emplace_back(offset, size);
        const auto totalChars = fcitx::utf8::length(terminalBuffer);
        if (offset <= 0 && static_cast<std::size_t>(-offset) <= totalChars) {
            const std::size_t startChar = totalChars - static_cast<std::size_t>(-offset);
            const std::size_t endChar = std::min(totalChars, startChar + size);
            const std::size_t startByte =
                startChar == 0 ? 0 : fcitx::utf8::ncharByteLength(terminalBuffer.begin(), startChar);
            const std::size_t endByte =
                endChar == 0 ? 0 : fcitx::utf8::ncharByteLength(terminalBuffer.begin(), endChar);
            terminalBuffer.erase(startByte, endByte - startByte);
            syncSurrounding();
        }
        if (resetOnDeleteSurrounding) {
            reset();
        }
    }

    void forwardKeyImpl(const fcitx::ForwardKeyEvent &key) override {
        if (key.isRelease()) {
            return;
        }
        forwarded.push_back(key.rawKey());
        if (key.rawKey().sym() == FcitxKey_BackSpace && !terminalBuffer.empty()) {
            terminalBuffer.pop_back();
        }
    }

    void updatePreeditImpl() override {
        preedit = inputPanel().clientPreedit().toString();
    }

    bool sendKey(const fcitx::Key &k) {
        fcitx::KeyEvent press(this, k, false);
        bool accepted = keyEvent(press);
        fcitx::KeyEvent release(this, k, true);
        keyEvent(release);
        return accepted;
    }

    void typeAsciiToTerminal(const std::string &s, bool updateSurrounding = true) {
        for (char c : s) {
            fcitx::Key k(static_cast<fcitx::KeySym>(static_cast<unsigned char>(c)));
            if (!sendKey(k)) {
                terminalBuffer.push_back(c);
                if (updateSurrounding &&
                    capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
                    syncSurrounding();
                }
            }
        }
    }

    std::vector<std::string> commits;
    std::vector<fcitx::Key> forwarded;
    std::vector<std::pair<int, unsigned int>> surroundingDeletes;
    bool resetOnDeleteSurrounding = false;
    std::string terminalBuffer;
    std::string preedit;
};

void testHistoryCorrectionAndPersistence() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("fcitx5-llm-hist-" + std::to_string(::getpid()));
    fs::create_directories(tmp);
    const fs::path histFile = tmp / "history.json";

    char arg0[] = "fcitx5";
    char arg1[] = "--disable=all";
    char *argv[] = {arg0, arg1, nullptr};
    fcitx::Instance instance(2, argv);
    FakeInputContext gui(instance.inputContextManager(), true);

    SentenceHistory hist;
    std::optional<SentenceHistory::Draft> draft;

    // 1. LLM commits "王小明回来"
    gui.terminalBuffer.clear();
    gui.syncSurrounding();
    hist.recordCommit(draft, gui, "王小明回来", "[\"wangxiaoming huilai\",\"王小明回来\"]", 1);
    gui.terminalBuffer = "王小明回来";
    gui.syncSurrounding();
    hist.update(draft, gui, 2);

    // 2. User selects "小" (char 1..2) and directly overwrites with "晓" via foreground IME commit!
    gui.setSelection(1, 2);
    hist.recordCommit(draft, gui, "晓", nullptr, 3);
    gui.terminalBuffer = "王晓明回来";
    gui.syncSurrounding();
    hist.update(draft, gui, 4);
    CHECK(hist.corrections(512) == "「王小明回来」→「王晓明回来」");

    // 3. Second sentence + compaction archives first correction & summary
    gui.terminalBuffer = "第二句";
    gui.syncSurrounding();
    hist.recordCommit(draft, gui, "第二句", "[\"dierju\",\"第二句\"]", 5);
    CHECK(hist.needsCompaction(1, 1200));
    std::string batch = hist.beginCompaction(1200);
    CHECK(batch.find("王晓明回来") != std::string::npos);
    hist.finishCompaction("人物：王晓明");
    CHECK(hist.summary() == "人物：王晓明");
    CHECK(hist.corrections(512) == "「王小明回来」→「王晓明回来」");

    // 4. Save to disk (0600) and reload into a fresh SentenceHistory instance
    CHECK(hist.saveToFile(histFile.string()));
    struct stat st{};
    CHECK(::stat(histFile.c_str(), &st) == 0);
    CHECK((st.st_mode & 0777) == 0600);

    SentenceHistory loaded;
    CHECK(loaded.loadFromFile(histFile.string()));
    CHECK(loaded.summary() == "人物：王晓明");
    CHECK(loaded.corrections(512) == "「王小明回来」→「王晓明回来」");
    CHECK(loaded.forRequest(512) == "第二句");
    CHECK(loaded.replayTurns(512).find("dierju") != std::string::npos);

    // 5. Delivery tracking survives restart without snowball re-packing
    {
        SentenceHistory h;
        std::optional<SentenceHistory::Draft> d;
        std::optional<SentenceHistory::ShadowDraft> s;
        FakeInputContext term(instance.inputContextManager(), false);

        // Old IME commit at rev 10, followed by long-sentence turn at rev 11 (sentThrough = 11)
        h.recordCommit(d, s, term, "旧普通输入法句子", nullptr, 10, 1000, 0);
        h.recordCommit(d, s, term, "第一条长句", "[\"diyitiao\",\"第一条长句\"]", 11, 2000, 0);
        // Short-input correction at rev 12, then another long turn at rev 13 (sentThrough = 13)
        CHECK(h.recordShortCommit(d, s, term, "晓明", 12, 3000, 0, "小明"));
        h.recordCommit(d, s, term, "第二条长句", "[\"diertiao\",\"第二条长句\"]", 13, 4000, 0);
        // Unsent trailing IME commit at rev 14 & unsent short-input correction at rev 15
        h.recordCommit(d, s, term, "重启前新打的字", nullptr, 14, 5000, 0);
        CHECK(h.recordShortCommit(d, s, term, "飞书", 15, 6000, 0, "非数"));

        const fs::path trackedFile = tmp / "tracked.json";
        CHECK(h.saveToFile(trackedFile.string(), 13));

        SentenceHistory reloaded;
        CHECK(reloaded.loadFromFile(trackedFile.string()));
        // Simulates loadHistoryFromDisk(): notesSentThrough_ = 1
        CHECK(reloaded.newText(2048, 1) == "重启前新打的字");
        CHECK(reloaded.corrections(2048, 1) == "「非数」→「飞书」");
        // Short-input candidate mode (after == 0) still sees all archived corrections
        const std::string allCorr = reloaded.corrections(2048, 0);
        CHECK(allCorr.find("「小明」→「晓明」") != std::string::npos);
        CHECK(allCorr.find("「非数」→「飞书」") != std::string::npos);
        // Once the next turn commits at rev >= 2, nothing is re-sent
        CHECK(reloaded.newText(2048, 2).empty());
        CHECK(reloaded.corrections(2048, 2).empty());

        // Compaction re-anchors archived corrections without re-sending already-delivered surviving IME commits
        h.recordCommit(d, s, term, "第三条长句", "[\"disantiao\",\"第三条长句\"]", 16, 7000, 0);
        std::uint64_t sentThrough = 16; // rev 14 IME commit and rev 15 correction are now inside turn 16
        CHECK(h.needsCompaction(2, 8192));
        (void)h.beginCompaction(8192);
        const std::uint64_t compactRev = 17;
        h.finishCompaction("摘要内容", compactRev);
        // Surviving rev 14 IME commit was already delivered (14 <= 16), so NOT re-sent!
        CHECK(h.newText(2048, sentThrough).empty());
        // Archived corrections got noteRevision = compactRev (17 > 16), so re-anchored once!
        CHECK(h.corrections(2048, sentThrough).find("「非数」→「飞书」") != std::string::npos);
        CHECK(h.corrections(2048, compactRev).empty());
    }

    // 6. Legacy v1 history.json (without notesTracked) does not re-dump old pre-turn records
    {
        const fs::path legacyFile = tmp / "legacy.json";
        {
            std::ofstream out(legacyFile);
            out << R"({"version":1,"summary":"旧摘要","archivedCorrections":["「老错」→「老对」"],)"
                << R"("records":[)"
                << R"({"text":"旧短句","original":"旧短句","correction":"","turn":""},)"
                << R"({"text":"旧长句","original":"旧长句原","correction":"「旧长句原」→「旧长句」","turn":"[\"jiu\",\"旧长句\"]"},)"
                << R"({"text":"新普通输入","original":"新普通输入","correction":"","turn":""})"
                << R"(]})";
        }
        SentenceHistory legacy;
        CHECK(legacy.loadFromFile(legacyFile.string()));
        CHECK(legacy.newText(2048, 1) == "新普通输入");
        CHECK(legacy.corrections(2048, 1).empty());
        CHECK(legacy.corrections(2048, 0).find("「老错」→「老对」") != std::string::npos);
    }

    // 7. Archived corrections are capped at 100 entries
    {
        SentenceHistory capped;
        std::optional<SentenceHistory::Draft> d;
        std::optional<SentenceHistory::ShadowDraft> s;
        FakeInputContext term(instance.inputContextManager(), false);
        for (int i = 0; i < 115; ++i) {
            CHECK(capped.recordShortCommit(d, s, term, "新词" + std::to_string(i),
                                           static_cast<std::uint64_t>(i + 1), 1000, 0,
                                           "旧词" + std::to_string(i)));
        }
        const std::string all = capped.corrections(16384, 0);
        CHECK(all.find("「旧词0」→「新词0」") == std::string::npos);
        CHECK(all.find("「旧词14」→「新词14」") == std::string::npos);
        CHECK(all.find("「旧词15」→「新词15」") != std::string::npos);
        CHECK(all.find("「旧词114」→「新词114」") != std::string::npos);
    }

    // 8. Corrupt/missing files fail gracefully without crashing
    const fs::path badFile = tmp / "bad.json";
    {
        std::ofstream out(badFile);
        out << "{not valid json";
    }
    SentenceHistory untouched;
    CHECK(!untouched.loadFromFile(badFile.string()));
    CHECK(!untouched.loadFromFile((tmp / "missing.json").string()));

    fs::remove_all(tmp);
}

void testTerminalShadowBuffer() {
    char arg0[] = "fcitx5";
    char arg1[] = "--disable=all";
    char *argv[] = {arg0, arg1, nullptr};
    fcitx::Instance instance(2, argv);
    FakeInputContext term(instance.inputContextManager(), false); // NO SurroundingText

    std::uint64_t rev = 0;
    std::uint64_t now = 1000000;
    constexpr std::uint64_t kTimeout = 5000000; // 5 s

    // 1. BackSpace from end + multi-commit foreground IME replacement
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;

        hist.recordCommit(draft, shadow, term, "王小明回来",
                          "[\"wangxiaoming huilai\",\"王小明回来\"]", ++rev, now, kTimeout);
        CHECK(!draft.has_value());
        CHECK(shadow.has_value());

        for (int i = 0; i < 4; ++i) {
            hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        }
        CHECK(shadow && shadow->text == "王");
        hist.recordCommit(draft, shadow, term, "晓", nullptr, ++rev, now += 1000, kTimeout);
        hist.recordCommit(draft, shadow, term, "明回来", nullptr, ++rev, now += 1000, kTimeout);
        CHECK(hist.corrections(512) == "「王小明回来」→「王晓明回来」");
        CHECK(hist.forRequest(512) == "王晓明回来");
    }

    // 2. Left arrow + BackSpace in the middle of a sentence
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;

        hist.recordCommit(draft, shadow, term, "王小明回来",
                          "[\"wangxiaoming huilai\",\"王小明回来\"]", ++rev, now, kTimeout);
        for (int i = 0; i < 3; ++i) {
            hist.shadowMoveLeft(shadow, now += 1000, kTimeout);
        }
        hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        CHECK(shadow && shadow->text == "王明回来");
        hist.recordCommit(draft, shadow, term, "晓", nullptr, ++rev, now += 1000, kTimeout);
        CHECK(hist.corrections(512) == "「王小明回来」→「王晓明回来」");
        CHECK(hist.forRequest(512) == "王晓明回来");
    }

    // 3. Left arrow + Delete key in the middle
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;

        hist.recordCommit(draft, shadow, term, "王小明回来",
                          "[\"wangxiaoming huilai\",\"王小明回来\"]", ++rev, now, kTimeout);
        for (int i = 0; i < 4; ++i) {
            hist.shadowMoveLeft(shadow, now += 1000, kTimeout);
        }
        hist.shadowDelete(shadow, ++rev, now += 1000, kTimeout);
        CHECK(shadow && shadow->text == "王明回来");
        hist.recordCommit(draft, shadow, term, "晓", nullptr, ++rev, now += 1000, kTimeout);
        CHECK(hist.corrections(512) == "「王小明回来」→「王晓明回来」");
    }

    // 4. Deleting an entire short word ("小明") and re-entering ("晓明")
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;

        hist.recordCommit(draft, shadow, term, "小明", "[\"xiaoming\",\"小明\"]", ++rev, now, kTimeout);
        hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        CHECK(shadow && shadow->text.empty());
        // Record keeps last non-empty text until replacement arrives
        CHECK(hist.forRequest(512) == "小明");
        hist.recordCommit(draft, shadow, term, "晓明", nullptr, ++rev, now += 1000, kTimeout);
        CHECK(hist.corrections(512) == "「小明」→「晓明」");
        // BackSpace past start resets shadow draft
        hist.shadowMoveLeft(shadow, now += 1000, kTimeout);
        hist.shadowMoveLeft(shadow, now += 1000, kTimeout);
        hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        CHECK(!shadow.has_value());
    }

    // 5. Replacing misrecognized Chinese with English ASCII ("吉特" -> "git")
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;

        hist.recordCommit(draft, shadow, term, "请用吉特提交",
                          "[\"qing yong git tijiao\",\"请用吉特提交\"]", ++rev, now, kTimeout);
        hist.shadowMoveLeft(shadow, now += 1000, kTimeout);
        hist.shadowMoveLeft(shadow, now += 1000, kTimeout);
        hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        hist.shadowInsertAscii(shadow, "git", ++rev, now += 1000, kTimeout);
        CHECK(hist.corrections(512) == "「请用吉特提交」→「请用git提交」");
    }

    // 6. BackSpace + ASCII pinyin + Alt+U erase + LLM replacement merge, then clean next sentence
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;

        hist.recordCommit(draft, shadow, term, "王小明回来",
                          "[\"wangxiaoming huilai\",\"王小明回来\"]", ++rev, now, kTimeout);
        for (int i = 0; i < 4; ++i) {
            hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        }
        hist.shadowInsertAscii(shadow, "xiaoming huilai", ++rev, now += 1000, kTimeout);
        CHECK(shadow && shadow->text == "王xiaoming huilai");
        // Alt+U erases the pinyin suffix BEFORE sending LLM request -> no pinyin leak in corrections!
        CHECK(hist.shadowEraseSuffixBeforeCursor(shadow, "xiaoming huilai", ++rev, now += 1000, kTimeout));
        CHECK(shadow && shadow->text == "王");
        CHECK(hist.corrections(512).empty());
        // LLM returns replacement "晓明回来" -> merged into original record!
        hist.recordCommit(draft, shadow, term, "晓明回来",
                          "[\"xiaoming huilai\",\"晓明回来\"]", ++rev, now += 1000, kTimeout);
        CHECK(hist.corrections(512) == "「王小明回来」→「王晓明回来」");
        CHECK(hist.forRequest(512) == "王晓明回来");
        // Typing new ASCII next without BackSpace closes shadow draft cleanly (no pollution!)
        CHECK(!hist.shadowInsertAscii(shadow, "w", ++rev, now += 1000, kTimeout));
        CHECK(!shadow.has_value());
        CHECK(hist.corrections(512) == "「王小明回来」→「王晓明回来」");
    }

    // 7. Timeout expiry & sentence punctuation boundary
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;

        hist.recordCommit(draft, shadow, term, "王小明回来。",
                          "[\"wangxiaoming huilai\",\"王小明回来。\"]", ++rev, now, kTimeout);
        for (int i = 0; i < 5; ++i) {
            hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        }
        hist.recordCommit(draft, shadow, term, "晓明回来。", nullptr, ++rev, now += 1000, kTimeout);
        CHECK(hist.corrections(512) == "「王小明回来。」→「王晓明回来。」");
        // Next sentence after terminal punctuation starts a brand-new record
        hist.recordCommit(draft, shadow, term, "明天见", nullptr, ++rev, now += 1000, kTimeout);
        CHECK(hist.corrections(512) == "「王小明回来。」→「王晓明回来。」");
        CHECK(hist.forRequest(512) == "王晓明回来。\n明天见");

        // After idle timeout, BackSpace does NOT edit old record
        now += kTimeout + 1000;
        CHECK(!hist.shadowBackspace(shadow, ++rev, now, kTimeout));
        CHECK(!shadow.has_value());
    }
}

void testShortCommitCorrectionOnly() {
    char arg0[] = "fcitx5";
    char arg1[] = "--disable=all";
    char *argv[] = {arg0, arg1, nullptr};
    fcitx::Instance instance(2, argv);
    FakeInputContext gui(instance.inputContextManager(), true);
    FakeInputContext term(instance.inputContextManager(), false);

    std::uint64_t rev = 0;
    std::uint64_t now = 1000000;
    constexpr std::uint64_t kTimeout = 5000000;

    // 1. Standalone short commit picking 1st candidate -> untouched (no record, no correction)
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;
        CHECK(!hist.recordShortCommit(draft, shadow, term, "你好", ++rev, now, kTimeout, "你好"));
        CHECK(hist.empty());
        CHECK(hist.forRequest(512).empty());
        CHECK(hist.corrections(512, 0).empty());
    }

    // 2. Standalone short commit picking 2nd candidate -> NO sentence record, ONLY correction pool
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;
        const std::uint64_t beforeRev = rev;
        CHECK(hist.recordShortCommit(draft, shadow, term, "晓明", ++rev, now, kTimeout, "小明"));
        CHECK(hist.forRequest(512).empty()); // no sentence record pushed!
        CHECK(hist.replayTurns(512).empty());
        CHECK(hist.corrections(512, 0) == "「小明」→「晓明」");
        CHECK(hist.corrections(512, beforeRev) == "「小明」→「晓明」"); // incremental long sentence sees it!
        CHECK(hist.corrections(512, rev).empty()); // already sent once -> not duplicated!
    }

    // 3. GUI in-place replacement on previous long sentence merges into previous record!
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;
        gui.terminalBuffer.clear();
        gui.syncSurrounding();
        hist.recordCommit(draft, shadow, gui, "王小明回来",
                          "[\"wangxiaoming huilai\",\"王小明回来\"]", ++rev, now, kTimeout);
        gui.terminalBuffer = "王小明回来";
        gui.syncSurrounding();
        hist.update(draft, gui, ++rev);

        // User selects "小" (1..2) and replaces via short-input Alt+U candidate "晓"
        gui.setSelection(1, 2);
        CHECK(hist.recordShortCommit(draft, shadow, gui, "晓", ++rev, now, kTimeout, "小"));
        CHECK(hist.forRequest(512) == "王晓明回来");
        CHECK(hist.corrections(512, 0) == "「王小明回来」→「王晓明回来」");
    }

    // 4. Terminal BackSpace + short-input Alt+U replacement merges into previous shadow record!
    {
        SentenceHistory hist;
        std::optional<SentenceHistory::Draft> draft;
        std::optional<SentenceHistory::ShadowDraft> shadow;
        hist.recordCommit(draft, shadow, term, "王小明回来",
                          "[\"wangxiaoming huilai\",\"王小明回来\"]", ++rev, now, kTimeout);
        for (int i = 0; i < 4; ++i) {
            hist.shadowBackspace(shadow, ++rev, now += 1000, kTimeout);
        }
        CHECK(hist.recordShortCommit(draft, shadow, term, "晓明回来", ++rev, now += 1000, kTimeout, "小明回来"));
        CHECK(hist.forRequest(512) == "王晓明回来");
        CHECK(hist.corrections(512, 0) == "「王小明回来」→「王晓明回来」");
    }
}

void testEndToEndFcitx5Module() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("fcitx5-llm-test-" + std::to_string(::getpid()));
    fs::create_directories(tmp / "config" / "fcitx5" / "conf");

    // Start a tiny Python mock OpenAI SSE server
    const fs::path mockScript = tmp / "mock_server.py";
    const fs::path portFile = tmp / "port";
    {
        std::ofstream out(mockScript);
        out << R"PY(
import json, os, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPLIES = {
    "ni mingtian you shijian ma": "你明天有时间吗？",
    "xiufu zhengju shuru": "修复整句输入",
    "women mingtian jian": "我们明天见。",
    "wangxiaoming huilai": "王小明回来",
    "xiaoming": json.dumps(
        ["小明", "晓明", "校名", "肖明", "孝明"],
        ensure_ascii=False,
    ),
    "xiaoming10": json.dumps(
        ["小明", "晓明", "校名", "肖明", "孝明", "小名", "消明", "晓铭", "萧明", "笑鸣"],
        ensure_ascii=False,
    ),
    "nihao_single": json.dumps(["你好"], ensure_ascii=False),
}

class H(BaseHTTPRequestHandler):
    seen_history_sys = None
    seen_cand_sys = None
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n))
        msgs = body["messages"]
        src = msgs[-1]["content"]
        reply = REPLIES.get(src)
        if src == "wangxiaoming huilai":
            H.seen_history_sys = msgs[0]["content"]
        if src == "xiaoming" and len(msgs) == 2:
            H.seen_cand_sys = msgs[0]["content"]
        if reply is None and "继续给出" in src:
            # Verify multi-turn prefix cache invariance on load-more rounds!
            if len(msgs) == 4 and msgs[0]["content"] == H.seen_cand_sys:
                # Include a duplicate "小明" to verify client-side deduplication
                reply = json.dumps(["小明", "小名", "消明", "晓铭", "萧明", "笑鸣"], ensure_ascii=False)
            elif len(msgs) == 6 and msgs[0]["content"] == H.seen_cand_sys:
                reply = json.dumps(["小鸣", "晓鸣"], ensure_ascii=False)
            else:
                reply = json.dumps(["错乱" + str(len(msgs))], ensure_ascii=False)
        if reply is None:
            try:
                obj = json.loads(src)
                if obj.get("pinyin") == "querending":
                    # Verify loopback proxy still receives exact multi-turn prefix replay!
                    prefix_ok = (
                        len(msgs) == 4
                        and msgs[0]["content"] == H.seen_history_sys
                        and msgs[1] == {"role": "user", "content": "wangxiaoming huilai"}
                        and msgs[2] == {"role": "assistant", "content": "王小明回来"}
                    )
                    corr_ok = "「王小明回来」→「王晓明回来」" in obj.get("corrections", "")
                    if prefix_ok and corr_ok:
                        reply = "确认王晓明"
                    else:
                        reply = f"失配:prefix={prefix_ok},corr={corr_ok},len={len(msgs)}"
                elif obj.get("pinyin") == "querending_cand":
                    corr_ok = "「小明」→「晓明」" in obj.get("corrections", "")
                    no_history_ok = "history" not in obj
                    no_turns_ok = len(msgs) == 2 and "候选词生成引擎" in msgs[0]["content"]
                    reply = "学到晓明" if (corr_ok and no_history_ok and no_turns_ok) else f"未学到:corr={corr_ok},no_hist={no_history_ok},no_turns={no_turns_ok},len={len(msgs)}"
                elif obj.get("pinyin") == "xiaoming huilai":
                    # Verify temporary ASCII pinyin did NOT leak into corrections before Alt+U!
                    if "xiaoming" in obj.get("corrections", ""):
                        reply = "泄露拼音:" + obj.get("corrections", "")
                    else:
                        reply = "晓明回来"
                else:
                    reply = REPLIES.get(obj.get("pinyin", ""), "未知:" + src)
            except Exception:
                reply = "未知:" + src
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for ch in reply:
            chunk = {"choices": [{"delta": {"content": ch}}]}
            self.wfile.write(f"data: {json.dumps(chunk, ensure_ascii=False)}\n\n".encode())
            self.wfile.flush()
            time.sleep(0.01)
        self.wfile.write(b"data: [DONE]\n\n")
    def log_message(self, *args):
        pass

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
with open(sys.argv[1], "w") as f:
    f.write(f"{srv.server_address[1]} {os.getpid()}")
srv.serve_forever()
)PY";
    }

    const std::string cmd = "python3 \"" + mockScript.string() + "\" \"" + portFile.string() + "\" &";
    CHECK(std::system(cmd.c_str()) == 0);
    std::string port;
    pid_t serverPid = -1;
    for (int i = 0; i < 100; ++i) {
        if (fs::exists(portFile)) {
            std::ifstream in(portFile);
            in >> port >> serverPid;
            if (!port.empty() && serverPid > 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(!port.empty() && serverPid > 0);

    const std::string baseUrl = "http://127.0.0.1:" + port + "/v1";
    const fs::path historyFile = tmp / "data" / "fcitx5-yuntype" / "history.json";
    ::setenv("XDG_CONFIG_HOME", (tmp / "config").c_str(), 1);
    ::setenv("FCITX5_YUNTYPE_URL", baseUrl.c_str(), 1);
    ::setenv("FCITX5_YUNTYPE_MODEL", "mock-llm", 1);
    ::setenv("FCITX5_YUNTYPE_HISTORY_FILE", historyFile.c_str(), 1);
    ::setenv("FCITX5_YUNTYPE_PERSIST_IDLE_MS", "120", 1);

    char arg0[] = "fcitx5";
    char arg1[] = "--disable=all";
    char *argv[] = {arg0, arg1, nullptr};
    fcitx::Instance instance(2, argv);

    std::unique_ptr<YunTypeAddon> addon;
    std::unique_ptr<FakeInputContext> ic;
    std::unique_ptr<FakeInputContext> guiIc;

    int stage = 0;
    int ticks = 0;
    auto timer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 1000, 1000,
        [&](fcitx::EventSourceTime *src, uint64_t) {
            if (++ticks >= 1000) {
                std::cerr << "TIMEOUT at stage=" << stage
                          << " ic->commits=" << ic->commits.size()
                          << " guiIc->commits=" << (guiIc ? guiIc->commits.size() : 0)
                          << " hasCands=" << (ic && ic->inputPanel().candidateList() && !ic->inputPanel().candidateList()->empty())
                          << " preedit=" << (ic ? ic->preedit : "")
                          << " auxDown=" << (ic ? ic->inputPanel().auxDown().toString() : "")
                          << "\n";
                CHECK(false);
            }
            if (stage == 0) {
                addon = std::make_unique<YunTypeAddon>(&instance);
                ic = std::make_unique<FakeInputContext>(instance.inputContextManager(), false);
                ic->focusIn();
                // Case 1: Type pinyin directly into terminal, then press Alt+u ONCE -> auto-backspaces and replaces!
                ic->typeAsciiToTerminal("ni mingtian you shijian ma");
                CHECK(ic->terminalBuffer == "ni mingtian you shijian ma");
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_u, fcitx::KeyState::Alt)));
                CHECK(ic->forwarded.size() == 26);
                CHECK(ic->surroundingDeletes.empty());
                CHECK(ic->terminalBuffer.empty());
                stage = 1;
            } else if (stage == 1 && ic->commits.size() >= 1) {
                CHECK(ic->commits.back() == "你明天有时间吗？");
                CHECK(ic->terminalBuffer == "你明天有时间吗？");

                // Case 2: Type shell command with opening quote `git commit -m "xiufu zhengju shuru`, then press Alt+U ONCE
                // -> only backspaces `xiufu zhengju shuru` after the opening quote!
                ic->terminalBuffer.clear();
                ic->forwarded.clear();
                ic->typeAsciiToTerminal("git commit -m \"xiufu zhengju shuru");
                CHECK(ic->terminalBuffer == "git commit -m \"xiufu zhengju shuru");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                CHECK(ic->forwarded.size() == std::string("xiufu zhengju shuru").size());
                CHECK(ic->terminalBuffer == "git commit -m \"");
                stage = 2;
            } else if (stage == 2 && ic->commits.size() >= 2) {
                CHECK(ic->commits.back() == "修复整句输入");
                CHECK(ic->terminalBuffer == "git commit -m \"修复整句输入");

                // Case 3: Press Alt+U first when nothing replaceable is at cursor -> enters draft mode, type & press Return
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                ic->typeAsciiToTerminal("women mingtian jian");
                CHECK(ic->preedit == "women mingtian jian");
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_Return)));
                stage = 3;
            } else if (stage == 3 && ic->commits.size() >= 3) {
                CHECK(ic->commits.back() == "我们明天见。");

                // Case 4: Non-terminal GUI app (SurroundingText enabled, NO selection, appAscii_ wiped by Reset)
                // -> reads preceding pinyin from SurroundingText, deletes via deleteSurroundingText (NOT forwardKey!),
                //    survives client-side reset triggered by deleteSurroundingText, and replaces cleanly!
                ic->focusOut();
                guiIc = std::make_unique<FakeInputContext>(instance.inputContextManager(), true);
                guiIc->resetOnDeleteSurrounding = true;
                guiIc->focusIn();
                guiIc->terminalBuffer = "刚才说的，ni mingtian you shijian ma";
                guiIc->syncSurrounding();
                guiIc->reset(); // simulates browser/Electron resetting IC before Alt+U
                CHECK(guiIc->sendKey(fcitx::Key("Alt+U")));
                CHECK(guiIc->forwarded.empty());
                CHECK(guiIc->surroundingDeletes.size() == 1);
                CHECK(guiIc->surroundingDeletes.back() == std::make_pair(-26, 26u));
                CHECK(guiIc->terminalBuffer == "刚才说的，");
                stage = 4;
            } else if (stage == 4 && guiIc->commits.size() >= 1) {
                CHECK(guiIc->commits.back() == "你明天有时间吗？");
                CHECK(guiIc->terminalBuffer == "刚才说的，你明天有时间吗？");

                // Case 5: GUI app where SurroundingText is 1 char behind fast typing
                guiIc->surroundingDeletes.clear();
                guiIc->forwarded.clear();
                guiIc->terminalBuffer.clear();
                guiIc->syncSurrounding();
                guiIc->reset();
                guiIc->typeAsciiToTerminal("ni mingtian you shijian m", true);
                guiIc->typeAsciiToTerminal("a", false); // simulate async SurroundingText lag
                CHECK(guiIc->terminalBuffer == "ni mingtian you shijian ma");
                CHECK(guiIc->sendKey(fcitx::Key("Alt+U")));
                CHECK(guiIc->forwarded.empty());
                CHECK(guiIc->surroundingDeletes.size() == 1);
                CHECK(guiIc->surroundingDeletes.back() == std::make_pair(-26, 26u));
                CHECK(guiIc->terminalBuffer.empty());
                stage = 5;
            } else if (stage == 5 && guiIc->commits.size() >= 2) {
                CHECK(guiIc->commits.back() == "你明天有时间吗？");
                CHECK(guiIc->terminalBuffer == "你明天有时间吗？");

                // Case 6: GUI app cursor right after Chinese text -> does NOT delete Chinese, enters draft mode
                guiIc->surroundingDeletes.clear();
                guiIc->forwarded.clear();
                CHECK(guiIc->sendKey(fcitx::Key("Alt+U")));
                CHECK(guiIc->surroundingDeletes.empty());
                CHECK(guiIc->forwarded.empty());
                CHECK(guiIc->terminalBuffer == "你明天有时间吗？");
                CHECK(guiIc->sendKey(fcitx::Key(FcitxKey_Escape)));

                // Case 7: Enable HistoryEnabled, convert "wangxiaoming huilai" -> "王小明回来",
                // select "小" and overwrite with "晓", verify debounced persistence -> restart addon ->
                // verify persisted correction is sent on next conversion, then clearHistory deletes file!
                {
                    std::ofstream cfgOut(tmp / "config" / "fcitx5" / "conf" / "yuntype.conf");
                    cfgOut << "HistoryEnabled=True\nHistoryCount=3\nPersistIdleSeconds=12\nPersistMaxSeconds=180\nShortCandidateThreshold=0\n";
                }
                addon->reloadConfig();
                {
                    fcitx::RawConfig raw;
                    addon->getConfig()->save(raw);
                    CHECK(raw.valueByPath("PersistIdleSeconds") &&
                          *raw.valueByPath("PersistIdleSeconds") == "12");
                    CHECK(raw.valueByPath("PersistMaxSeconds") &&
                          *raw.valueByPath("PersistMaxSeconds") == "180");
                }
                guiIc->terminalBuffer = "wangxiaoming huilai";
                guiIc->syncSurrounding();
                guiIc->reset();
                CHECK(guiIc->sendKey(fcitx::Key("Alt+U")));
                stage = 6;
            } else if (stage == 6 && guiIc->commits.size() >= 3) {
                CHECK(guiIc->commits.back() == "王小明回来");
                CHECK(guiIc->terminalBuffer == "王小明回来");
                // Must NOT have flushed synchronously right on commit (debounced!)
                CHECK(!fs::exists(historyFile));

                // User selects "小" (index 1..2) and directly overwrites with "晓" via IME commit
                guiIc->setSelection(1, 2);
                guiIc->commitString("晓");
                CHECK(guiIc->terminalBuffer == "王晓明回来");
                stage = 7;
            } else if (stage == 7 && fs::exists(historyFile)) {
                // Idle debounce timer fired and wrote historyFile with 0600 permissions
                struct stat st{};
                CHECK(::stat(historyFile.c_str(), &st) == 0);
                CHECK((st.st_mode & 0777) == 0600);

                // Simulate fcitx5 restart by destroying and recreating the addon!
                addon.reset();
                addon = std::make_unique<YunTypeAddon>(&instance);
                guiIc->terminalBuffer = "querending";
                guiIc->syncSurrounding();
                guiIc->reset();
                CHECK(guiIc->sendKey(fcitx::Key("Alt+U")));
                stage = 8;
            } else if (stage == 8 && guiIc->commits.size() >= 5) {
                CHECK(guiIc->commits.back() == "确认王晓明");
                addon->clearHistory();
                CHECK(!fs::exists(historyFile));

                // Case 8: Terminal (NO SurroundingText) shadow buffer correction via Left + BackSpace + IME commit!
                guiIc->focusOut();
                ic->focusIn();
                ic->commits.clear();
                ic->terminalBuffer.clear();
                ic->typeAsciiToTerminal("wangxiaoming huilai");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 9;
            } else if (stage == 9 && ic->commits.size() >= 1) {
                CHECK(ic->commits.back() == "王小明回来");
                // Move cursor left 3 chars (past 来, 明, 小), BackSpace "小", commit "晓" via foreground IME
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_Left)));
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_Left)));
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_Left)));
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_BackSpace)));
                ic->commitString("晓");
                // Press Return to end the line, then trigger next sentence to verify correction is sent!
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_Return)));
                ic->typeAsciiToTerminal("querending");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 10;
            } else if (stage == 10 && ic->commits.size() >= 3) {
                CHECK(ic->commits.back() == "确认王晓明");

                // Case 9: Terminal shadow buffer correction via BackSpace + ASCII pinyin + Alt+U self-replacement!
                addon->clearHistory();
                ic->commits.clear();
                ic->terminalBuffer.clear();
                ic->typeAsciiToTerminal("wangxiaoming huilai");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 11;
            } else if (stage == 11 && ic->commits.size() >= 1) {
                CHECK(ic->commits.back() == "王小明回来");
                for (int i = 0; i < 4; ++i) {
                    CHECK(!ic->sendKey(fcitx::Key(FcitxKey_BackSpace)));
                }
                ic->typeAsciiToTerminal("xiaoming huilai");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 12;
            } else if (stage == 12 && ic->commits.size() >= 2) {
                CHECK(ic->commits.back() == "晓明回来");
                // Now type next sentence directly without Enter -> should NOT pollute correction!
                ic->typeAsciiToTerminal("querending");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 13;
            } else if (stage == 13 && ic->commits.size() >= 3) {
                CHECK(ic->commits.back() == "确认王晓明");
                addon->clearHistory();
                CHECK(!fs::exists(historyFile));

                // Case 10: Short-input candidate list with on-demand multi-turn load-more
                // ('=' on last page & '+' on any page), paging (-/=, arrows), digit selection,
                // and automatic implicit correction recording!
                {
                    std::ofstream cfgOut(tmp / "config" / "fcitx5" / "conf" / "yuntype.conf");
                    cfgOut << "HistoryEnabled=True\nHistoryCount=5\n"
                              "ShortCandidateThreshold=50\nCandidatePageSize=5\n";
                }
                addon->reloadConfig();
                ic->commits.clear();
                ic->terminalBuffer.clear();
                ic->typeAsciiToTerminal("xiaoming");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 14;
            } else if (stage == 14 && ic->inputPanel().candidateList() &&
                       !ic->inputPanel().candidateList()->empty()) {
                auto cl = ic->inputPanel().candidateList();
                auto *common = dynamic_cast<fcitx::CommonCandidateList *>(cl.get());
                CHECK(common != nullptr);
                CHECK(common->totalSize() == 5);
                CHECK(common->size() == 5);
                CHECK(common->totalPages() == 1);
                CHECK(common->currentPage() == 0);
                CHECK(common->cursorIndex() == 0);
                CHECK(common->candidate(0).text().toString() == "小明");

                // '=' on last page triggers multi-turn Load More (Round 2)!
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_equal)));
                CHECK(ic->preedit == "xiaoming…");
                CHECK(ic->inputPanel().auxDown().toString().empty());
                stage = 141;
            } else if (stage == 141 && ic->inputPanel().candidateList() &&
                       ic->inputPanel().candidateList()->toBulk() &&
                       ic->inputPanel().candidateList()->toBulk()->totalSize() > 5) {
                auto cl = ic->inputPanel().candidateList();
                auto *common = dynamic_cast<fcitx::CommonCandidateList *>(cl.get());
                CHECK(common != nullptr);
                CHECK(ic->preedit == "xiaoming");
                CHECK(ic->inputPanel().auxDown().toString().empty());
                // Round 2 returned ["小明", "小名", "消明", "晓铭", "萧明", "笑鸣"] -> capped to 5, "小明" deduped -> 4 new items ("小名","消明","晓铭","萧明") => total 9
                CHECK(common->totalSize() == 9);
                CHECK(common->totalPages() == 2);
                CHECK(common->currentPage() == 1);
                CHECK(common->cursorIndex() == 0);
                CHECK(common->candidate(0).text().toString() == "小名");

                // '+' triggers Load More (Round 3) from any page!
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_plus, fcitx::KeyState::Shift)));
                CHECK(ic->preedit == "xiaoming…");
                CHECK(ic->inputPanel().auxDown().toString().empty());
                stage = 142;
            } else if (stage == 142 && ic->inputPanel().candidateList() &&
                       ic->inputPanel().candidateList()->toBulk() &&
                       ic->inputPanel().candidateList()->toBulk()->totalSize() > 9) {
                auto cl = ic->inputPanel().candidateList();
                auto *common = dynamic_cast<fcitx::CommonCandidateList *>(cl.get());
                CHECK(common != nullptr);
                // Round 3 appended ["小鸣", "晓鸣"] -> total 11 (3 pages: 5, 5, 1)
                CHECK(common->totalSize() == 11);
                CHECK(common->totalPages() == 3);
                CHECK(common->currentPage() == 1); // index 9 is 5th slot on page 1!
                CHECK(common->cursorIndex() == 4);
                CHECK(common->candidate(4).text().toString() == "小鸣");

                // '=' flips to page 2 (last page has 1 item: "晓鸣")
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_equal)));
                CHECK(common->currentPage() == 2);
                CHECK(common->size() == 1);
                CHECK(common->candidate(0).text().toString() == "晓鸣");

                // Pressing '3' on a 1-item page (within pageSize=5) is swallowed cleanly
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_3)));
                CHECK(ic->commits.empty());

                // '-' flips back to page 1
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_minus)));
                CHECK(common->currentPage() == 1);
                CHECK(common->cursorIndex() == 0);

                // Up arrow from index 0 on page 1 crosses back to page 0 last item (index 4: "孝明")
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_Up)));
                CHECK(common->currentPage() == 0);
                CHECK(common->cursorIndex() == 4);
                CHECK(common->candidate(4).text().toString() == "孝明");

                // Down arrow crosses forward to page 1 index 0
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_Down)));
                CHECK(common->currentPage() == 1);
                CHECK(common->cursorIndex() == 0);

                // End jumps to very last candidate (page 2, index 0: "晓鸣"), Home back to page 0 index 0
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_End)));
                CHECK(common->currentPage() == 2);
                CHECK(common->cursorIndex() == 0);
                CHECK(common->candidate(0).text().toString() == "晓鸣");
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_Home)));
                CHECK(common->currentPage() == 0);
                CHECK(common->cursorIndex() == 0);

                // Press '2' to select "晓明"
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_2)));
                CHECK(ic->commits.size() == 1);
                CHECK(ic->commits.back() == "晓明");
                CHECK(!ic->inputPanel().candidateList() || ic->inputPanel().candidateList()->empty());

                // Verify implicit correction 「小明」→「晓明」 is sent on the next request!
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_Return)));
                ic->typeAsciiToTerminal("querending_cand");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 15;
            } else if (stage == 15 && ic->commits.size() >= 2) {
                CHECK(ic->commits.back() == "学到晓明");

                // Case 11: Direct continuation typing auto-commits highlighted candidate & passes key through!
                ic->commits.clear();
                ic->terminalBuffer.clear();
                ic->typeAsciiToTerminal("xiaoming");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 16;
            } else if (stage == 16 && ic->inputPanel().candidateList() &&
                       !ic->inputPanel().candidateList()->empty()) {
                // Standalone Shift_L must NOT prematurely commit candidate
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_Shift_L)));
                CHECK(ic->commits.empty());
                CHECK(ic->inputPanel().candidateList() && !ic->inputPanel().candidateList()->empty());

                // Move down to index 1 ("晓明"), then type 'a' directly
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_Down)));
                ic->typeAsciiToTerminal("a");
                CHECK(ic->commits.size() == 1);
                CHECK(ic->commits.back() == "晓明");
                CHECK(ic->terminalBuffer == "晓明a");
                CHECK(!ic->inputPanel().candidateList() || ic->inputPanel().candidateList()->empty());

                // Case 12: Esc cancels candidate list and restores original pinyin back to terminal!
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_Return)));
                ic->commits.clear();
                ic->terminalBuffer.clear();
                ic->typeAsciiToTerminal("xiaoming");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 17;
            } else if (stage == 17 && ic->inputPanel().candidateList() &&
                       !ic->inputPanel().candidateList()->empty()) {
                CHECK(ic->terminalBuffer.empty());
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_Escape)));
                CHECK(!ic->inputPanel().candidateList() || ic->inputPanel().candidateList()->empty());
                CHECK(ic->terminalBuffer == "xiaoming");

                // Case 13: CandidatePageSize=10 -> '0' key selects 10th candidate ("笑鸣")!
                CHECK(!ic->sendKey(fcitx::Key(FcitxKey_Return)));
                ic->commits.clear();
                ic->terminalBuffer.clear();
                {
                    std::ofstream cfgOut(tmp / "config" / "fcitx5" / "conf" / "yuntype.conf");
                    cfgOut << "HistoryEnabled=False\nShortCandidateThreshold=50\nCandidatePageSize=10\n";
                }
                addon->reloadConfig();
                ic->typeAsciiToTerminal("xiaoming10");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 18;
            } else if (stage == 18 && ic->inputPanel().candidateList() &&
                       !ic->inputPanel().candidateList()->empty()) {
                auto cl = ic->inputPanel().candidateList();
                auto *common = dynamic_cast<fcitx::CommonCandidateList *>(cl.get());
                CHECK(common != nullptr);
                CHECK(common->size() == 10);
                CHECK(common->totalPages() == 1);
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_0)));
                CHECK(ic->commits.size() == 1);
                CHECK(ic->commits.back() == "笑鸣");

                // Case 14: Single-element JSON array ["你好"] commits directly without candidate popup!
                ic->commits.clear();
                ic->terminalBuffer.clear();
                ic->typeAsciiToTerminal("nihao_single");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                stage = 19;
            } else if (stage == 19 && ic->commits.size() >= 1) {
                CHECK(ic->commits.back() == "你好");
                CHECK(!ic->inputPanel().candidateList() || ic->inputPanel().candidateList()->empty());
                addon->clearHistory();
                CHECK(!fs::exists(historyFile));

                guiIc.reset();
                ic.reset();
                addon.reset();
                instance.exit();
                return true;
            }
            src->setNextInterval(15000);
            src->setOneShot();
            return true;
        });

    instance.exec();

    if (serverPid > 0) {
        ::kill(serverPid, SIGTERM);
    }
    fs::remove_all(tmp);
}

} // namespace

int main() {
    testPinyinSuffixExtraction();
    testJsonAndSseHelpers();
    testRequestBuilding();
    testHistoryCorrectionAndPersistence();
    testTerminalShadowBuffer();
    testShortCommitCorrectionOnly();
    testEndToEndFcitx5Module();
    std::cout << "test_yuntype unit & end-to-end tests ok\n";
    return 0;
}
