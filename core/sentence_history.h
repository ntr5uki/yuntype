#ifndef YUNTYPE_CORE_SENTENCE_HISTORY_H
#define YUNTYPE_CORE_SENTENCE_HISTORY_H

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <process.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "utf8_util.h"

namespace yuntype {

/// Cross-platform surrounding-text snapshot (UTF-8 text + UTF-8 byte offsets [start, end]).
struct SurroundingSnapshot {
    std::string text;
    std::size_t start = 0;
    std::size_t end = 0;
};

/// Shared history. Each input context owns its own editable range and record ID.
class SentenceHistory {
public:
    struct Draft {
        std::uint64_t recordId;
        std::string left, right, text;
        bool awaiting;
    };

    /// Shadow buffer fallback for clients without SurroundingText (e.g. terminals).
    /// Tracks immediate local edits (BackSpace/Delete/Left/Right + re-input) on the
    /// latest committed sentence so corrections are still recorded accurately.
    struct ShadowDraft {
        std::uint64_t recordId = 0;
        std::string text;
        std::size_t cursor = 0; // UTF-8 aligned byte offset in `text`
        bool edited = false;
        bool deletedSinceLastCommit = false;
        std::size_t deletedChars = 0;
        std::size_t insertedChars = 0;
        std::uint64_t lastActivityUsec = 0;
    };

    static constexpr std::uint64_t kDefaultShadowTimeoutUsec = 30ULL * 1000 * 1000;

    void recordCommit(std::optional<Draft> &draft,
                      const std::optional<SurroundingSnapshot> &snapshot,
                      const std::string &commit, const char *turn, std::uint64_t revision) {
        std::optional<ShadowDraft> unusedShadow;
        recordCommit(draft, unusedShadow, snapshot, commit, turn, revision, 0, 0);
    }

    void recordCommit(std::optional<Draft> &draft, std::optional<ShadowDraft> &shadow,
                      const std::optional<SurroundingSnapshot> &snapshot,
                      const std::string &commit, const char *turn, std::uint64_t revision,
                      std::uint64_t nowUsec,
                      std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec,
                      std::string_view llmOriginal = {}) {
        const auto id = nextId_;
        if (snapshot) {
            shadow.reset();
            const std::string_view text = snapshot->text;
            const std::size_t start = std::min(snapshot->start, text.size());
            const std::size_t end = std::min(std::max(snapshot->end, start), text.size());
            const auto body = editableBody(draft, text);
            if (!turn && body && *body == draft->text && start >= draft->left.size() &&
                end <= text.size() - draft->right.size()) {
                if (auto *record = find(draft->recordId)) {
                    const auto offset = start - draft->left.size();
                    const bool replacedSelection = end > start;
                    draft->text.replace(offset, end - start, commit);
                    if (!replacedSelection && record->turn.empty() &&
                        record->correction.empty() && record->original == record->text) {
                        record->original = draft->text;
                    }
                    draft->awaiting = true;
                    revise(*record, draft->text, revision);
                    return;
                }
            }
            draft = Draft{id, std::string(text.substr(0, start)), std::string(text.substr(end)), commit, true};
        } else {
            draft.reset();
            if (auto *record = activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec)) {
                const bool isLlmTurn = (turn != nullptr);
                bool canMerge = isLlmTurn
                    ? (shadow->edited && shadow->deletedSinceLastCommit)
                    : shadow->edited;
                if (canMerge && !isLlmTurn && shadow->cursor == shadow->text.size() &&
                    shadow->insertedChars > 0 && shadow->insertedChars >= shadow->deletedChars) {
                    const std::size_t commitChars = utf8Length(commit);
                    if (endsWithSentencePunct(shadow->text) ||
                        shadow->insertedChars + commitChars > 32) {
                        canMerge = false;
                    }
                }
                if (canMerge) {
                    shadow->text.insert(shadow->cursor, commit);
                    shadow->cursor += commit.size();
                    shadow->insertedChars += utf8Length(commit);
                    shadow->deletedSinceLastCommit = false;
                    shadow->lastActivityUsec = nowUsec;
                    if (isLlmTurn) {
                        shadow->edited = false;
                        shadow->deletedChars = 0;
                        shadow->insertedChars = 0;
                    }
                    if (!shadow->text.empty()) {
                        revise(*record, shadow->text, revision);
                    }
                    return;
                }
            }
            if (!commit.empty()) {
                shadow = ShadowDraft{id, commit, commit.size(), false, false, 0, 0, nowUsec};
            } else {
                shadow.reset();
            }
        }
        const bool hasTurn = (turn != nullptr && *turn != '\0');
        records_.push_back({nextId_++, commit, commit, {}, hasTurn ? turn : "", hasTurn ? 0 : revision});
        if (!llmOriginal.empty() && llmOriginal != commit) {
            auto &rec = records_.back();
            rec.original = std::string(llmOriginal);
            rec.noteRevision = revision;
            rec.revise(commit);
        }
    }

