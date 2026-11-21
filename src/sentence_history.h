#ifndef FCITX5_LLM_SENTENCE_HISTORY_H
#define FCITX5_LLM_SENTENCE_HISTORY_H

#include <algorithm>
#include <cstdint>
#include <deque>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <fcitx/inputcontext.h>
#include <fcitx-utils/utf8.h>

namespace fcitx5_llm {

/// Shared history. Each input context owns its own editable range and record ID.
class SentenceHistory {
public:
    struct Draft {
        std::uint64_t recordId;
        std::string left, right, text;
        bool awaiting;
    };

    void recordCommit(std::optional<Draft> &draft, const fcitx::InputContext &ic,
                      const std::string &commit, const char *turn, std::uint64_t revision) {
        const auto id = nextId_;
        if (const auto snapshot = read(ic)) {
            const auto &[text, start, end] = *snapshot;
            const auto body = editableBody(draft, text);
            if (!turn && body && *body == draft->text && start >= draft->left.size() &&
                end <= text.size() - draft->right.size()) {
                if (auto *record = find(draft->recordId)) {
                    const auto offset = start - draft->left.size();
                    draft->text.replace(offset, end - start, commit);
                    if (record->correction.empty() && record->original == record->text) {
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
        }
        records_.push_back({nextId_++, commit, commit, {}, turn ? turn : "", turn ? 0 : revision});
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

    void finishCompaction(const char *summary) {
        if (summary && *summary) {
            summary_ = summary;
        }
        while (pending_ > 0) {
            if (!records_.front().correction.empty()) {
                archivedCorrections_.push_back(std::move(records_.front().correction));
                if (archivedCorrections_.size() > 300) {
                    archivedCorrections_.pop_front();
                }
            }
            records_.pop_front();
            --pending_;
        }
    }

    void deferCompaction() {
        pending_ = 0;
        pendingStale_ = false;
    }

    bool update(std::optional<Draft> &draft, const fcitx::InputContext &ic, std::uint64_t revision) {
        const auto snapshot = read(ic);
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
            if (it->noteRevision > after && it->correction.empty()) {
                prependWithinBudget(text, it->text, budget);
            }
        }
        return text;
    }

    const std::string &summary() const { return summary_; }

    std::string corrections(std::size_t budget, std::uint64_t after = 0) const {
        // Select newest entries that fit, then put them back in chronological order.
        std::string text;
        for (auto it = records_.rbegin(); it != records_.rend(); ++it) {
            if (it->noteRevision > after) {
                prependWithinBudget(text, it->correction, budget);
            }
        }
        if (after == 0) {
            for (auto it = archivedCorrections_.rbegin(); it != archivedCorrections_.rend(); ++it) {
                prependWithinBudget(text, *it, budget);
            }
        }
        return text;
    }

private:
    static void prependWithinBudget(std::string &text, const std::string &line, std::size_t budget) {
        if (!line.empty() && text.size() + line.size() + 1 <= budget) {
            text.insert(0, line + (text.empty() ? "" : "\n"));
        }
    }

    struct Record {
        std::uint64_t id;
        std::string text, original, correction, turn;
        std::uint64_t noteRevision;

        void revise(std::string_view body) {
            text.assign(body);
            const auto old = fcitx::utf8::MakeUTF8StringViewRange(original);
            const auto updated = fcitx::utf8::MakeUTF8StringViewRange(text);
            const std::vector<std::string_view> before(old.begin(), old.end());
            const std::vector<std::string_view> after(updated.begin(), updated.end());
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
            correction.clear();
            if (!removed || !inserted || removed > 32 || inserted > 32) {
                return;
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
            correction = "「" + fragment(before, removed) + "」→「" + fragment(after, inserted) + "」";
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
    struct Snapshot {
        std::string_view text;
        std::size_t start, end;
    };
    std::deque<Record> records_;
    std::deque<std::string> archivedCorrections_;
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

    static std::optional<Snapshot> read(const fcitx::InputContext &ic) {
        const auto &surrounding = ic.surroundingText();
        if (!ic.capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) ||
            !surrounding.isValid()) {
            return std::nullopt;
        }
        const auto &text = surrounding.text();
        const auto start = std::min(surrounding.cursor(), surrounding.anchor());
        const auto end = std::max(surrounding.cursor(), surrounding.anchor());
        const auto offset = [&text](unsigned pos) -> std::size_t {
            return pos == 0 ? 0 : fcitx::utf8::ncharByteLength(text.begin(), pos);
        };
        return Snapshot{text, offset(start), offset(end)};
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

} // namespace fcitx5_llm
#endif
