#ifndef FCITX5_YUNTYPE_ADDON_H
#define FCITX5_YUNTYPE_ADDON_H

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx-config/rawconfig.h>
#include <fcitx-utils/dbus/bus.h>
#include <fcitx-utils/dbus/objectvtable.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/handlertable.h>
#include <fcitx-utils/key.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/instance.h>

#include "llm_client.h"
#include "sentence_history.h"

namespace fcitx5_yuntype {

FCITX_CONFIGURATION(
    YunTypeConfig,
    fcitx::Option<bool> enabled{this, "Enabled", "启用云打整句大模型输入", true};
    fcitx::Option<fcitx::Key> triggerKey{
        this, "TriggerKey", "触发整句转换快捷键", fcitx::Key("Alt+U")};
    fcitx::Option<bool> replaceTypedPinyin{
        this, "ReplaceTypedPinyin", "直接替换光标前刚输入的拼音", true};
    fcitx::Option<bool> historyEnabled{
        this, "HistoryEnabled", "跨窗口记住近期输入历史", false};
    fcitx::Option<int, fcitx::IntConstrain> historyCount{
        this, "HistoryCount", "完整历史条数上限", 3, fcitx::IntConstrain(1, 300)};
    fcitx::Option<int, fcitx::IntConstrain> persistIdleSeconds{
        this, "PersistIdleSeconds", "历史闲置落盘冷却（秒）", 30, fcitx::IntConstrain(1, 3600)};
    fcitx::Option<int, fcitx::IntConstrain> persistMaxSeconds{
        this, "PersistMaxSeconds", "历史最大强制落盘时长（秒）", 300, fcitx::IntConstrain(1, 86400)};
    fcitx::Option<int, fcitx::IntConstrain> shortCandidateThreshold{
        this, "ShortCandidateThreshold", "短输入候选拼音长度阈值", 50, fcitx::IntConstrain(0, 200)};
    fcitx::Option<int, fcitx::IntConstrain> candidatePageSize{
        this, "CandidatePageSize", "候选框单页显示上限", 5, fcitx::IntConstrain(2, 10)};
    fcitx::Option<std::string> sentencePrompt{
        this, "SentencePrompt", "自定义整句转换提示词", ""};
    fcitx::Option<std::string> baseUrl{
        this, "BaseUrl", "OpenAI 兼容接口地址", "https://api.deepseek.com/v1"};
    fcitx::Option<std::string> model{
        this, "Model", "模型名称", "deepseek-chat"};
    fcitx::Option<std::string> thinkingMode{
        this, "ThinkingMode", "思考强度控制模式", "default"};
    fcitx::Option<std::string> extraRequestJson{
        this, "ExtraRequestJson", "自定义额外请求 JSON", ""};
    fcitx::ExternalOption settings{
        this, "Settings", "打开云打整句输入设置界面",
        "fcitx5-yuntype-config"};
);

struct SubmittedSentence {
    std::string query;
    std::string fallback;
    bool erasedFromApp = false;
    bool started = false;
    std::optional<std::string> ready;
    std::optional<std::string> error;
};

struct PendingCandidates {
    std::string query;
    std::string fallback;
    bool erasedFromApp = false;
    std::vector<std::string> candidates;
    std::uint64_t sentRev = 0;
    int globalCursor = 0;
    std::string sessionTurns;
    int loadMoreRounds = 0;
    bool loadingMore = false;
    std::string statusNote;
};

// Extract the trailing replaceable ASCII pinyin suffix from a typed ASCII buffer.
// Stops after unmatched shell/command delimiters like ", ', `, (, [, {, =, |, >, <, $.
std::string extractReplaceablePinyinSuffix(std::string_view buffer);

class YunTypeState : public fcitx::InputContextProperty {
public:
    std::string appAscii_;
    std::string imePreeditAscii_;
    bool draftActive_ = false;
    std::string draft_;
    std::size_t draftCursor_ = 0;
    std::deque<SubmittedSentence> queue_;
    std::optional<PendingCandidates> pendingCandidates_;
    std::optional<SentenceHistory::Draft> historyDraft_;
    std::optional<SentenceHistory::ShadowDraft> shadowDraft_;
    std::uint64_t historyRevision_ = 0;

