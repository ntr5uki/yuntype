#ifndef FCITX5_LLM_SENTENCE_CLIENT_H
#define FCITX5_LLM_SENTENCE_CLIENT_H

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace fcitx5_llm {

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
    std::string prompt;

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

    bool empty() const {
        return turns.empty() && history.empty() && summary.empty() && corrections.empty();
    }
};

struct Snapshot {
    JobState state = JobState::Idle;
    std::string source;
    std::string text;
    std::string error;
    std::string turnJson; // JSON array: [user_msg, assistant_msg]
    std::uint64_t revision = 0;
};

// JSON helpers exposed for unit tests
std::string jsonEscape(std::string_view s);
std::string jsonQuote(std::string_view s);
std::pair<std::string, std::string> parseTurnPair(std::string_view line);
std::string extractSseDelta(std::string_view line);
std::string extractJsonError(std::string_view body);
std::string stripThinkAndClean(std::string_view raw, bool finished);
std::string buildSentenceRequestJson(const LlmConfig &config, std::string_view source,
                                     const SentenceContext &ctx, std::string *outUserContent = nullptr);
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

} // namespace fcitx5_llm

#endif
