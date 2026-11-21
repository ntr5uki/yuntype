#include "llm_sentence.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>

#include <fcitx-utils/log.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx-utils/utf8.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputmethodentry.h>
#include <fcitx/inputpanel.h>

namespace fcitx5_llm {

namespace {

constexpr uint64_t kPollIntervalUsec = 40000;      // 40 ms
constexpr uint64_t kCompactionIntervalUsec = 100000; // 100 ms
constexpr uint64_t kOwnKeysResetUsec = 300000;     // 300 ms
constexpr std::size_t kMaxAsciiTrackBytes = 512;

bool hasAsciiAlpha(std::string_view s) {
    for (unsigned char c : s) {
        if (std::isalpha(c)) {
            return true;
        }
    }
    return false;
}

bool isAllAsciiGraphicOrSpace(std::string_view s) {
    if (s.empty()) {
        return false;
    }
    for (unsigned char c : s) {
        if (c < 0x20 || c > 0x7E) {
            return false;
        }
    }
    return true;
}

bool hasCommandModifier(fcitx::KeyStates states) {
    return states.testAny(fcitx::KeyStates{
        fcitx::KeyState::Ctrl, fcitx::KeyState::Alt, fcitx::KeyState::Super});
}

std::string defaultKeyFilePath() {
    if (const char *env = std::getenv("FCITX5_LLM_SENTENCE_KEY_FILE"); env && *env) {
        return env;
    }
    if (const char *xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg) {
        return std::string(xdg) + "/fcitx5-llm-sentence/api.key";
    }
    if (const char *home = std::getenv("HOME"); home && *home) {
        return std::string(home) + "/.config/fcitx5-llm-sentence/api.key";
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

} // namespace

std::string extractReplaceablePinyinSuffix(std::string_view buffer) {
    if (buffer.empty()) {
        return {};
    }
    // Find the last command/quote boundary so typing `git commit -m "ni mingtian jian`
    // only replaces `ni mingtian jian` after the opening quote.
    constexpr std::string_view kBoundaries = "\"'`([{=|><$&\\/";
    std::size_t start = 0;
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        if (kBoundaries.find(buffer[i]) != std::string_view::npos) {
            start = i + 1;
        }
    }
    // Skip leading spaces after a boundary (unless the entire buffer started with spaces)
    while (start > 0 && start < buffer.size() && buffer[start] == ' ') {
        ++start;
    }
    std::string_view candidate = buffer.substr(start);
    if (!hasAsciiAlpha(candidate)) {
        return {};
    }
    return std::string(candidate);
}

class LlmSentenceAddon::DbusControl : public fcitx::dbus::ObjectVTable<DbusControl> {
public:
    explicit DbusControl(LlmSentenceAddon *addon) : addon_(addon) {}

