#ifndef FCITX5_YUNTYPE_HISTORY_H
#define FCITX5_YUNTYPE_HISTORY_H

#include <algorithm>
#include <optional>
#include <string>
#include <string_view>

#include <fcitx/inputcontext.h>
#include <fcitx-utils/utf8.h>

#include "../core/sentence_history.h"

namespace fcitx5_yuntype {

using yuntype::SurroundingSnapshot;

class SentenceHistory : public yuntype::SentenceHistory {
public:
    using yuntype::SentenceHistory::recordCommit;
    using yuntype::SentenceHistory::recordShortCommit;
    using yuntype::SentenceHistory::update;

    static std::optional<SurroundingSnapshot> readFcitx(const fcitx::InputContext &ic) {
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
        return SurroundingSnapshot{text, offset(start), offset(end)};
    }

    void recordCommit(std::optional<Draft> &draft, const fcitx::InputContext &ic,
                      const std::string &commit, const char *turn, std::uint64_t revision) {
        yuntype::SentenceHistory::recordCommit(draft, readFcitx(ic), commit, turn, revision);
    }

    void recordCommit(std::optional<Draft> &draft, std::optional<ShadowDraft> &shadow,
                      const fcitx::InputContext &ic, const std::string &commit,
                      const char *turn, std::uint64_t revision,
                      std::uint64_t nowUsec,
                      std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec,
                      std::string_view llmOriginal = {}) {
        yuntype::SentenceHistory::recordCommit(draft, shadow, readFcitx(ic), commit, turn,
                                               revision, nowUsec, shadowTimeoutUsec, llmOriginal);
    }

    bool recordShortCommit(std::optional<Draft> &draft, std::optional<ShadowDraft> &shadow,
                           const fcitx::InputContext &ic, const std::string &commit,
                           std::uint64_t revision, std::uint64_t nowUsec,
                           std::uint64_t shadowTimeoutUsec = kDefaultShadowTimeoutUsec,
                           std::string_view llmOriginal = {}) {
        return yuntype::SentenceHistory::recordShortCommit(
            draft, shadow, readFcitx(ic), commit, revision, nowUsec, shadowTimeoutUsec,
            llmOriginal);
    }

    bool update(std::optional<Draft> &draft, const fcitx::InputContext &ic,
                std::uint64_t revision) {
        return yuntype::SentenceHistory::update(draft, readFcitx(ic), revision);
    }
};

} // namespace fcitx5_yuntype

#endif
