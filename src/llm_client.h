#ifndef FCITX5_YUNTYPE_CLIENT_H
#define FCITX5_YUNTYPE_CLIENT_H

#include "../core/llm_client.h"

namespace fcitx5_yuntype {

using yuntype::JobState;
using yuntype::LlmClient;
using yuntype::LlmConfig;
using yuntype::SentenceContext;
using yuntype::Snapshot;
using yuntype::buildCompactionRequestJson;
using yuntype::buildMoreCandidatesRequestJson;
using yuntype::buildSentenceRequestJson;
using yuntype::extractJsonError;
using yuntype::extractSseDelta;
using yuntype::extraRequestFieldsFragment;
using yuntype::jsonEscape;
using yuntype::jsonQuote;
using yuntype::parseCandidates;
using yuntype::parseTurnPair;
using yuntype::shouldRequestCandidates;
using yuntype::stripThinkAndClean;

} // namespace fcitx5_yuntype

#endif
