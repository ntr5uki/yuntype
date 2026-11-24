#include "yuntype.h"
#include "../core/pinyin_util.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/candidatelist.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>

namespace fcitx5_yuntype {

using yuntype::hasAsciiAlpha;
using yuntype::isAllAsciiGraphicOrSpace;

namespace {

constexpr uint64_t kPollIntervalUsec = 40000;      // 40 ms
constexpr uint64_t kCompactionIntervalUsec = 100000; // 100 ms
constexpr uint64_t kOwnKeysResetUsec = 300000;     // 300 ms
constexpr std::size_t kMaxAsciiTrackBytes = 512;

uint64_t persistIdleDelayUsec(int configSeconds) {
    if (const char *env = std::getenv("FCITX5_YUNTYPE_PERSIST_IDLE_MS"); env && *env) {
        char *end = nullptr;
        unsigned long long ms = std::strtoull(env, &end, 10);
        if (end && *end == '\0' && ms >= 10) {
            return static_cast<uint64_t>(ms) * 1000ULL;
        }
    }
    const int clamped = std::clamp(configSeconds, 1, 3600);
    return static_cast<uint64_t>(clamped) * 1000000ULL;
}

uint64_t persistMaxDelayUsec(int configSeconds) {
    if (const char *env = std::getenv("FCITX5_YUNTYPE_PERSIST_MAX_MS"); env && *env) {
        char *end = nullptr;
        unsigned long long ms = std::strtoull(env, &end, 10);
        if (end && *end == '\0' && ms >= 10) {
            return static_cast<uint64_t>(ms) * 1000ULL;
        }
    }
    const int clamped = std::clamp(configSeconds, 1, 86400);
    return static_cast<uint64_t>(clamped) * 1000000ULL;
}

uint64_t shadowTimeoutUsec() {
    if (const char *env = std::getenv("FCITX5_YUNTYPE_SHADOW_TIMEOUT_MS"); env && *env) {
        char *end = nullptr;
        unsigned long long ms = std::strtoull(env, &end, 10);
        if (end && *end == '\0' && ms >= 10) {
            return static_cast<uint64_t>(ms) * 1000ULL;
        }
    }
    return SentenceHistory::kDefaultShadowTimeoutUsec;
}

std::string defaultHistoryFilePath() {
    if (const char *env = std::getenv("FCITX5_YUNTYPE_HISTORY_FILE"); env && *env) {
        return env;
    }
    if (const char *xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/fcitx5-yuntype/history.json";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.local/share/fcitx5-yuntype/history.json";
    }
    return {};
}

bool hasCommandModifier(fcitx::KeyStates states) {
    return states.testAny(fcitx::KeyStates{
        fcitx::KeyState::Ctrl, fcitx::KeyState::Alt, fcitx::KeyState::Super});
}

std::string defaultKeyFilePath() {
    if (const char *env = std::getenv("FCITX5_YUNTYPE_KEY_FILE"); env && *env) {
        return env;
    }
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/fcitx5-yuntype/api.key";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/fcitx5-yuntype/api.key";
    }
    return {};
}

std::optional<std::string> selectedSurroundingAscii(fcitx::InputContext *ic) {
    if (!ic || !ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return std::nullopt;
    }
    const auto &s = ic->surroundingText();
    if (!s.isValid() || s.cursor() == s.anchor()) {
        return std::nullopt;
    }
    const auto &text = s.text();
    const unsigned startChar = std::min(s.cursor(), s.anchor());
    const unsigned endChar = std::max(s.cursor(), s.anchor());
    const auto totalChars = fcitx::utf8::length(text);
    if (endChar > totalChars) {
        return std::nullopt;
    }
    const std::size_t startByte =
        startChar == 0 ? 0 : fcitx::utf8::ncharByteLength(text.begin(), startChar);
    const std::size_t endByte =
        endChar == 0 ? 0 : fcitx::utf8::ncharByteLength(text.begin(), endChar);
    if (endByte <= startByte || endByte > text.size()) {
        return std::nullopt;
    }
    std::string sel = text.substr(startByte, endByte - startByte);
    if (!hasAsciiAlpha(sel)) {
        return std::nullopt;
    }
    return sel;
}

std::optional<std::string> precedingSurroundingAscii(fcitx::InputContext *ic) {
    if (!ic || !ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return std::nullopt;
    }
    const auto &s = ic->surroundingText();
    if (!s.isValid() || s.cursor() != s.anchor() || s.cursor() == 0) {
        return std::nullopt;
    }
    const auto &text = s.text();
    const auto totalChars = fcitx::utf8::length(text);
    if (s.cursor() > totalChars) {
        return std::nullopt;
    }
    const std::size_t cursorByte =
        fcitx::utf8::ncharByteLength(text.begin(), s.cursor());
    if (cursorByte == 0 || cursorByte > text.size()) {
        return std::nullopt;
    }
    std::string_view before(text.data(), cursorByte);
    if (before.size() > kMaxAsciiTrackBytes) {
        before = before.substr(before.size() - kMaxAsciiTrackBytes);
        while (!before.empty() &&
               (static_cast<unsigned char>(before.front()) & 0xC0) == 0x80) {
            before.remove_prefix(1);
        }
    }
    std::string suffix = extractReplaceablePinyinSuffix(before);
    if (suffix.empty()) {
        return std::nullopt;
    }
    return suffix;
}

bool surroundingEndsWithNonAscii(fcitx::InputContext *ic) {
    if (!ic || !ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText)) {
        return false;
    }
    const auto &s = ic->surroundingText();
    if (!s.isValid() || s.cursor() != s.anchor() || s.cursor() == 0) {
        return false;
    }
    const auto &text = s.text();
    const auto totalChars = fcitx::utf8::length(text);
    if (s.cursor() > totalChars) {
        return false;
    }
    const std::size_t cursorByte =
        fcitx::utf8::ncharByteLength(text.begin(), s.cursor());
    if (cursorByte == 0 || cursorByte > text.size()) {
        return false;
    }
    return static_cast<unsigned char>(text[cursorByte - 1]) >= 0x80;
}

class YunTypeCandidateWord : public fcitx::CandidateWord {
public:
    YunTypeCandidateWord(YunTypeAddon *addon, std::size_t index, std::string text)
        : fcitx::CandidateWord(fcitx::Text(std::move(text))),
          addon_(addon),
          index_(index) {}

    void select(fcitx::InputContext *inputContext) const override {
        if (addon_ && inputContext) {
            addon_->commitPendingCandidate(inputContext, index_);
        }
    }

private:
    YunTypeAddon *addon_;
    std::size_t index_;
};

fcitx::KeyList candidateSelectionKeys(int pageSize) {
    static constexpr fcitx::KeySym kSyms[10] = {
        FcitxKey_1, FcitxKey_2, FcitxKey_3, FcitxKey_4, FcitxKey_5,
        FcitxKey_6, FcitxKey_7, FcitxKey_8, FcitxKey_9, FcitxKey_0,
    };
    const int count = std::clamp(pageSize, 1, 10);
    fcitx::KeyList keys;
    keys.reserve(static_cast<std::size_t>(count));
    for (int i = 0; i < count; ++i) {
        keys.emplace_back(kSyms[i]);
    }
    return keys;
}

} // namespace