    void clearHistory() { addon_->clearHistory(); }
    void reloadConfig() { addon_->reloadConfig(); }
    std::string version() { return FCITX5_LLM_SENTENCE_VERSION; }

private:
    FCITX_OBJECT_VTABLE_METHOD(clearHistory, "ClearHistory", "", "");
    FCITX_OBJECT_VTABLE_METHOD(reloadConfig, "ReloadConfig", "", "");
    FCITX_OBJECT_VTABLE_METHOD(version, "Version", "", "s");
    LlmSentenceAddon *addon_;
};

LlmSentenceAddon::LlmSentenceAddon(fcitx::Instance *instance)
    : instance_(instance),
      factory_([](fcitx::InputContext &) { return new LlmSentenceState; }) {
    instance_->inputContextManager().registerProperty("llmSentenceState", &factory_);
    reloadConfig();

    try {
        auto bus = std::make_unique<fcitx::dbus::Bus>(fcitx::dbus::BusType::Session);
        if (bus->isOpen()) {
            bus->attachEventLoop(&instance_->eventLoop());
            auto ctrl = std::make_unique<DbusControl>(this);
            if (bus->addObjectVTable("/org/fcitx/LlmSentence", "org.fcitx.Fcitx.LlmSentence1", *ctrl) &&
                bus->requestName("org.fcitx.Fcitx.LlmSentence",
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

LlmSentenceAddon::~LlmSentenceAddon() = default;

void LlmSentenceAddon::reloadConfig() {
    fcitx::readAsIni(config_, "conf/llm_sentence.conf");
    applyConfig();
}

void LlmSentenceAddon::setConfig(const fcitx::RawConfig &config) {
    config_.load(config, true);
    fcitx::safeSaveAsIni(config_, "conf/llm_sentence.conf");
    applyConfig();
}

void LlmSentenceAddon::applyConfig() {
    LlmConfig llmCfg;
    llmCfg.baseUrl = *config_.baseUrl;
    llmCfg.model = *config_.model;
    llmCfg.keyFile = defaultKeyFilePath();
    llmCfg.prompt = *config_.sentencePrompt;
    if (const char *envUrl = std::getenv("FCITX5_LLM_SENTENCE_URL"); envUrl && *envUrl) {
        llmCfg.baseUrl = envUrl;
    }
    if (const char *envModel = std::getenv("FCITX5_LLM_SENTENCE_MODEL"); envModel && *envModel) {
        llmCfg.model = envModel;
    }
    client_.configure(llmCfg);

    if (!*config_.historyEnabled) {
        clearHistory();
    } else {
        scheduleCompaction();
    }
}

void LlmSentenceAddon::clearHistory() {
    client_.cancelCompaction();
    history_ = {};
    historyClearedAt_ = ++historyRevision_;
    notesSentThrough_ = 0;
    instance_->inputContextManager().foreach([this](fcitx::InputContext *ic) {
        auto *state = ic->propertyFor(&factory_);
        state->historyDraft_.reset();
        state->historyRevision_ = 0;
        return true;
    });
}

bool LlmSentenceAddon::isTranslatable(fcitx::InputContext *ic) const {
    return ic && !ic->capabilityFlags().testAny(fcitx::CapabilityFlags{
                     fcitx::CapabilityFlag::PasswordOrSensitive});
}

bool LlmSentenceAddon::resetFromOwnKeys(fcitx::InputContext *ic) const {
    return ic && ownKeysIc_.get() == ic && ownKeysAtUsec_ != 0 &&
           fcitx::now(CLOCK_MONOTONIC) - ownKeysAtUsec_ < kOwnKeysResetUsec;
}

void LlmSentenceAddon::sendAppKeys(fcitx::InputContext *ic, fcitx::KeySym sym, std::size_t count) {
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

void LlmSentenceAddon::onPreKeyEvent(fcitx::KeyEvent &event) {
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

void LlmSentenceAddon::onPostKeyEvent(fcitx::KeyEvent &event) {
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
        }
        return;
    }

    if (sym >= 0x20 && sym <= 0x7E) {
        state->appAscii_.push_back(static_cast<char>(sym));
        if (state->appAscii_.size() > kMaxAsciiTrackBytes) {
            state->appAscii_.erase(0, state->appAscii_.size() - kMaxAsciiTrackBytes);
        }
    } else if (sym == FcitxKey_BackSpace) {
        if (!state->appAscii_.empty()) {
            state->appAscii_.pop_back();
        }
    } else if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter ||
               sym == FcitxKey_Left || sym == FcitxKey_Right ||
               sym == FcitxKey_Up || sym == FcitxKey_Down ||
               sym == FcitxKey_Home || sym == FcitxKey_End ||
               sym == FcitxKey_Page_Up || sym == FcitxKey_Page_Down ||
               sym == FcitxKey_Tab || sym == FcitxKey_Delete ||
               sym == FcitxKey_Escape) {
        state->appAscii_.clear();
    }
}

void LlmSentenceAddon::onCommitString(fcitx::CommitStringEvent &event) {
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
    } else {
        state->appAscii_.clear();
        if (*config_.historyEnabled && isTranslatable(ic) && !text.empty()) {
            history_.recordCommit(state->historyDraft_, *ic, text, nullptr, ++historyRevision_);
            scheduleCompaction();
        }
    }
}

void LlmSentenceAddon::onSurroundingUpdated(fcitx::SurroundingTextUpdatedEvent &event) {
    auto *ic = event.inputContext();
    if (!ic || !*config_.historyEnabled || !isTranslatable(ic) || !ic->hasFocus()) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    if (history_.update(state->historyDraft_, *ic, ++historyRevision_)) {
        if (history_.compactionStale()) {
            client_.cancelCompaction();
            history_.deferCompaction();
        }
    }
}

void LlmSentenceAddon::onFocusOut(fcitx::FocusOutEvent &event) {
    auto *ic = event.inputContext();
    if (!ic) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    state->appAscii_.clear();
    state->imePreeditAscii_.clear();
    if (state->historyDraft_) {
        state->historyDraft_->awaiting = true;
    }
    if (state->hasActiveWork()) {
        if (!state->queue_.empty() && state->queue_.front().started) {
            client_.cancelSentence(state->queue_.front().query);
        }
        state->queue_.clear();
        state->draft_.clear();
        state->draftCursor_ = 0;
        state->draftActive_ = false;
        updatePreedit(ic, state);
    }
}

void LlmSentenceAddon::onReset(fcitx::ResetEvent &event) {
    auto *ic = event.inputContext();
    if (!ic || resetFromOwnKeys(ic)) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    state->appAscii_.clear();
    state->imePreeditAscii_.clear();
    if (state->hasActiveWork()) {
        if (!state->queue_.empty() && state->queue_.front().started) {
            client_.cancelSentence(state->queue_.front().query);
        }
        state->queue_.clear();
        state->draft_.clear();
        state->draftCursor_ = 0;
        state->draftActive_ = false;
        updatePreedit(ic, state);
    }
}

bool LlmSentenceAddon::handleTriggerKey(fcitx::InputContext *ic, LlmSentenceState *state,
                                        fcitx::KeyEvent &event) {
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

        // 4. If user typed ASCII pinyin directly into the terminal/app (e.g. English mode)
        if (std::string suffix = extractReplaceablePinyinSuffix(state->appAscii_); !suffix.empty()) {
            const std::size_t charCount = fcitx::utf8::length(suffix);
            state->appAscii_.erase(state->appAscii_.size() - suffix.size());
            sendAppKeys(ic, FcitxKey_BackSpace, charCount);
            submitSentence(ic, state, std::move(suffix), true);
            return true;
        }
    }

    // 5. Otherwise, enter standalone preedit draft mode
    state->draftActive_ = true;
    state->draft_.clear();
    state->draftCursor_ = 0;
    updatePreedit(ic, state);
    return true;
}

bool LlmSentenceAddon::handleActiveKey(fcitx::InputContext *ic, LlmSentenceState *state,
                                       fcitx::KeyEvent &event) {
    const fcitx::Key key = event.key();
    if (hasCommandModifier(key.states())) {
        return false;
    }
    const auto sym = key.sym();
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

void LlmSentenceAddon::submitSentence(fcitx::InputContext *ic, LlmSentenceState *state,
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

SentenceContext LlmSentenceAddon::buildHistoryContext(fcitx::InputContext *ic,
                                                      LlmSentenceState *state) {
    if (!*config_.historyEnabled || !isTranslatable(ic)) {
        return {};
    }
    if (history_.update(state->historyDraft_, *ic, ++historyRevision_)) {
        if (history_.compactionStale()) {
            client_.cancelCompaction();
            history_.deferCompaction();
        }
    }
    const std::size_t budget = client_.historyBudget();
    const std::size_t corrBudget = std::max<std::size_t>(budget / 5, 256);
    if (!client_.config().isLoopback()) {
        std::string turns = history_.replayTurns(budget);
        const std::uint64_t after = turns.empty() ? 0 : notesSentThrough_;
        return SentenceContext{
            std::move(turns),
            history_.newText(budget, after),
            history_.summary(),
            history_.corrections(corrBudget, after),
            historyRevision_,
        };
    }
    return SentenceContext{
        {},
        history_.forRequest(budget),
        history_.summary(),
        history_.corrections(corrBudget),
        historyRevision_,
    };
}

void LlmSentenceAddon::pollQueue() {
    auto *ic = activeIc_.get();
    if (!ic) {
        return;
    }
    auto *state = ic->propertyFor(&factory_);
    while (!state->queue_.empty()) {
        auto &front = state->queue_.front();
        if (front.error) {
            break;
        }
        std::string turnJson;
        std::uint64_t sentRev = 0;
        if (!front.ready) {
            if (!front.started) {
                front.started = true;
                client_.startSentence(front.query, front.fallback, buildHistoryContext(ic, state));
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
            turnJson = std::move(snap.turnJson);
            sentRev = snap.revision;
        }

        std::string committed = std::move(*front.ready);
        state->queue_.pop_front();
        updatePreedit(ic, state);

        internalCommit_ = true;
        if (*config_.historyEnabled && isTranslatable(ic) &&
            (turnJson.empty() || sentRev >= historyClearedAt_)) {
            if (!turnJson.empty() && sentRev >= historyCompactedAt_) {
                notesSentThrough_ = std::max(notesSentThrough_, sentRev);
            }
            history_.recordCommit(state->historyDraft_, *ic, committed,
                                  turnJson.empty() ? nullptr : turnJson.c_str(),
                                  ++historyRevision_);
            scheduleCompaction();
        }
        ic->commitString(committed);
        internalCommit_ = false;
    }

    updatePreedit(ic, state);

    const bool running = !state->queue_.empty() && !state->queue_.front().error;
    if (running) {
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

void LlmSentenceAddon::updatePreedit(fcitx::InputContext *ic, LlmSentenceState *state) {
    auto &panel = ic->inputPanel();
    panel.reset();

    if (!state->hasActiveWork()) {
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        return;
    }

    std::string shown;
    for (const auto &item : state->queue_) {
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

void LlmSentenceAddon::scheduleCompaction() {
    if (!*config_.historyEnabled) {
        return;
    }
    const std::size_t budget = client_.historyBudget();
    const std::size_t trigger = client_.config().isLoopback() ? budget : (budget - budget / 4);
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
                const std::size_t t = client_.config().isLoopback() ? b : (b - b / 4);
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

void LlmSentenceAddon::pollCompaction() {
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
            history_.finishCompaction(state == JobState::Done ? summary.c_str() : nullptr);
            historyCompactedAt_ = ++historyRevision_;
            notesSentThrough_ = 0;
            client_.cancelCompaction();
        }
    }
    const std::size_t budget = client_.historyBudget();
    const std::size_t trigger = client_.config().isLoopback() ? budget : (budget - budget / 4);
    const std::size_t limit = static_cast<std::size_t>(std::max(1, *config_.historyCount));
    if (history_.needsCompaction(limit, trigger)) {
        std::string batch = history_.beginCompaction(budget);
        client_.startCompaction(batch, history_.summary());
    }
}

class LlmSentenceModuleFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new LlmSentenceAddon(manager->instance());
    }
};

} // namespace fcitx5_llm

FCITX_ADDON_FACTORY(fcitx5_llm::LlmSentenceModuleFactory);
