#ifndef YUNTYPE_WIN_APP_H
#define YUNTYPE_WIN_APP_H

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../../core/llm_client.h"
#include "../../core/sentence_history.h"
#include "win_caret.h"
#include "win_config.h"
#include "win_overlay.h"
#include "win_settings_server.h"

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace yuntype::win {

struct SubmittedSentence {
    std::string query;
    std::string fallback;
    std::size_t pendingEraseChars = 0;
    std::string pendingEraseSuffix;
    bool replaceSelection = false;
    bool started = false;
    std::optional<std::string> ready;
    std::optional<std::string> error;
};

struct PendingCandidates {
    std::string query;
    std::string fallback;
    std::size_t pendingEraseChars = 0;
    std::string pendingEraseSuffix;
    bool replaceSelection = false;
    std::vector<std::string> candidates;
    std::uint64_t sentRev = 0;
    int globalCursor = 0;
    std::string sessionTurns;
    int loadMoreRounds = 0;
    bool loadingMore = false;
    std::string statusNote;
};

class WinApp {
public:
    WinApp() = default;
    ~WinApp();

    WinApp(const WinApp &) = delete;
    WinApp &operator=(const WinApp &) = delete;

    int run(HINSTANCE hInst);

private:
    static WinApp *instance_;
    static LRESULT CALLBACK ctrlWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK lowLevelKeyboardProc(int code, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK lowLevelMouseProc(int code, WPARAM wp, LPARAM lp);
    static void CALLBACK winEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject,
                                      LONG idChild, DWORD dwEventThread, DWORD dwmsEventTime);

    bool initControllerWindow(HINSTANCE hInst);
    bool initTrayIcon();
    void removeTrayIcon();
    void showTrayMenu();
    void openSettings();

    void reloadConfigAndSync();
    void loadHistoryFromDisk();
    void flushHistoryToDisk();
    void markHistoryDirty();
    void onHistoryMutated();
    void clearHistory();

    bool isTriggerMatch(DWORD vk) const;
    bool hasActiveSession() const;
    char vkToPrintableAscii(DWORD vk, bool shift, bool caps) const;

    void onTrigger();
    void onActiveKey(DWORD vk, bool shift, bool ctrl, bool alt, char asciiCh);
    void onPassiveKey(DWORD vk, bool shift, bool ctrl, bool alt, char asciiCh);
    void onForegroundChanged(HWND newFg);
    void onCaptureStart(HWND hwnd);
    void onMouseClick(POINT pt);
    void saveWindowAscii(HWND hwnd);
    void syncMouseHook(bool active);
    void syncPollTimer();
    void syncCompactionTimer();
    void schedulePersistTimer();
    void schedulePostCommitSync();
    void cancelPostCommitSync();
    void onPersistTimer();
    void onPostCommitSyncTimer();

    void handleDraftKey(DWORD vk, bool ctrl, char asciiCh);
    void handleQueueKey(DWORD vk, char asciiCh);
    void handleCandidateKey(DWORD vk, bool shift, bool ctrl, char asciiCh);

    void submitSentence(std::string query, std::size_t pendingEraseChars,
                        std::string pendingEraseSuffix, bool replaceSelection = false);
    void startLoadMoreCandidates();
    void commitPendingCandidate(int globalIndex);
    void commitSentenceResult(std::string committed, std::string turnJson,
                              std::uint64_t sentRev, std::string_view llmOriginal,
                              bool candidateMode, std::size_t eraseChars,
                              std::string_view eraseSuffix, bool replaceSelection = false);

    yuntype::SentenceContext buildHistoryContext(bool candidateMode);
    void pollQueue();
    void pollCompaction();
    void refreshOverlay();
    void resetActiveSession();

    HINSTANCE hInst_ = nullptr;
    HWND ctrlHwnd_ = nullptr;
    HICON trayIcon_ = nullptr;
    HHOOK kbHook_ = nullptr;
    HHOOK mouseHook_ = nullptr;
    HWINEVENTHOOK fgHook_ = nullptr;
    HWINEVENTHOOK captureHook_ = nullptr;

    std::string configDir_;
    std::string dataDir_;
    std::string historyPath_;

    mutable std::mutex configMutex_;
    AppConfig config_;
    TriggerSpec triggerSpec_;

    std::atomic<bool> enabledAtomic_{true};
    std::atomic<bool> activeSessionAtomic_{false};
    std::atomic<std::uint64_t> lastTriggerTickMs_{0};

    CaretTracker caret_;
    CandidateOverlay overlay_;
    SettingsServer settingsServer_;
    yuntype::LlmClient client_;
    yuntype::SentenceHistory history_;

    std::optional<yuntype::SentenceHistory::Draft> historyDraft_;
    std::optional<yuntype::SentenceHistory::ShadowDraft> shadowDraft_;
    std::uint64_t historyRevision_ = 0;
    std::uint64_t notesSentThrough_ = 0;
    std::uint64_t historyClearedAt_ = 0;
    std::uint64_t historyCompactedAt_ = 0;
    bool historyDirty_ = false;
    std::uint64_t historyDirtySinceUsec_ = 0;
    std::uint64_t historyLastMutatedUsec_ = 0;

    struct SavedWindowAscii {
        std::string text;
        std::uint64_t savedUsec = 0;
    };

    HWND lastFgWnd_ = nullptr;
    std::uint64_t lastFgChangedUsec_ = 0;
    RECT activeAnchor_{};

    std::string appAscii_;
    std::unordered_map<HWND, SavedWindowAscii> savedWindowAscii_;
    bool selectionLikely_ = false;
    bool mouseCaptureSeen_ = false;
    bool pollTimerActive_ = false;
    bool compactionTimerActive_ = false;
    bool persistTimerActive_ = false;
    bool postCommitTimerActive_ = false;
    bool draftActive_ = false;
    std::string draft_;
    std::size_t draftCursor_ = 0;
    std::size_t draftEraseChars_ = 0;
    std::string draftEraseSuffix_;
    bool draftReplaceSelection_ = false;
    std::deque<SubmittedSentence> queue_;
    std::optional<PendingCandidates> pendingCandidates_;
    std::uint64_t lastCommitUsec_ = 0;
    std::string lastCommittedText_;
};

} // namespace yuntype::win

#endif