std::string extractReplaceablePinyinSuffix(std::string_view buffer) {
    return yuntype::extractReplaceablePinyinSuffix(buffer);
}

class YunTypeAddon::DbusControl : public fcitx::dbus::ObjectVTable<DbusControl> {
public:
    explicit DbusControl(YunTypeAddon *addon) : addon_(addon) {}

    void clearHistory() { addon_->clearHistory(); }
    void reloadConfig() { addon_->reloadConfig(); }
    std::string version() { return FCITX5_YUNTYPE_VERSION; }

private:
    FCITX_OBJECT_VTABLE_METHOD(clearHistory, "ClearHistory", "", "");
    FCITX_OBJECT_VTABLE_METHOD(reloadConfig, "ReloadConfig", "", "");
    FCITX_OBJECT_VTABLE_METHOD(version, "Version", "", "s");
    YunTypeAddon *addon_;
};

YunTypeAddon::YunTypeAddon(fcitx::Instance *instance)
    : instance_(instance),
      factory_([](fcitx::InputContext &) { return new YunTypeState; }) {
    instance_->inputContextManager().registerProperty("yuntypeState", &factory_);
    reloadConfig();

    try {
        auto bus = std::make_unique<fcitx::dbus::Bus>(fcitx::dbus::BusType::Session);
        if (bus->isOpen()) {
            bus->attachEventLoop(&instance_->eventLoop());
            auto ctrl = std::make_unique<DbusControl>(this);
            if (bus->addObjectVTable("/org/fcitx/YunType", "org.fcitx.Fcitx.YunType1", *ctrl) &&
                bus->requestName("org.fcitx.Fcitx.YunType",
                                 fcitx::Flags<fcitx::dbus::RequestNameFlag>{
                                     fcitx::dbus::RequestNameFlag::ReplaceExisting,
                                     fcitx::dbus::RequestNameFlag::AllowReplacement})) {
                dbusBus_ = std::move(bus);
                dbusControl_ = std::move(ctrl);
            }
        }
    } catch (...) {
    }

    watchers_.push_back(instance_->watchEvent(
        fcitx::EventType::InputContextKeyEvent, fcitx::EventWatcherPhase::PreInputMethod,
        [this](fcitx::Event &event) {
            onPreKeyEvent(static_cast<fcitx::KeyEvent &>(event));
        }));
    watchers_.push_back(instance_->watchEvent(
        fcitx::EventType::InputContextKeyEvent, fcitx::EventWatcherPhase::PostInputMethod,
        [this](fcitx::Event &event) {
            onPostKeyEvent(static_cast<fcitx::KeyEvent &>(event));
        }));
    watchers_.push_back(instance_->watchEvent(
        fcitx::EventType::InputContextCommitString, fcitx::EventWatcherPhase::Default,
        [this](fcitx::Event &event) {
            onCommitString(static_cast<fcitx::CommitStringEvent &>(event));
        }));
    watchers_.push_back(instance_->watchEvent(
        fcitx::EventType::InputContextSurroundingTextUpdated, fcitx::EventWatcherPhase::Default,
        [this](fcitx::Event &event) {
            onSurroundingUpdated(static_cast<fcitx::SurroundingTextUpdatedEvent &>(event));
        }));
    watchers_.push_back(instance_->watchEvent(
        fcitx::EventType::InputContextFocusOut, fcitx::EventWatcherPhase::Default,
        [this](fcitx::Event &event) {
            onFocusOut(static_cast<fcitx::FocusOutEvent &>(event));
        }));
    watchers_.push_back(instance_->watchEvent(
        fcitx::EventType::InputContextReset, fcitx::EventWatcherPhase::Default,
        [this](fcitx::Event &event) {
            onReset(static_cast<fcitx::ResetEvent &>(event));
        }));
}

YunTypeAddon::~YunTypeAddon() {
    flushHistoryToDisk();
}

void YunTypeAddon::save() {
    flushHistoryToDisk();
}

void YunTypeAddon::reloadConfig() {
    fcitx::readAsIni(config_, "conf/yuntype.conf");
    applyConfig();
}

void YunTypeAddon::setConfig(const fcitx::RawConfig &config) {
    config_.load(config, true);
    fcitx::safeSaveAsIni(config_, "conf/yuntype.conf");
    applyConfig();
}

void YunTypeAddon::applyConfig() {
    LlmConfig llmCfg;
    llmCfg.baseUrl = *config_.baseUrl;
    llmCfg.model = *config_.model;
    llmCfg.keyFile = defaultKeyFilePath();
    llmCfg.prompt = *config_.sentencePrompt;
    llmCfg.thinkingMode = *config_.thinkingMode;
    llmCfg.extraRequestJson = *config_.extraRequestJson;
    llmCfg.shortCandidateThreshold = *config_.shortCandidateThreshold;
    llmCfg.candidatePageSize = *config_.candidatePageSize;
    if (const char *envUrl = std::getenv("FCITX5_YUNTYPE_URL"); envUrl && *envUrl) {
        llmCfg.baseUrl = envUrl;
    }
    if (const char *envModel = std::getenv("FCITX5_YUNTYPE_MODEL"); envModel && *envModel) {
        llmCfg.model = envModel;
    }
    client_.configure(llmCfg);

    const std::string newHistoryPath = defaultHistoryFilePath();
    if (newHistoryPath != historyFilePath_) {
        flushHistoryToDisk();
        historyFilePath_ = newHistoryPath;
        historyLoaded_ = false;
    }

    if (!*config_.historyEnabled) {
        flushHistoryToDisk();
        clearHistory(false);
    } else {
        if (!historyLoaded_) {
            loadHistoryFromDisk();
        }
        if (historyDirty_) {
            schedulePersistTimer();
        }
        scheduleCompaction();
    }
}

void YunTypeAddon::loadHistoryFromDisk() {
    historyLoaded_ = true;
    historyDirty_ = false;
    historyDirtySinceUsec_ = 0;
    if (historyFilePath_.empty()) {
        return;
    }
    if (history_.loadFromFile(historyFilePath_)) {
        notesSentThrough_ = 1;
        historyRevision_ = std::max<std::uint64_t>(historyRevision_, 2);
    }
}

void YunTypeAddon::markHistoryDirty() {
    if (!*config_.historyEnabled || historyFilePath_.empty()) {
        return;
    }
    const uint64_t now = fcitx::now(CLOCK_MONOTONIC);
    if (!historyDirty_) {
        historyDirty_ = true;
        historyDirtySinceUsec_ = now;
    }
    schedulePersistTimer();
}

void YunTypeAddon::schedulePersistTimer() {
    if (!historyDirty_ || !*config_.historyEnabled || historyFilePath_.empty()) {
        return;
    }
    const uint64_t now = fcitx::now(CLOCK_MONOTONIC);
    const uint64_t idleAt = now + persistIdleDelayUsec(*config_.persistIdleSeconds);
    const uint64_t capAt =
        historyDirtySinceUsec_ + persistMaxDelayUsec(*config_.persistMaxSeconds);
    const uint64_t target = std::min(idleAt, capAt);
    if (target <= now) {
        flushHistoryToDisk();
        return;
    }
    if (!persistTimer_) {
        persistTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, target, 1000,
            [this](fcitx::EventSourceTime *, uint64_t) {
                flushHistoryToDisk();
                return true;
            });
    } else {
        persistTimer_->setTime(target);
        persistTimer_->setEnabled(true);
    }
    persistTimer_->setOneShot();
}