    /// Record a short-input candidate commit as correction-only:
    /// - If editing an active GUI draft or terminal shadow draft of a previous sentence,
    ///   merges into that sentence and updates its correction.
    /// - Otherwise, does NOT push a new sentence record into `records_`; only records
    ///   a correction entry when the user picked a non-first candidate (`llmOriginal != commit`).
    bool recordShortCommit(std::optional<Draft> &draft, std::optional<ShadowDraft> &shadow,
                           const std::optional<SurroundingSnapshot> &snapshot,
                           const std::string &commit, std::uint64_t revision,
                           std::uint64_t nowUsec,
                           std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec,
                           std::string_view llmOriginal = {}) {
        if (snapshot) {
            shadow.reset();
            const std::string_view text = snapshot->text;
            const std::size_t start = std::min(snapshot->start, text.size());
            const std::size_t end = std::min(std::max(snapshot->end, start), text.size());
            const auto body = editableBody(draft, text);
            if (body && *body == draft->text && start >= draft->left.size() &&
                end <= text.size() - draft->right.size()) {
                if (auto *record = find(draft->recordId)) {
                    const auto offset = start - draft->left.size();
                    const bool replacedSelection = end > start;
                    draft->text.replace(offset, end - start, commit);
                    if (!replacedSelection && record->turn.empty() &&
                        record->correction.empty() && record->original == record->text) {
                        record->original = draft->text;
                    }
                    draft->awaiting = true;
                    revise(*record, draft->text, revision);
                    return true;
                }
            }
            draft.reset();
        } else {
            draft.reset();
            if (auto *record = activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec)) {
                if (shadow->edited && shadow->deletedSinceLastCommit) {
                    shadow->text.insert(shadow->cursor, commit);
                    shadow->cursor += commit.size();
                    shadow->insertedChars += utf8Length(commit);
                    shadow->deletedSinceLastCommit = false;
                    shadow->edited = false;
                    shadow->deletedChars = 0;
                    shadow->insertedChars = 0;
                    shadow->lastActivityUsec = nowUsec;
                    if (!shadow->text.empty()) {
                        revise(*record, shadow->text, revision);
                    }
                    return true;
                }
            }
            shadow.reset();
        }
        if (!llmOriginal.empty() && llmOriginal != commit) {
            std::string corr = Record::makeCorrection(llmOriginal, commit);
            if (!corr.empty()) {
                pushArchivedCorrection(std::move(corr), revision);
                return true;
            }
        }
        return false;
    }

    void touchShadow(std::optional<ShadowDraft> &shadow, std::uint64_t nowUsec,
                     std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec) {
        if (activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec)) {
            shadow->lastActivityUsec = nowUsec;
        }
    }

    bool shadowBackspace(std::optional<ShadowDraft> &shadow, std::uint64_t revision,
                         std::uint64_t nowUsec,
                         std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec) {
        auto *record = activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec);
        if (!record) {
            return false;
        }
        if (shadow->cursor == 0) {
            shadow.reset();
            return false;
        }
        const std::size_t prev = prevUtf8Char(shadow->text, shadow->cursor);
        shadow->text.erase(prev, shadow->cursor - prev);
        shadow->cursor = prev;
        shadow->edited = true;
        shadow->deletedSinceLastCommit = true;
        ++shadow->deletedChars;
        shadow->lastActivityUsec = nowUsec;
        if (shadow->text.empty()) {
            return revertShadowRecord(*record, revision);
        }
        return reviseIfChanged(*record, shadow->text, revision);
    }

    bool shadowDelete(std::optional<ShadowDraft> &shadow, std::uint64_t revision,
                      std::uint64_t nowUsec,
                      std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec) {
        auto *record = activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec);
        if (!record) {
            return false;
        }
        if (shadow->cursor >= shadow->text.size()) {
            shadow.reset();
            return false;
        }
        const std::size_t next = nextUtf8Char(shadow->text, shadow->cursor);
        shadow->text.erase(shadow->cursor, next - shadow->cursor);
        shadow->edited = true;
        shadow->deletedSinceLastCommit = true;
        ++shadow->deletedChars;
        shadow->lastActivityUsec = nowUsec;
        if (shadow->text.empty()) {
            return revertShadowRecord(*record, revision);
        }
        return reviseIfChanged(*record, shadow->text, revision);
    }

    void shadowMoveLeft(std::optional<ShadowDraft> &shadow, std::uint64_t nowUsec,
                        std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec) {
        if (!activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec)) {
            return;
        }
        if (shadow->cursor == 0) {
            shadow.reset();
            return;
        }
        shadow->cursor = prevUtf8Char(shadow->text, shadow->cursor);
        shadow->edited = true;
        shadow->deletedSinceLastCommit = true;
        shadow->lastActivityUsec = nowUsec;
    }

    void shadowMoveRight(std::optional<ShadowDraft> &shadow, std::uint64_t nowUsec,
                         std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec) {
        if (!activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec)) {
            return;
        }
        if (shadow->cursor >= shadow->text.size()) {
            shadow.reset();
            return;
        }
        shadow->cursor = nextUtf8Char(shadow->text, shadow->cursor);
        shadow->lastActivityUsec = nowUsec;
    }

    bool shadowInsertAscii(std::optional<ShadowDraft> &shadow, std::string_view ascii,
                           std::uint64_t revision, std::uint64_t nowUsec,
                           std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec) {
        auto *record = activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec);
        if (!record || ascii.empty()) {
            return false;
        }
        if (!shadow->edited || !shadow->deletedSinceLastCommit) {
            shadow.reset();
            return false;
        }
        if (shadow->text.size() + ascii.size() > 1024) {
            shadow.reset();
            return false;
        }
        shadow->text.insert(shadow->cursor, ascii);
        shadow->cursor += ascii.size();
        shadow->insertedChars += ascii.size();
        shadow->lastActivityUsec = nowUsec;
        return reviseIfChanged(*record, shadow->text, revision);
    }

    bool shadowEraseSuffixBeforeCursor(std::optional<ShadowDraft> &shadow, std::string_view suffix,
                                       std::uint64_t revision, std::uint64_t nowUsec,
                                       std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec) {
        auto *record = activeShadowRecord(shadow, nowUsec, shadowTimeoutUsec);
        if (!record || suffix.empty()) {
            return false;
        }
        if (shadow->cursor < suffix.size() ||
            std::string_view(shadow->text).substr(shadow->cursor - suffix.size(), suffix.size()) !=
                suffix) {
            shadow.reset();
            return false;
        }
        shadow->text.erase(shadow->cursor - suffix.size(), suffix.size());
        shadow->cursor -= suffix.size();
        const std::size_t suffixChars = utf8Length(suffix);
        shadow->insertedChars =
            shadow->insertedChars > suffixChars ? shadow->insertedChars - suffixChars : 0;
        shadow->lastActivityUsec = nowUsec;
        if (shadow->text.empty()) {
            return revertShadowRecord(*record, revision);
        }
        return reviseIfChanged(*record, shadow->text, revision);
    }

    void keepLast(std::size_t limit) {
        while (records_.size() > limit) {
            records_.pop_front();
        }
        summary_.clear();
        archivedCorrections_.clear();
        deferCompaction();
    }

    bool compacting() const { return pending_ != 0; }
    bool compactionStale() const { return pendingStale_; }

    bool needsCompaction(std::size_t limit, std::size_t budget) const {
        if (records_.size() < 2) {
            return false; // Keep the live draft available for application edits.
        }
        if (records_.size() > limit) {
            return true;
        }
        std::size_t bytes = summary_.size() + 2;
        for (const auto &record : records_) {
            bytes += std::max(record.text.size(), record.turn.size()) + 2;
            if (bytes > budget) {
                return true;
            }
        }
        return false;
    }

    std::string beginCompaction(std::size_t budget) {
        // Leave the newest record intact. Edits to this batch invalidate the snapshot.
        pending_ = std::min(records_.size() - 1, (records_.size() * 2 + 4) / 5);
        return collect(0, pending_, budget);
    }

    void finishCompaction(const char *summary, std::uint64_t revision = 1) {
        if (summary && *summary) {
            summary_ = summary;
        }
        for (auto &item : archivedCorrections_) {
            item.noteRevision = revision;
        }
        while (pending_ > 0) {
            if (!records_.front().correction.empty()) {
                pushArchivedCorrection(std::move(records_.front().correction), revision);
            }
            records_.pop_front();
            --pending_;
        }
    }

    void deferCompaction() {
        pending_ = 0;
        pendingStale_ = false;
    }

    bool update(std::optional<Draft> &draft,
                const std::optional<SurroundingSnapshot> &snapshot,
                std::uint64_t revision) {
        auto *record = draft ? find(draft->recordId) : nullptr;
        if (!record || !snapshot) {
            draft.reset();
            return false;
        }
        const auto body = editableBody(draft, snapshot->text);
        if (!body || body->empty()) {
            // A cleared field ends the editable draft. Its last text stays in history.
            draft.reset();
        } else if (*body == draft->text) {
            draft->awaiting = false;
        } else if (!draft->awaiting) {
            draft->text.assign(*body);
            revise(*record, *body, revision);
            return true;
        }
        return false;
    }

    std::string forRequest(std::size_t budget) const {
        return collect(0, records_.size(), budget);
    }

    std::string replayTurns(std::size_t budget) const {
        std::size_t bytes = 0;
        auto first = records_.end();
        while (first != records_.begin()) {
            const auto it = std::prev(first);
            if (bytes + it->turn.size() + 1 > budget) {
                break;
            }
            bytes += it->turn.size() + 1;
            first = it;
        }
        std::string turns;
        turns.reserve(bytes);
        for (auto it = first; it != records_.end(); ++it) {
            if (!it->turn.empty()) {
                turns += it->turn;
                turns.push_back('\n');
            }
        }
        return turns;
    }

    std::string newText(std::size_t budget, std::uint64_t after) const {
        std::string text;
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if ((after == 0 ? it->turn.empty() : it->noteRevision > after) &&
                it->correction.empty()) {
                prependWithinBudget(text, it->text, budget);
            }
        }
        return text;
    }

    const std::string &summary() const { return summary_; }

    bool empty() const {
        return records_.empty() && archivedCorrections_.empty() && summary_.empty();
    }

    std::string serializeJson(std::uint64_t sentThrough = 0) const {
        std::size_t pendingArchived = 0;
        for (auto it = archivedCorrections_.rbegin(); it != archivedCorrections_.rend(); ++it) {
            if (it->noteRevision > sentThrough) {
                ++pendingArchived;
            } else {
                break;
            }
        }
        std::string out;
        out.reserve(512);
        out += "{\"version\":1,\"notesTracked\":true,\"pendingArchived\":";
        out += std::to_string(pendingArchived);
        out += ",\"summary\":";
        out += quoteJson(summary_);
        out += ",\"archivedCorrections\":[";
        for (std::size_t i = 0; i < archivedCorrections_.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            out += quoteJson(archivedCorrections_[i].text);
        }
        out += "],\"records\":[";
        for (std::size_t i = 0; i < records_.size(); ++i) {
            if (i > 0) {
                out.push_back(',');
            }
            const auto &r = records_[i];
            const bool isPending =
                r.noteRevision > sentThrough && (!r.correction.empty() || r.turn.empty());
            out += "{\"text\":";
            out += quoteJson(r.text);
            out += ",\"original\":";
            out += quoteJson(r.original);
            out += ",\"correction\":";
            out += quoteJson(r.correction);
            out += ",\"turn\":";
            out += quoteJson(r.turn);
            if (isPending) {
                out += ",\"pending\":true";
            }
            out.push_back('}');
        }
        out += "]}\n";
        return out;
    }

    bool deserializeJson(std::string_view json) {
        JsonReader reader(json);
        if (!reader.consume('{')) {
            return false;
        }
        std::string newSummary;
        std::vector<std::string> newArchived;
        std::vector<Record> newRecords;
        std::vector<bool> newRecordPending;
        bool hasKnownField = false;
        bool notesTracked = false;
        std::size_t pendingArchived = 0;

        if (!reader.consume('}')) {
            while (true) {
                std::string key;
                if (!reader.readString(key) || !reader.consume(':')) {
                    return false;
                }
                if (key == "version") {
                    if (!reader.skipValue()) {
                        return false;
                    }
                    hasKnownField = true;
                } else if (key == "notesTracked") {
                    if (!reader.readBool(notesTracked)) {
                        return false;
                    }
                    hasKnownField = true;
                } else if (key == "pendingArchived") {
                    if (!reader.readUint(pendingArchived)) {
                        return false;
                    }
                    hasKnownField = true;
                } else if (key == "summary") {
                    if (!reader.readString(newSummary)) {
                        return false;
                    }
                    hasKnownField = true;
                } else if (key == "archivedCorrections") {
                    if (!reader.consume('[')) {
                        return false;
                    }
                    if (!reader.consume(']')) {
                        while (true) {
                            std::string item;
                            if (!reader.readString(item)) {
                                return false;
                            }
                            if (!item.empty()) {
                                newArchived.push_back(std::move(item));
                            }
                            if (reader.consume(']')) {
                                break;
                            }
                            if (!reader.consume(',')) {
                                return false;
                            }
                        }
                    }
                    hasKnownField = true;
                } else if (key == "records") {
                    if (!reader.consume('[')) {
                        return false;
                    }
                    if (!reader.consume(']')) {
                        while (true) {
                            if (!reader.consume('{')) {
                                return false;
                            }
                            Record rec{};
                            bool recPending = false;
                            if (!reader.consume('}')) {
                                while (true) {
                                    std::string rkey;
                                    if (!reader.readString(rkey) || !reader.consume(':')) {
                                        return false;
                                    }
                                    if (rkey == "text") {
                                        if (!reader.readString(rec.text)) return false;
                                    } else if (rkey == "original") {
                                        if (!reader.readString(rec.original)) return false;
                                    } else if (rkey == "correction") {
                                        if (!reader.readString(rec.correction)) return false;
                                    } else if (rkey == "turn") {
                                        if (!reader.readString(rec.turn)) return false;
                                    } else if (rkey == "pending") {
                                        if (!reader.readBool(recPending)) return false;
                                    } else {
                                        if (!reader.skipValue()) return false;
                                    }
                                    if (reader.consume('}')) {
                                        break;
                                    }
                                    if (!reader.consume(',')) {
                                        return false;
                                    }
                                }
                            }
                            if (rec.original.empty()) {
                                rec.original = rec.text;
                            }
                            if (!rec.text.empty() || !rec.turn.empty() || !rec.correction.empty()) {
                                newRecords.push_back(std::move(rec));
                                newRecordPending.push_back(recPending);
                            }
                            if (reader.consume(']')) {
                                break;
                            }
                            if (!reader.consume(',')) {
                                return false;
                            }
                        }
                    }
                    hasKnownField = true;
                } else {
                    if (!reader.skipValue()) {
                        return false;
                    }
                }
                if (reader.consume('}')) {
                    break;
                }
                if (!reader.consume(',')) {
                    return false;
                }
            }
        }
        if (!hasKnownField) {
            return false;
        }

        records_.clear();
        archivedCorrections_.clear();
        summary_ = std::move(newSummary);
        pending_ = 0;
        pendingStale_ = false;
        nextId_ = 1;

        for (auto &c : newArchived) {
            pushArchivedCorrection(std::move(c), 0);
        }

        constexpr std::size_t kMaxLoadedRecords = 300;
        const std::size_t startIdx =
            newRecords.size() > kMaxLoadedRecords ? newRecords.size() - kMaxLoadedRecords : 0;
        std::size_t lastTurnIdx = newRecords.size();
        for (std::size_t i = newRecords.size(); i > startIdx; --i) {
            if (!newRecords[i - 1].turn.empty()) {
                lastTurnIdx = i - 1;
                break;
            }
        }
        const bool hasAnyTurn = (lastTurnIdx < newRecords.size());

        for (std::size_t i = startIdx; i < newRecords.size(); ++i) {
            auto &r = newRecords[i];
            r.id = nextId_++;
            const bool canHaveNote = (!r.correction.empty() || r.turn.empty());
            if (notesTracked) {
                r.noteRevision = (newRecordPending[i] && canHaveNote) ? 2 : 0;
            } else if (hasAnyTurn) {
                // Legacy v1 file without delivery tracking: records at or before the last turn
                // were already included in prior turns; only trailing records stay pending.
                r.noteRevision = (i > lastTurnIdx && canHaveNote) ? 2 : 0;
            } else {
                r.noteRevision = canHaveNote ? 2 : 0;
            }
            records_.push_back(std::move(r));
        }

        if (notesTracked) {
            const std::size_t markCount = std::min(pendingArchived, archivedCorrections_.size());
            for (std::size_t i = archivedCorrections_.size() - markCount;
                 i < archivedCorrections_.size(); ++i) {
                archivedCorrections_[i].noteRevision = 2;
            }
        } else if (!hasAnyTurn) {
            for (auto &item : archivedCorrections_) {
                item.noteRevision = 2;
            }
        }
        return true;
    }

    static std::filesystem::path utf8ToFsPath(const std::string &path) {
        return std::filesystem::path(
            std::u8string(reinterpret_cast<const char8_t *>(path.data()), path.size()));
    }

    bool saveToFile(const std::string &path, std::uint64_t sentThrough = 0) const {
        if (path.empty()) {
            return false;
        }
        const std::filesystem::path target = utf8ToFsPath(path);
        if (empty()) {
            std::error_code ec;
            std::filesystem::remove(target, ec);
            return true;
        }
        std::error_code ec;
        if (const auto parent = target.parent_path(); !parent.empty()) {
            std::filesystem::create_directories(parent, ec);
            if (ec) {
                return false;
            }
        }
        const std::string data = serializeJson(sentThrough);
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
        const std::string tmp = path + ".tmp." + std::to_string(::getpid());
        int fd = ::open(tmp.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            return false;
        }
        ::fchmod(fd, 0600);
        std::size_t written = 0;
        while (written < data.size()) {
            const ssize_t n = ::write(fd, data.data() + written, data.size() - written);
            if (n < 0) {
                if (errno == EINTR) {
                    continue;
                }
                ::close(fd);
                ::unlink(tmp.c_str());
                return false;
            }
            written += static_cast<std::size_t>(n);
        }
        if (::close(fd) != 0) {
            ::unlink(tmp.c_str());
            return false;
        }
        if (::rename(tmp.c_str(), path.c_str()) != 0) {
            ::unlink(tmp.c_str());
            return false;
        }
        return true;
#endif
    }

    bool loadFromFile(const std::string &path) {
        if (path.empty()) {
            return false;
        }
        const std::filesystem::path target = utf8ToFsPath(path);
        std::error_code ec;
        const auto sz = std::filesystem::file_size(target, ec);
        if (ec || sz == 0 || sz > (2U << 20)) {
            return false;
        }
        std::ifstream in(target, std::ios::binary);
        if (!in) {
            return false;
        }
        std::ostringstream ss;
        ss << in.rdbuf();
        return deserializeJson(ss.str());
    }

    std::string corrections(std::size_t budget, std::uint64_t after = 0) const {
        // Select newest entries that fit, then put them back in chronological order.
        std::string text;
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if ((after == 0 || it->noteRevision > after) && !it->correction.empty()) {
                prependWithinBudget(text, it->correction, budget);
            }
        }
        for (auto it = archivedCorrections_.rbegin(); it != archivedCorrections_.rend(); ++it) {
            if (after == 0 || it->noteRevision > after) {
                prependWithinBudget(text, it->text, budget);
            }
        }
        return text;
    }