    bool hasActiveWork() const {
        return draftActive_ || !queue_.empty() || pendingCandidates_.has_value();
    }
};

class YunTypeAddon : public fcitx::AddonInstance {
public:
    explicit YunTypeAddon(fcitx::Instance *instance);
    ~YunTypeAddon() override;

    void reloadConfig() override;
    void save() override;
    const fcitx::Configuration *getConfig() const override { return &config_; }
    void setConfig(const fcitx::RawConfig &config) override;

    void clearHistory(bool removePersistedFile = true);
    void commitPendingCandidate(fcitx::InputContext *ic, std::size_t index);

private:
    void applyConfig();
    void onPreKeyEvent(fcitx::KeyEvent &event);
    void onPostKeyEvent(fcitx::KeyEvent &event);
    void onCommitString(fcitx::CommitStringEvent &event);
    void onSurroundingUpdated(fcitx::SurroundingTextUpdatedEvent &event);
    void onFocusOut(fcitx::FocusOutEvent &event);
    void onReset(fcitx::ResetEvent &event);

    bool handleTriggerKey(fcitx::InputContext *ic, YunTypeState *state, fcitx::KeyEvent &event);
    bool handleCandidateKey(fcitx::InputContext *ic, YunTypeState *state, fcitx::KeyEvent &event);
    bool handleActiveKey(fcitx::InputContext *ic, YunTypeState *state, fcitx::KeyEvent &event);
    void startLoadMoreCandidates(fcitx::InputContext *ic, YunTypeState *state);
    void commitHighlightedCandidate(fcitx::InputContext *ic, YunTypeState *state);
    void commitSentenceResult(fcitx::InputContext *ic, YunTypeState *state,
                              std::string committed, std::string turnJson,
                              std::uint64_t sentRev, std::string_view llmOriginal = {},
                              bool candidateMode = false);
    void submitSentence(fcitx::InputContext *ic, YunTypeState *state,
                        std::string query, bool erasedFromApp);
    void schedulePollTimer(bool active);
    void pollQueue();
    void updatePreedit(fcitx::InputContext *ic, YunTypeState *state);
    void erasePrecedingText(fcitx::InputContext *ic, std::size_t count,
                            bool useSurroundingDelete);
    void sendAppKeys(fcitx::InputContext *ic, fcitx::KeySym sym, std::size_t count);
    bool resetFromOwnKeys(fcitx::InputContext *ic) const;
    bool isTranslatable(fcitx::InputContext *ic) const;
    SentenceContext buildHistoryContext(fcitx::InputContext *ic, YunTypeState *state,
                                        bool candidateMode = false);
    void scheduleCompaction();
    void pollCompaction();
    void loadHistoryFromDisk();
    void markHistoryDirty();
    void schedulePersistTimer();
    void onHistoryMutated();
    void flushHistoryToDisk();

    fcitx::Instance *instance_;
    YunTypeConfig config_;
    fcitx::FactoryFor<YunTypeState> factory_;
    LlmClient client_;
    SentenceHistory history_;
    std::string historyFilePath_;
    bool historyLoaded_ = false;
    bool historyDirty_ = false;
    std::uint64_t historyDirtySinceUsec_ = 0;
    std::uint64_t historyRevision_ = 1;
    std::uint64_t historyClearedAt_ = 0;
    std::uint64_t historyCompactedAt_ = 0;
    std::uint64_t notesSentThrough_ = 0;
    bool internalCommit_ = false;

    fcitx::TrackableObjectReference<fcitx::InputContext> activeIc_;
    fcitx::TrackableObjectReference<fcitx::InputContext> ownKeysIc_;
    std::uint64_t ownKeysAtUsec_ = 0;

    std::unique_ptr<fcitx::EventSourceTime> pollTimer_;
    std::unique_ptr<fcitx::EventSourceTime> compactionTimer_;
    std::unique_ptr<fcitx::EventSourceTime> persistTimer_;
    std::vector<std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>>> watchers_;

    class DbusControl;
    std::unique_ptr<fcitx::dbus::Bus> dbusBus_;
    std::unique_ptr<DbusControl> dbusControl_;
};

} // namespace fcitx5_yuntype

#endif