void YunTypeAddon::onHistoryMutated() {
    markHistoryDirty();
    if (history_.compactionStale()) {
        client_.cancelCompaction();
        history_.deferCompaction();
    }
}

void YunTypeAddon::flushHistoryToDisk() {
    if (persistTimer_) {
        persistTimer_->setEnabled(false);
    }
    if (!historyDirty_ || historyFilePath_.empty()) {
        return;
    }
    if (history_.saveToFile(historyFilePath_, notesSentThrough_)) {
        historyDirty_ = false;
        historyDirtySinceUsec_ = 0;
    }
}

void YunTypeAddon::clearHistory(bool removePersistedFile) {
    client_.cancelCompaction();
    if (persistTimer_) {
        persistTimer_->setEnabled(false);
    }
    historyDirty_ = false;
    historyDirtySinceUsec_ = 0;
    history_ = {};
    historyClearedAt_ = ++historyRevision_;
    notesSentThrough_ = 0;
    if (removePersistedFile) {
        if (!historyFilePath_.empty()) {
            std::error_code ec;
            std::filesystem::remove(historyFilePath_, ec);
        }
    } else {
        historyLoaded_ = false;
    }
    instance_->inputContextManager().foreach([this](fcitx::InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->historyDraft_.reset();
        state->shadowDraft_.reset();
        state->historyRevision_ = 0;
        return true;
    });
}

bool YunTypeAddon::isTranslatable(fcitx::InputContext *ic) const {
    return ic && !ic->capabilityFlags().testAny(fcitx::CapabilityFlags{
                     fcitx::CapabilityFlag::PasswordOrSensitive});
}

bool YunTypeAddon::resetFromOwnKeys(fcitx::InputContext *ic) const {
    return ic && ownKeysIc_.get() == ic && ownKeysAtUsec_ != 0 &&
           fcitx::now(CLOCK_MONOTONIC) - ownKeysAtUsec_ < kOwnKeysResetUsec;
}

void YunTypeAddon::erasePrecedingText(fcitx::InputContext *ic, std::size_t count,
                                          bool useSurroundingDelete) {
    if (!ic || count == 0) {
        return;
    }
    if (useSurroundingDelete) {
        ownKeysIc_ = ic->watch();
        ownKeysAtUsec_ = fcitx::now(CLOCK_MONOTONIC);
        ic->deleteSurroundingText(-static_cast<int>(count),
                                  static_cast<unsigned int>(count));
        return;
    }
    sendAppKeys(ic, FcitxKey_BackSpace, count);
}

void YunTypeAddon::sendAppKeys(fcitx::InputContext *ic, fcitx::KeySym sym, std::size_t count) {
    if (!ic || count == 0) {
        return;
    }
    ownKeysIc_ = ic->watch();
    ownKeysAtUsec_ = fcitx::now(CLOCK_MONOTONIC);
    const fcitx::Key key(sym);
    for (std::size_t i = 0; i < count; ++i) {
        ic->forwardKey(key, false);
        ic->forwardKey(key, true);
    }
}

void YunTypeAddon::onPreKeyEvent(fcitx::KeyEvent &event) {
    if (!*config_.enabled || event.accepted()) {
        return;
    }
    auto *ic = event.inputContext();
    if (!isTranslatable(ic)) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    const fcitx::Key key = event.key();

    // Check trigger hotkey (e.g. Alt+U)
    if (config_.triggerKey->sym() != FcitxKey_None &&
        key.check(*config_.triggerKey)) {
        if (event.isRelease() || event.rawKey().states().test(fcitx::KeyState::Repeat)) {
            event.filterAndAccept();
            return;
        }
        if (handleTriggerKey(ic, state, event)) {
            event.filterAndAccept();
            return;
        }
    }

    if (state->hasActiveWork()) {
        if (event.isRelease()) {
            return;
        }
        if (handleActiveKey(ic, state, event)) {
            event.filterAndAccept();
            return;
        }
    }
}

void YunTypeAddon::onPostKeyEvent(fcitx::KeyEvent &event) {
    if (!*config_.enabled || event.isRelease()) {
        return;
    }
    auto *ic = event.inputContext();
    if (!isTranslatable(ic)) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    if (state->hasActiveWork()) {
        return;
    }

    const fcitx::Key key = event.key();
    const auto sym = key.sym();

    if (event.accepted()) {
        // Key was consumed by the foreground IME (e.g., typing pinyin in WowType or Rime preedit)
        if (*config_.historyEnabled && state->shadowDraft_) {
            history_.touchShadow(state->shadowDraft_, fcitx::now(CLOCK_MONOTONIC),
                                 shadowTimeoutUsec());
        }
        if (!hasCommandModifier(key.states())) {
            if (sym >= 0x20 && sym <= 0x7E) {
                state->imePreeditAscii_.push_back(static_cast<char>(sym));
                if (state->imePreeditAscii_.size() > kMaxAsciiTrackBytes) {
                    state->imePreeditAscii_.erase(0, state->imePreeditAscii_.size() - kMaxAsciiTrackBytes);
                }
            } else if (sym == FcitxKey_BackSpace) {
                if (!state->imePreeditAscii_.empty()) {
                    state->imePreeditAscii_.pop_back();
                }
            } else if (sym == FcitxKey_Escape) {
                state->imePreeditAscii_.clear();
            }
        }
        return;
    }

    // Key was NOT accepted by foreground IME -> goes directly into the app / terminal!
    state->imePreeditAscii_.clear();
    if (hasCommandModifier(key.states())) {
        // Modifier shortcuts (Ctrl+C, Ctrl+U, Ctrl+W, Alt+B, etc.) change terminal cursor/line
        if (sym != FcitxKey_Shift_L && sym != FcitxKey_Shift_R &&
            sym != FcitxKey_Control_L && sym != FcitxKey_Control_R &&
            sym != FcitxKey_Alt_L && sym != FcitxKey_Alt_R &&
            sym != FcitxKey_Super_L && sym != FcitxKey_Super_R) {
            state->appAscii_.clear();
            state->shadowDraft_.reset();
        }
        return;
    }

    const uint64_t now = fcitx::now(CLOCK_MONOTONIC);
    const uint64_t shadowTimeout = shadowTimeoutUsec();
    const bool trackShadow = *config_.historyEnabled && state->shadowDraft_.has_value();

    if (sym >= 0x20 && sym <= 0x7E) {
        const char ch = static_cast<char>(sym);
        state->appAscii_.push_back(ch);
        if (state->appAscii_.size() > kMaxAsciiTrackBytes) {
            state->appAscii_.erase(0, state->appAscii_.size() - kMaxAsciiTrackBytes);
        }
        if (trackShadow &&
            history_.shadowInsertAscii(state->shadowDraft_, std::string_view(&ch, 1),
                                       ++historyRevision_, now, shadowTimeout)) {
            onHistoryMutated();
        }
    } else if (sym == FcitxKey_BackSpace) {
        if (!state->appAscii_.empty()) {
            state->appAscii_.pop_back();
        }
        if (trackShadow &&
            history_.shadowBackspace(state->shadowDraft_, ++historyRevision_, now, shadowTimeout)) {
            onHistoryMutated();
        }
    } else if (sym == FcitxKey_Delete || sym == FcitxKey_KP_Delete) {
        state->appAscii_.clear();
        if (trackShadow &&
            history_.shadowDelete(state->shadowDraft_, ++historyRevision_, now, shadowTimeout)) {
            onHistoryMutated();
        }
    } else if (sym == FcitxKey_Left) {
        state->appAscii_.clear();
        if (trackShadow) {
            history_.shadowMoveLeft(state->shadowDraft_, now, shadowTimeout);
        }
    } else if (sym == FcitxKey_Right) {
        state->appAscii_.clear();
        if (trackShadow) {
            history_.shadowMoveRight(state->shadowDraft_, now, shadowTimeout);
        }
    } else if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter ||
               sym == FcitxKey_Up || sym == FcitxKey_Down ||
               sym == FcitxKey_Home || sym == FcitxKey_End ||
               sym == FcitxKey_Page_Up || sym == FcitxKey_Page_Down ||
               sym == FcitxKey_Tab || sym == FcitxKey_Escape) {
        state->appAscii_.clear();
        state->shadowDraft_.reset();
    }
}