private:
    static constexpr std::size_t kMaxArchivedCorrections = 100;

    struct ArchivedCorrection {
        std::string text;
        std::uint64_t noteRevision = 0;
    };

    void pushArchivedCorrection(std::string correction, std::uint64_t noteRevision = 0) {
        if (correction.empty()) {
            return;
        }
        auto it = std::find_if(archivedCorrections_.begin(), archivedCorrections_.end(),
                               [&](const ArchivedCorrection &item) {
                                   return item.text == correction;
                               });
        if (it != archivedCorrections_.end()) {
            archivedCorrections_.erase(it);
        }
        archivedCorrections_.push_back(ArchivedCorrection{std::move(correction), noteRevision});
        if (archivedCorrections_.size() > kMaxArchivedCorrections) {
            archivedCorrections_.pop_front();
        }
    }

    static std::string quoteJson(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 2);
        out.push_back('"');
        for (unsigned char c : s) {
            switch (c) {
            case '\\':
                out += "\\\\";
                break;
            case '"':
                out += "\\\"";
                break;
            case '\n':
                out += "\\n";
                break;
            case '\r':
                out += "\\r";
                break;
            case '\t':
                out += "\\t";
                break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out.push_back(static_cast<char>(c));
                }
                break;
            }
        }
        out.push_back('"');
        return out;
    }

    struct JsonReader {
        std::string_view s;
        std::size_t pos = 0;

        explicit JsonReader(std::string_view sv) : s(sv) {}

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

        bool readUint(std::size_t &out) {
            skipWs();
            if (pos >= s.size() || s[pos] < '0' || s[pos] > '9') {
                return false;
            }
            std::size_t val = 0;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                const std::size_t digit = static_cast<std::size_t>(s[pos] - '0');
                if (val <= (std::size_t(-1) - digit) / 10) {
                    val = val * 10 + digit;
                }
                ++pos;
            }
            out = val;
            return true;
        }

        static bool parseHex4(std::string_view s, std::size_t &pos, unsigned int &out) {
            if (pos + 4 > s.size()) {
                return false;
            }
            unsigned int cp = 0;
            for (int i = 0; i < 4; ++i) {
                cp <<= 4;
                char h = s[pos++];
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
                char c = s[pos++];
                if (c == '"') {
                    return true;
                }
                if (c == '\\') {
                    if (pos >= s.size()) {
                        return false;
                    }
                    char esc = s[pos++];
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
                            std::size_t saved = pos;
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
                    std::string dummy;
                    if (!readString(dummy) || !consume(':') || !skipValue(depth + 1)) {
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
                ++pos;
                while (pos < s.size() &&
                       (std::isdigit(static_cast<unsigned char>(s[pos])) ||
                        s[pos] == '.' || s[pos] == 'e' || s[pos] == 'E' ||
                        s[pos] == '+' || s[pos] == '-')) {
                    ++pos;
                }
                return true;
            }
            return false;
        }
    };

    static void prependWithinBudget(std::string &text, const std::string &line, std::size_t budget) {
        if (!line.empty() && text.size() + line.size() + 1 <= budget) {
            text.insert(0, line + (text.empty() ? "" : "\n"));
        }
    }

    struct Record {
        std::uint64_t id;
        std::string text, original, correction, turn;
        std::uint64_t noteRevision;

        static std::string makeCorrection(std::string_view origText, std::string_view newText) {
            const std::vector<std::string_view> before = utf8SplitChars(origText);
            const std::vector<std::string_view> after = utf8SplitChars(newText);
            std::size_t prefix = 0;
            while (prefix < std::min(before.size(), after.size()) && before[prefix] == after[prefix]) {
                ++prefix;
            }
            std::size_t suffix = 0;
            while (suffix < std::min(before.size(), after.size()) - prefix &&
                   before[before.size() - suffix - 1] == after[after.size() - suffix - 1]) {
                ++suffix;
            }
            const auto removed = before.size() - prefix - suffix;
            const auto inserted = after.size() - prefix - suffix;
            if (!removed || !inserted || removed > 32 || inserted > 32) {
                return {};
            }
            const auto first = prefix - std::min(prefix, std::size_t{4});
            const auto tail = std::min(suffix, std::size_t{4});
            const auto fragment = [first, tail, prefix](const auto &chars, std::size_t changed) {
                std::string result;
                for (auto i = first; i < prefix + changed + tail; ++i) {
                    result += chars[i];
                }
                return result;
            };
            return "「" + fragment(before, removed) + "」→「" + fragment(after, inserted) + "」";
        }

        void revise(std::string_view body) {
            text.assign(body);
            correction = makeCorrection(original, text);
        }
    };

    std::string collect(std::size_t start, std::size_t end, std::size_t budget) const {
        std::size_t first = end;
        std::size_t bytes = 0;
        while (first > start && bytes + records_[first - 1].text.size() + 2 <= budget) {
            bytes += records_[--first].text.size() + 2;
        }
        if (first == end && end > start) {
            const auto &last = records_[end - 1].text;
            auto at = last.size() > budget ? last.size() - budget : 0;
            while (at < last.size() && (static_cast<unsigned char>(last[at]) & 0xc0) == 0x80) {
                ++at;
            }
            return last.substr(at);
        }
        std::string text;
        text.reserve(bytes);
        for (auto i = first; i < end; ++i) {
            if (!text.empty()) {
                text.push_back('\n');
            }
            text += records_[i].text;
        }
        return text;
    }
    std::deque<Record> records_;
    std::deque<ArchivedCorrection> archivedCorrections_;
    std::string summary_;
    std::uint64_t nextId_ = 1;
    std::size_t pending_ = 0;
    bool pendingStale_ = false;

    Record *find(std::uint64_t id) {
        if (records_.empty() || id < records_.front().id || id > records_.back().id) {
            return nullptr;
        }
        return &records_[id - records_.front().id];
    }

    void revise(Record &record, std::string_view text, std::uint64_t revision) {
        if (record.text != text) {
            pendingStale_ |= record.id - records_.front().id < pending_;
            record.noteRevision = revision;
            record.revise(text);
        }
    }

    bool reviseIfChanged(Record &record, std::string_view text, std::uint64_t revision) {
        if (record.text != text) {
            revise(record, text, revision);
            return true;
        }
        return false;
    }

    bool revertShadowRecord(Record &record, std::uint64_t revision) {
        if (record.text != record.original || !record.correction.empty()) {
            pendingStale_ |= record.id - records_.front().id < pending_;
            record.text = record.original;
            record.correction.clear();
            record.noteRevision = record.turn.empty() ? revision : 0;
            return true;
        }
        return false;
    }

    Record *activeShadowRecord(std::optional<ShadowDraft> &shadow,
                               std::uint64_t nowUsec,
                               std::uint64_t shadowTimeoutUsec) {
        if (!shadow) {
            return nullptr;
        }
        if (shadowTimeoutUsec != 0 && nowUsec > shadow->lastActivityUsec &&
            nowUsec - shadow->lastActivityUsec > shadowTimeoutUsec) {
            shadow.reset();
            return nullptr;
        }
        auto *record = find(shadow->recordId);
        if (!record) {
            shadow.reset();
            return nullptr;
        }
        return record;
    }

    static std::size_t prevUtf8Char(std::string_view s, std::size_t pos) {
        if (pos == 0) {
            return 0;
        }
        --pos;
        while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
            --pos;
        }
        return pos;
    }

    static std::size_t nextUtf8Char(std::string_view s, std::size_t pos) {
        if (pos >= s.size()) {
            return s.size();
        }
        ++pos;
        while (pos < s.size() && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80) {
            ++pos;
        }
        return pos;
    }

    static bool endsWithSentencePunct(std::string_view s) {
        if (s.empty()) {
            return false;
        }
        const char back = s.back();
        if (back == '.' || back == '!' || back == '?' || back == ';') {
            return true;
        }
        return s.ends_with("。") || s.ends_with("！") || s.ends_with("？") || s.ends_with("；");
    }

    static std::optional<std::string_view> editableBody(const std::optional<Draft> &draft,
                                                        std::string_view text) {
        if (!draft || text.size() < draft->left.size() + draft->right.size() ||
            !text.starts_with(draft->left) || !text.ends_with(draft->right)) {
            return std::nullopt;
        }
        return text.substr(draft->left.size(), text.size() - draft->left.size() - draft->right.size());
    }
};

} // namespace yuntype
#endif
