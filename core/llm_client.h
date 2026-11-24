#ifndef YUNTYPE_CORE_LLM_CLIENT_H
#define YUNTYPE_CORE_LLM_CLIENT_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace yuntype {

enum class JobState {
    Idle = 0,
    Running = 1,
    Done = 2,
    Error = 3,
};

struct LlmConfig {
    std::string baseUrl;
    std::string model;
    std::string keyFile;
    std::string apiKey; // Optional inline API key (takes precedence over keyFile when non-empty)
    std::string prompt;
    std::string thinkingMode;
    std::string extraRequestJson;
    int shortCandidateThreshold = 50;
    int candidatePageSize = 5;

    bool isReady() const;
    bool isLoopback() const;
    std::string endpoint() const;
    std::size_t historyBudget() const;

    bool operator==(const LlmConfig &other) const = default;
};

struct SentenceContext {
    std::string turns;
    std::string history;
    std::string summary;
    std::string corrections;
    std::uint64_t revision = 0;
    bool replayMode = false;

    bool empty() const {
        return !replayMode && turns.empty() && history.empty() && summary.empty() && corrections.empty();
    }
};

struct Snapshot {
    JobState state = JobState::Idle;
    std::string source;
    std::string text;
    std::vector<std::string> candidates;
    std::string error;
    std::string turnJson; // JSON array: [user_msg, assistant_msg]
    std::uint64_t revision = 0;
    bool candidateMode = false;
};

// JSON helpers exposed for unit tests
std::string jsonEscape(std::string_view s);
std::string jsonQuote(std::string_view s);
std::pair<std::string, std::string> parseTurnPair(std::string_view line);
std::string extractSseDelta(std::string_view line);
std::string extractJsonError(std::string_view body);
std::string stripThinkAndClean(std::string_view raw, bool finished);
bool shouldRequestCandidates(const LlmConfig &config, std::string_view source);
std::vector<std::string> parseCandidates(std::string_view cleaned, int maxCandidates = 20);
std::string extraRequestFieldsFragment(const LlmConfig &config, bool skipMaxTokens = false);
std::string buildSentenceRequestJson(const LlmConfig &config, std::string_view source,
                                     const SentenceContext &ctx, std::string *outUserContent = nullptr);
std::string buildMoreCandidatesRequestJson(const LlmConfig &config, std::string_view sessionTurns,
                                           std::string *outUserContent = nullptr);
std::string buildCompactionRequestJson(const LlmConfig &config, std::string_view batch,
                                       std::string_view previousSummary);

class LlmClient {
public:
    LlmClient();
    ~LlmClient();

    void configure(const LlmConfig &config);
    LlmConfig config() const;
    std::size_t historyBudget() const;

    void startSentence(const std::string &source, const std::string &fallback,
                       const SentenceContext &ctx);
    void startMoreCandidates(const std::string &source, const std::string &sessionTurns,
                             std::uint64_t revision);
    Snapshot pollSentence() const;
    void cancelSentence(const std::string &source = {});

    void startCompaction(const std::string &batch, const std::string &previousSummary);
    std::pair<JobState, std::string> pollCompaction() const;
    void cancelCompaction();

    struct JobBox;

private:
    std::shared_ptr<JobBox> sentence_;
    std::shared_ptr<JobBox> compaction_;
    mutable std::mutex configMutex_;
    LlmConfig config_;
};

} // namespace yuntype

#endif