void YunTypeAddon::onCommitString(fcitx::CommitStringEvent &event) {
    auto *ic = event.inputContext();
    if (!ic) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    state->imePreeditAscii_.clear();
    if (internalCommit_) {
        return;
    }
    const std::string &text = event.text();
    if (isAllAsciiGraphicOrSpace(text)) {
        // User pressed Enter in a Chinese IME to commit raw ASCII pinyin into the app/terminal
        state->appAscii_ += text;
        if (state->appAscii_.size() > kMaxAsciiTrackBytes) {
            state->appAscii_.erase(0, state->appAscii_.size() - kMaxAsciiTrackBytes);
        }
        if (*config_.historyEnabled && isTranslatable(ic) && state->shadowDraft_) {
            if (history_.shadowInsertAscii(state->shadowDraft_, text, ++historyRevision_,
                                           fcitx::now(CLOCK_MONOTONIC), shadowTimeoutUsec())) {
                onHistoryMutated();
            }
        }
    } else {
        state->appAscii_.clear();
        if (*config_.historyEnabled && isTranslatable(ic) && !text.empty()) {
            history_.recordCommit(state->historyDraft_, state->shadowDraft_, *ic, text, nullptr,
                                  ++historyRevision_, fcitx::now(CLOCK_MONOTONIC),
                                  shadowTimeoutUsec());
            onHistoryMutated();
            scheduleCompaction();
        }
    }
}

void YunTypeAddon::onSurroundingUpdated(fcitx::SurroundingTextUpdatedEvent &event) {
    auto *ic = event.inputContext();
    if (!ic || !*config_.historyEnabled || !isTranslatable(ic) || !ic->hasFocus()) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) &&
        ic->surroundingText().isValid()) {
        state->shadowDraft_.reset();
    }
    if (history_.update(state->historyDraft_, *ic, ++historyRevision_)) {
        onHistoryMutated();
    }
}

void YunTypeAddon::onFocusOut(fcitx::FocusOutEvent &event) {
    auto *ic = event.inputContext();
    if (!ic) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    state->appAscii_.clear();
    state->imePreeditAscii_.clear();
    state->shadowDraft_.reset();
    if (state->historyDraft_) {
        state->historyDraft_->awaiting = true;
    }
    if (state->hasActiveWork()) {
        if (!state->queue_.empty() && state->queue_.front().started) {
            client_.cancelSentence(state->queue_.front().query);
        }
        if (state->pendingCandidates_ && state->pendingCandidates_->loadingMore) {
            client_.cancelSentence(state->pendingCandidates_->query);
        }
        state->queue_.clear();
        state->pendingCandidates_.reset();
        state->draft_.clear();
        state->draftCursor_ = 0;
        state->draftActive_ = false;
        updatePreedit(ic, state);
    }
}

void YunTypeAddon::onReset(fcitx::ResetEvent &event) {
    auto *ic = event.inputContext();
    if (!ic || resetFromOwnKeys(ic)) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    state->appAscii_.clear();
    state->imePreeditAscii_.clear();
    state->shadowDraft_.reset();
    if (state->hasActiveWork()) {
        if (!state->queue_.empty() && state->queue_.front().started) {
            client_.cancelSentence(state->queue_.front().query);
        }
        if (state->pendingCandidates_ && state->pendingCandidates_->loadingMore) {
            client_.cancelSentence(state->pendingCandidates_->query);
        }
        state->queue_.clear();
        state->pendingCandidates_.reset();
        state->draft_.clear();
        state->draftCursor_ = 0;
        state->draftActive_ = false;
        updatePreedit(ic, state);
    }
}

bool YunTypeAddon::handleTriggerKey(fcitx::InputContext *ic, YunTypeState *state,
                                        fcitx::KeyEvent & /*event*/) {
    // 0. If candidate list is active: commit the currently highlighted candidate
    if (state->pendingCandidates_ && !state->draftActive_) {
        commitHighlightedCandidate(ic, state);
        return true;
    }

    // 1. If already in draft mode: submit the draft or exit empty draft
    if (state->draftActive_) {
        if (state->draft_.empty()) {
            state->draftActive_ = false;
            updatePreedit(ic, state);
        } else {
            std::string query = std::move(state->draft_);
            state->draft_.clear();
            state->draftCursor_ = 0;
            state->draftActive_ = false;
            submitSentence(ic, state, std::move(query), false);
        }
        return true;
    }

    if (*config_.replaceTypedPinyin) {
        // 2. If GUI app reports an active SurroundingText selection with ASCII letters
        if (auto selected = selectedSurroundingAscii(ic)) {
            state->appAscii_.clear();
            state->imePreeditAscii_.clear();
            submitSentence(ic, state, std::move(*selected), false);
            return true;
        }

        // 3. If user was typing pinyin in foreground Chinese IME's preedit (not yet committed)
        if (hasAsciiAlpha(state->imePreeditAscii_)) {
            std::string query = std::move(state->imePreeditAscii_);
            state->imePreeditAscii_.clear();
            state->appAscii_.clear();
            if (auto *engine = instance_->inputMethodEngine(ic)) {
                if (const auto *entry = instance_->inputMethodEntry(ic)) {
                    fcitx::InputContextEvent resetEv(ic, fcitx::EventType::InputContextReset);
                    engine->reset(*entry, resetEv);
                }
            }
            submitSentence(ic, state, std::move(query), true);
            return true;
        }

        // 4. Replace ASCII pinyin immediately before cursor (GUI SurroundingText or typed app buffer)
        auto surroundingSuffix = precedingSurroundingAscii(ic);
        std::string appSuffix = extractReplaceablePinyinSuffix(state->appAscii_);
        if (!surroundingSuffix && surroundingEndsWithNonAscii(ic)) {
            appSuffix.clear();
            state->appAscii_.clear();
        }

        std::string suffix;
        bool useSurroundingDelete = false;
        const bool hasSurroundingCap =
            ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText);
        const bool surroundingValid = hasSurroundingCap && ic->surroundingText().isValid();

        if (surroundingSuffix && !appSuffix.empty() &&
            appSuffix.size() > surroundingSuffix->size() &&
            appSuffix.starts_with(*surroundingSuffix)) {
            // Typed buffer is slightly ahead of async SurroundingText updates
            suffix = std::move(appSuffix);
            useSurroundingDelete =
                surroundingValid &&
                std::string_view(ic->frontend()) != "wayland_im";
        } else if (surroundingSuffix) {
            suffix = std::move(*surroundingSuffix);
            useSurroundingDelete = true;
        } else if (!appSuffix.empty()) {
            suffix = std::move(appSuffix);
            useSurroundingDelete =
                surroundingValid &&
                ic->surroundingText().cursor() >= fcitx::utf8::length(suffix);
        }

        if (!suffix.empty()) {
            const std::size_t charCount = fcitx::utf8::length(suffix);
            if (state->appAscii_.size() >= suffix.size() &&
                state->appAscii_.ends_with(suffix)) {
                state->appAscii_.erase(state->appAscii_.size() - suffix.size());
            } else {
                state->appAscii_.clear();
            }
            if (!useSurroundingDelete && *config_.historyEnabled && isTranslatable(ic) &&
                state->shadowDraft_) {
                if (history_.shadowEraseSuffixBeforeCursor(
                        state->shadowDraft_, suffix, ++historyRevision_,
                        fcitx::now(CLOCK_MONOTONIC), shadowTimeoutUsec())) {
                    onHistoryMutated();
                }
            }
            erasePrecedingText(ic, charCount, useSurroundingDelete);
            submitSentence(ic, state, std::move(suffix), true);
            return true;
        }
    }

    // 5. Otherwise, enter standalone preedit draft mode
    if (*config_.historyEnabled && state->shadowDraft_) {
        history_.touchShadow(state->shadowDraft_, fcitx::now(CLOCK_MONOTONIC), shadowTimeoutUsec());
    }
    state->draftActive_ = true;
    state->draft_.clear();
    state->draftCursor_ = 0;
    updatePreedit(ic, state);
    return true;
}

bool YunTypeAddon::handleCandidateKey(fcitx::InputContext *ic, YunTypeState *state,
                                      fcitx::KeyEvent &event) {
    if (!state->pendingCandidates_) {
        return false;
    }
    const fcitx::Key key = event.key();
    if (key.isModifier() || hasCommandModifier(key.states())) {
        return false;
    }
    const auto sym = key.sym();
    if (*config_.historyEnabled && state->shadowDraft_) {
        history_.touchShadow(state->shadowDraft_, fcitx::now(CLOCK_MONOTONIC), shadowTimeoutUsec());
    }
    if (event.rawKey().states().test(fcitx::KeyState::Repeat) &&
        (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter || sym == FcitxKey_space)) {
        return true;
    }

    auto &pc = *state->pendingCandidates_;
    auto candList = ic->inputPanel().candidateList();
    auto *common = dynamic_cast<fcitx::CommonCandidateList *>(candList.get());
    const int pageSize = std::clamp(*config_.candidatePageSize, 2, 10);

    auto syncCursor = [&]() {
        if (common) {
            const int g = common->globalCursorIndex();
            if (g >= 0 && g < static_cast<int>(pc.candidates.size())) {
                pc.globalCursor = g;
            }
        }
    };

    if (sym == FcitxKey_Escape) {
        PendingCandidates cancelled = std::move(pc);
        state->pendingCandidates_.reset();
        if (cancelled.loadingMore) {
            client_.cancelSentence(cancelled.query);
        }
        if (cancelled.erasedFromApp && state->queue_.empty()) {
            updatePreedit(ic, state);
            internalCommit_ = true;
            ic->commitString(cancelled.query);
            internalCommit_ = false;
            state->appAscii_ += cancelled.query;
            if (*config_.historyEnabled && isTranslatable(ic) && state->shadowDraft_) {
                if (history_.shadowInsertAscii(state->shadowDraft_, cancelled.query,
                                               ++historyRevision_,
                                               fcitx::now(CLOCK_MONOTONIC),
                                               shadowTimeoutUsec())) {
                    onHistoryMutated();
                }
            }
        } else {
            state->draft_ = std::move(cancelled.query);
            state->draftCursor_ = state->draft_.size();
            state->draftActive_ = true;
            updatePreedit(ic, state);
        }
        pollQueue();
        return true;
    }

    if (sym == FcitxKey_BackSpace) {
        PendingCandidates cancelled = std::move(pc);
        state->pendingCandidates_.reset();
        if (cancelled.loadingMore) {
            client_.cancelSentence(cancelled.query);
        }
        state->draft_ = std::move(cancelled.query);
        state->draftCursor_ = state->draft_.size();
        state->draftActive_ = true;
        updatePreedit(ic, state);
        return true;
    }

    if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter || sym == FcitxKey_space) {
        commitHighlightedCandidate(ic, state);
        return true;
    }

    if (sym == FcitxKey_plus || sym == FcitxKey_KP_Add) {
        startLoadMoreCandidates(ic, state);
        return true;
    }

    if (sym == FcitxKey_minus || sym == FcitxKey_KP_Subtract ||
        sym == FcitxKey_Page_Up || sym == FcitxKey_KP_Page_Up) {
        if (auto *pageable = candList ? candList->toPageable() : nullptr;
            pageable && pageable->hasPrev()) {
            pageable->prev();
            syncCursor();
            if (!pc.loadingMore && !pc.statusNote.empty()) {
                pc.statusNote.clear();
                updatePreedit(ic, state);
            } else {
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            }
        }
        return true;
    }

    if (sym == FcitxKey_equal || sym == FcitxKey_Page_Down || sym == FcitxKey_KP_Page_Down) {
        if (auto *pageable = candList ? candList->toPageable() : nullptr;
            pageable && pageable->hasNext()) {
            pageable->next();
            syncCursor();
            if (!pc.loadingMore && !pc.statusNote.empty()) {
                pc.statusNote.clear();
                updatePreedit(ic, state);
            } else {
                ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
            }
        } else {
            startLoadMoreCandidates(ic, state);
        }
        return true;
    }

    if (sym == FcitxKey_Up || sym == FcitxKey_Left) {
        if (auto *movable = candList ? candList->toCursorMovable() : nullptr) {
            movable->prevCandidate();
            syncCursor();
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return true;
    }

    if (sym == FcitxKey_Down || sym == FcitxKey_Right || sym == FcitxKey_Tab) {
        if (auto *movable = candList ? candList->toCursorMovable() : nullptr) {
            movable->nextCandidate();
            syncCursor();
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return true;
    }

    if (sym == FcitxKey_Home) {
        if (common && !pc.candidates.empty()) {
            common->setPage(0);
            common->setCursorIndex(0);
            pc.globalCursor = 0;
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return true;
    }

    if (sym == FcitxKey_End) {
        if (common && !pc.candidates.empty()) {
            const int last = static_cast<int>(pc.candidates.size()) - 1;
            common->setPage(last / pageSize);
            common->setCursorIndex(last % pageSize);
            pc.globalCursor = last;
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        return true;
    }

    if ((sym >= FcitxKey_1 && sym <= FcitxKey_9) || sym == FcitxKey_0) {
        const int idxInPage = (sym == FcitxKey_0) ? 9 : static_cast<int>(sym - FcitxKey_1);
        if (candList && idxInPage >= 0 && idxInPage < candList->size()) {
            candList->candidate(idxInPage).select(ic);
            return true;
        }
        if (idxInPage >= 0 && idxInPage < pageSize) {
            return true;
        }
    }

    // Direct continuation: typing a new letter/punctuation commits the highlighted candidate
    // and passes the new key through naturally.
    if (sym >= 0x20 && sym <= 0x7E) {
        commitHighlightedCandidate(ic, state);
        return false;
    }

    return false;
}

bool YunTypeAddon::handleActiveKey(fcitx::InputContext *ic, YunTypeState *state,
                                       fcitx::KeyEvent &event) {
    if (state->pendingCandidates_ && !state->draftActive_) {
        return handleCandidateKey(ic, state, event);
    }

    const fcitx::Key key = event.key();
    if (hasCommandModifier(key.states())) {
        return false;
    }
    const auto sym = key.sym();
    if (*config_.historyEnabled && state->shadowDraft_) {
        history_.touchShadow(state->shadowDraft_, fcitx::now(CLOCK_MONOTONIC), shadowTimeoutUsec());
    }
    if (event.rawKey().states().test(fcitx::KeyState::Repeat) &&
        (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter)) {
        return true;
    }

    if (!state->draftActive_) {
        // Queue is running/errored and no new draft has started yet
        if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) {
            if (!state->queue_.empty()) {
                auto &front = state->queue_.front();
                if (front.started) {
                    client_.cancelSentence(front.query);
                }
                front.ready = front.fallback;
                front.error.reset();
                pollQueue();
            }
            return true;
        }
        if (sym == FcitxKey_Escape || sym == FcitxKey_BackSpace) {
            if (!state->queue_.empty()) {
                auto item = std::move(state->queue_.back());
                state->queue_.pop_back();
                if (item.started) {
                    client_.cancelSentence(item.query);
                }
                if (sym == FcitxKey_Escape && item.erasedFromApp && state->queue_.empty()) {
                    // Restore back into the terminal/app immediately
                    updatePreedit(ic, state);
                    internalCommit_ = true;
                    ic->commitString(item.query);
                    internalCommit_ = false;
                    state->appAscii_ += item.query;
                    if (*config_.historyEnabled && isTranslatable(ic) && state->shadowDraft_) {
                        if (history_.shadowInsertAscii(state->shadowDraft_, item.query,
                                                       ++historyRevision_,
                                                       fcitx::now(CLOCK_MONOTONIC),
                                                       shadowTimeoutUsec())) {
                            onHistoryMutated();
                        }
                    }
                } else {
                    state->draft_ = std::move(item.query);
                    state->draftCursor_ = state->draft_.size();
                    state->draftActive_ = true;
                    updatePreedit(ic, state);
                }
            }
            return true;
        }
    }

    switch (sym) {
    case FcitxKey_Escape:
        state->draft_.clear();
        state->draftCursor_ = 0;
        state->draftActive_ = false;
        updatePreedit(ic, state);
        return true;
    case FcitxKey_Return:
    case FcitxKey_KP_Enter:
        if (state->draft_.empty()) {
            state->draftActive_ = false;
            updatePreedit(ic, state);
        } else {
            std::string query = std::move(state->draft_);
            state->draft_.clear();
            state->draftCursor_ = 0;
            state->draftActive_ = false;
            submitSentence(ic, state, std::move(query), false);
        }
        return true;
    case FcitxKey_BackSpace:
        if (state->draftCursor_ > 0) {
            std::size_t prev = state->draftCursor_ - 1;
            while (prev > 0 && (static_cast<unsigned char>(state->draft_[prev]) & 0xC0) == 0x80) {
                --prev;
            }
            state->draft_.erase(prev, state->draftCursor_ - prev);
            state->draftCursor_ = prev;
            updatePreedit(ic, state);
        }
        return true;
    case FcitxKey_Delete:
    case FcitxKey_KP_Delete:
        if (state->draftCursor_ < state->draft_.size()) {
            std::size_t next = state->draftCursor_ + 1;
            while (next < state->draft_.size() &&
                   (static_cast<unsigned char>(state->draft_[next]) & 0xC0) == 0x80) {
                ++next;
            }
            state->draft_.erase(state->draftCursor_, next - state->draftCursor_);
            updatePreedit(ic, state);
        }
        return true;
    case FcitxKey_Left:
        if (state->draftCursor_ > 0) {
            --state->draftCursor_;
            while (state->draftCursor_ > 0 &&
                   (static_cast<unsigned char>(state->draft_[state->draftCursor_]) & 0xC0) == 0x80) {
                --state->draftCursor_;
            }
            updatePreedit(ic, state);
        }
        return true;
    case FcitxKey_Right:
        if (state->draftCursor_ < state->draft_.size()) {
            ++state->draftCursor_;
            while (state->draftCursor_ < state->draft_.size() &&
                   (static_cast<unsigned char>(state->draft_[state->draftCursor_]) & 0xC0) == 0x80) {
                ++state->draftCursor_;
            }
            updatePreedit(ic, state);
        }
        return true;
    case FcitxKey_Home:
        state->draftCursor_ = 0;
        updatePreedit(ic, state);
        return true;
    case FcitxKey_End:
        state->draftCursor_ = state->draft_.size();
        updatePreedit(ic, state);
        return true;
    default:
        if (sym >= 0x20 && sym <= 0x7E) {
            char c = static_cast<char>(sym);
            state->draftActive_ = true;
            state->draft_.insert(state->draftCursor_, 1, c);
            ++state->draftCursor_;
            updatePreedit(ic, state);
            return true;
        }
        return state->draftActive_;
    }
}

void YunTypeAddon::submitSentence(fcitx::InputContext *ic, YunTypeState *state,
                                      std::string query, bool erasedFromApp) {
    const bool needsLlm = hasAsciiAlpha(query);
    state->queue_.push_back(SubmittedSentence{
        query,
        query,
        erasedFromApp,
        false,
        needsLlm ? std::nullopt : std::optional<std::string>(query),
        std::nullopt,
    });
    activeIc_ = ic->watch();
    pollQueue();
}

SentenceContext YunTypeAddon::buildHistoryContext(fcitx::InputContext *ic,
                                                      YunTypeState *state,
                                                      bool candidateMode) {
    if (!*config_.historyEnabled || !isTranslatable(ic)) {
        return {};
    }
    if (history_.update(state->historyDraft_, *ic, ++historyRevision_)) {
        onHistoryMutated();
    }
    const std::size_t budget = client_.historyBudget();
    const std::size_t corrBudget = std::max<std::size_t>(budget / 3, 256);
    if (candidateMode) {
        return SentenceContext{
            {},
            {},
            {},
            history_.corrections(std::min<std::size_t>(corrBudget, 1024), 0),
            historyRevision_,
            false,
        };
    }
    std::string turns = history_.replayTurns(budget);
    const std::uint64_t after = turns.empty() ? 0 : notesSentThrough_;
    return SentenceContext{
        std::move(turns),
        history_.newText(budget, after),
        history_.summary(),
        history_.corrections(corrBudget, after),
        historyRevision_,
        true,
    };
}

void YunTypeAddon::commitSentenceResult(fcitx::InputContext *ic, YunTypeState *state,
                                        std::string committed, std::string turnJson,
                                        std::uint64_t sentRev, std::string_view llmOriginal,
                                        bool candidateMode) {
    internalCommit_ = true;
    const bool allowHistory =
        *config_.historyEnabled && isTranslatable(ic) &&
        ((turnJson.empty() && !candidateMode) || sentRev >= historyClearedAt_);
    if (allowHistory) {
        if (candidateMode) {
            if (history_.recordShortCommit(state->historyDraft_, state->shadowDraft_, *ic,
                                           committed, ++historyRevision_,
                                           fcitx::now(CLOCK_MONOTONIC), shadowTimeoutUsec(),
                                           llmOriginal)) {
                onHistoryMutated();
                scheduleCompaction();
            }
        } else {
            if (!turnJson.empty() && sentRev >= historyCompactedAt_) {
                notesSentThrough_ = std::max(notesSentThrough_, sentRev);
            }
            history_.recordCommit(state->historyDraft_, state->shadowDraft_, *ic, committed,
                                  turnJson.empty() ? nullptr : turnJson.c_str(),
                                  ++historyRevision_, fcitx::now(CLOCK_MONOTONIC),
                                  shadowTimeoutUsec(), llmOriginal);
            onHistoryMutated();
            scheduleCompaction();
        }
    }
    ic->commitString(committed);
    internalCommit_ = false;
}

void YunTypeAddon::startLoadMoreCandidates(fcitx::InputContext *ic, YunTypeState *state) {
    if (!ic || !state || !state->pendingCandidates_) {
        return;
    }
    auto &pc = *state->pendingCandidates_;
    if (pc.loadingMore) {
        return;
    }
    constexpr int kMaxLoadMoreRounds = 5;
    if (pc.loadMoreRounds >= kMaxLoadMoreRounds) {
        pc.statusNote = "·";
        updatePreedit(ic, state);
        return;
    }
    if (pc.sessionTurns.empty()) {
        pc.statusNote = "·";
        updatePreedit(ic, state);
        return;
    }
    pc.loadingMore = true;
    pc.statusNote.clear();
    activeIc_ = ic->watch();
    client_.startMoreCandidates(pc.query, pc.sessionTurns, pc.sentRev);
    updatePreedit(ic, state);
    schedulePollTimer(true);
}

void YunTypeAddon::commitHighlightedCandidate(fcitx::InputContext *ic, YunTypeState *state) {
    if (!ic || !state || !state->pendingCandidates_) {
        return;
    }
    const auto &pc = *state->pendingCandidates_;
    if (pc.candidates.empty()) {
        if (pc.loadingMore) {
            client_.cancelSentence(pc.query);
        }
        state->pendingCandidates_.reset();
        updatePreedit(ic, state);
        pollQueue();
        return;
    }
    if (auto candList = ic->inputPanel().candidateList()) {
        const int cur = candList->cursorIndex();
        if (cur >= 0 && cur < candList->size()) {
            candList->candidate(cur).select(ic);
            return;
        }
    }
    std::size_t idx = 0;
    if (pc.globalCursor >= 0 &&
        static_cast<std::size_t>(pc.globalCursor) < pc.candidates.size()) {
        idx = static_cast<std::size_t>(pc.globalCursor);
    }
    commitPendingCandidate(ic, idx);
}

void YunTypeAddon::commitPendingCandidate(fcitx::InputContext *ic, std::size_t index) {
    if (!ic) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    if (!state || !state->pendingCandidates_) {
        return;
    }
    PendingCandidates pc = std::move(*state->pendingCandidates_);
    state->pendingCandidates_.reset();
    if (pc.loadingMore) {
        client_.cancelSentence(pc.query);
    }
    if (pc.candidates.empty()) {
        updatePreedit(ic, state);
        pollQueue();
        return;
    }
    if (index >= pc.candidates.size()) {
        index = 0;
    }
    const std::string llmOriginal = pc.candidates.front();
    std::string committed = std::move(pc.candidates[index]);
    updatePreedit(ic, state);
    commitSentenceResult(ic, state, std::move(committed), {}, pc.sentRev, llmOriginal, true);
    pollQueue();
}

void YunTypeAddon::schedulePollTimer(bool active) {
    if (active) {
        const uint64_t at = fcitx::now(CLOCK_MONOTONIC) + kPollIntervalUsec;
        if (!pollTimer_) {
            pollTimer_ = instance_->eventLoop().addTimeEvent(
                CLOCK_MONOTONIC, at, 1000,
                [this](fcitx::EventSourceTime *, uint64_t) {
                    pollQueue();
                    return true;
                });
        } else {
            pollTimer_->setTime(at);
            pollTimer_->setEnabled(true);
        }
        pollTimer_->setOneShot();
    } else if (pollTimer_) {
        pollTimer_->setEnabled(false);
    }
}

void YunTypeAddon::pollQueue() {
    auto *ic = activeIc_.get();
    if (!ic) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);

    if (state->pendingCandidates_ && state->pendingCandidates_->loadingMore) {
        auto &pc = *state->pendingCandidates_;
        Snapshot snap = client_.pollSentence();
        if (snap.source != pc.query) {
            pc.loadingMore = false;
            pc.statusNote = "!";
            updatePreedit(ic, state);
        } else if (snap.state == JobState::Running) {
            schedulePollTimer(true);
            return;
        } else if (snap.state == JobState::Idle || snap.state == JobState::Error) {
            pc.loadingMore = false;
            pc.statusNote = "!";
            updatePreedit(ic, state);
        } else {
            pc.loadingMore = false;
            ++pc.loadMoreRounds;
            if (!snap.turnJson.empty()) {
                pc.sessionTurns += snap.turnJson;
                pc.sessionTurns.push_back('\n');
            }
            const std::size_t oldCount = pc.candidates.size();
            for (auto &c : snap.candidates) {
                if (!c.empty() &&
                    std::find(pc.candidates.begin(), pc.candidates.end(), c) ==
                        pc.candidates.end()) {
                    pc.candidates.push_back(std::move(c));
                }
            }
            if (pc.candidates.size() > oldCount) {
                pc.globalCursor = static_cast<int>(oldCount);
                pc.statusNote.clear();
            } else {
                pc.statusNote = "·";
            }
            updatePreedit(ic, state);
        }
        schedulePollTimer(false);
        return;
    }

    while (!state->queue_.empty()) {
        if (state->pendingCandidates_) {
            break;
        }
        auto &front = state->queue_.front();
        if (front.error) {
            break;
        }
        std::string turnJson;
        std::uint64_t sentRev = 0;
        bool candidateMode = false;
        std::vector<std::string> candidates;
        if (!front.ready) {
            if (!front.started) {
                front.started = true;
                const bool useCandidates = shouldRequestCandidates(client_.config(), front.query);
                client_.startSentence(front.query, front.fallback,
                                      buildHistoryContext(ic, state, useCandidates));
            }
            Snapshot snap = client_.pollSentence();
            if (snap.source != front.query) {
                front.error = "转换任务已中断";
                break;
            }
            if (snap.state == JobState::Running) {
                break;
            }
            if (snap.state == JobState::Idle || snap.state == JobState::Error) {
                front.error = snap.error.empty() ? "转换任务已中断" : snap.error;
                break;
            }
            front.ready = snap.text;
            candidates = std::move(snap.candidates);
            turnJson = std::move(snap.turnJson);
            sentRev = snap.revision;
            candidateMode = snap.candidateMode;
        }

        if (candidates.size() > 1) {
            SubmittedSentence item = std::move(front);
            state->queue_.pop_front();
            std::string initialTurns;
            if (!turnJson.empty()) {
                initialTurns = std::move(turnJson);
                initialTurns.push_back('\n');
            }
            state->pendingCandidates_ = PendingCandidates{
                std::move(item.query),
                std::move(item.fallback),
                item.erasedFromApp,
                std::move(candidates),
                sentRev,
                0,
                std::move(initialTurns),
                0,
                false,
                {},
            };
            break;
        }

        std::string committed = std::move(*front.ready);
        state->queue_.pop_front();
        updatePreedit(ic, state);
        commitSentenceResult(ic, state, std::move(committed),
                             candidateMode ? std::string{} : std::move(turnJson), sentRev, {},
                             candidateMode);
    }

    updatePreedit(ic, state);

    const bool running =
        (state->pendingCandidates_ && state->pendingCandidates_->loadingMore) ||
        (!state->pendingCandidates_ && !state->queue_.empty() && !state->queue_.front().error);
    schedulePollTimer(running);
}

void YunTypeAddon::updatePreedit(fcitx::InputContext *ic, YunTypeState *state) {
    auto &panel = ic->inputPanel();
    panel.reset();

    if (!state->hasActiveWork()) {
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    std::string shown;
    if (state->pendingCandidates_) {
        shown += state->pendingCandidates_->query;
        if (state->pendingCandidates_->loadingMore) {
            shown += "…";
        } else if (!state->pendingCandidates_->statusNote.empty()) {
            shown += state->pendingCandidates_->statusNote;
        }
    }
    for (const auto &item : state->queue_) {
        if (!shown.empty()) {
            shown.push_back(' ');
        }
        shown += item.fallback;
        if (!item.error) {
            shown += "…";
        }
    }
    const std::size_t prefixLen = shown.size();
    shown += state->draft_;
    const std::size_t cursor = state->draftActive_ ? (prefixLen + state->draftCursor_) : shown.size();

    fcitx::Text preedit;
    if (shown.empty() && state->draftActive_) {
        // Visual cue when draft mode is entered before typing any letters
        panel.setAuxUp(fcitx::Text("整句拼音：输入后按回车转换，Esc 取消"));
    } else {
        preedit.append(shown, fcitx::TextFormatFlag::Underline);
        preedit.setCursor(static_cast<int>(cursor));
    }

    if (state->pendingCandidates_ && !state->pendingCandidates_->candidates.empty()) {
        const auto &pc = *state->pendingCandidates_;
        const int pageSize = std::clamp(*config_.candidatePageSize, 2, 10);
        auto list = std::make_unique<fcitx::CommonCandidateList>();
        list->setPageSize(pageSize);
        list->setSelectionKey(candidateSelectionKeys(pageSize));
        list->setCursorIncludeUnselected(false);
        list->setCursorKeepInSamePage(false);
        list->setCursorPositionAfterPaging(fcitx::CursorPositionAfterPaging::ResetToFirst);
        for (std::size_t i = 0; i < pc.candidates.size(); ++i) {
            list->append<YunTypeCandidateWord>(this, i, pc.candidates[i]);
        }
        const int total = static_cast<int>(pc.candidates.size());
        const int clampedCursor = std::clamp(pc.globalCursor, 0, total - 1);
        list->setPage(clampedCursor / pageSize);
        list->setCursorIndex(clampedCursor % pageSize);
        panel.setCandidateList(std::move(list));
    }

    if (!state->queue_.empty() && state->queue_.front().error) {
        panel.setAuxDown(fcitx::Text("✗ " + *state->queue_.front().error +
                                     "（回车用原拼音，Esc 取回修改）"));
    }

    if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit)) {
        panel.setClientPreedit(preedit);
    } else {
        panel.setPreedit(preedit);
    }
    ic->updatePreedit();
    ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
}

void YunTypeAddon::scheduleCompaction() {
    if (!*config_.historyEnabled) {
        return;
    }
    const std::size_t budget = client_.historyBudget();
    const std::size_t trigger = budget - budget / 4;
    const std::size_t limit = static_cast<std::size_t>(std::max(1, *config_.historyCount));
    if (!history_.compacting() && !history_.needsCompaction(limit, trigger)) {
        return;
    }
    const uint64_t at = fcitx::now(CLOCK_MONOTONIC) + kCompactionIntervalUsec;
    if (!compactionTimer_) {
        compactionTimer_ = instance_->eventLoop().addTimeEvent(
            CLOCK_MONOTONIC, at, 1000,
            [this](fcitx::EventSourceTime *source, uint64_t) {
                pollCompaction();
                const std::size_t b = client_.historyBudget();
                const std::size_t t = b - b / 4;
                const std::size_t l = static_cast<std::size_t>(std::max(1, *config_.historyCount));
                if (history_.compacting() || history_.needsCompaction(l, t)) {
                    source->setNextInterval(kCompactionIntervalUsec);
                    source->setOneShot();
                }
                return true;
            });
    } else if (!compactionTimer_->isEnabled()) {
        compactionTimer_->setTime(at);
        compactionTimer_->setOneShot();
    }
}

void YunTypeAddon::pollCompaction() {
    if (history_.compactionStale()) {
        client_.cancelCompaction();
        history_.deferCompaction();
    }
    if (history_.compacting()) {
        auto [state, summary] = client_.pollCompaction();
        if (state == JobState::Running) {
            return;
        }
        if (state == JobState::Idle) {
            history_.deferCompaction();
        } else {
            historyCompactedAt_ = ++historyRevision_;
            history_.finishCompaction(state == JobState::Done ? summary.c_str() : nullptr,
                                      historyCompactedAt_);
            markHistoryDirty();
            client_.cancelCompaction();
        }
    }
    const std::size_t budget = client_.historyBudget();
    const std::size_t trigger = budget - budget / 4;
    const std::size_t limit = static_cast<std::size_t>(std::max(1, *config_.historyCount));
    if (history_.needsCompaction(limit, trigger)) {
        std::string batch = history_.beginCompaction(budget);
        client_.startCompaction(batch, history_.summary());
    }
}

class YunTypeModuleFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new YunTypeAddon(manager->instance());
    }
};

} // namespace fcitx5_yuntype

FCITX_ADDON_FACTORY(fcitx5_yuntype::YunTypeModuleFactory);
