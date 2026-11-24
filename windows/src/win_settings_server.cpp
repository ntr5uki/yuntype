#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <bcrypt.h>
#include <shellapi.h>

#include "win_settings_server.h"
#include "win_settings_html.h"
#include "win_utf8.h"

#include "../../core/llm_client.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

namespace yuntype::win {

namespace {

std::string generateRandomHexToken() {
    unsigned char buf[16]{};
    if (::BCryptGenRandom(nullptr, buf, sizeof(buf), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        const std::uint64_t t = monotonicNowUsec() ^ static_cast<std::uint64_t>(::GetCurrentProcessId());
        std::memcpy(buf, &t, sizeof(t));
    }
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (unsigned char b : buf) {
        out.push_back(kHex[b >> 4]);
        out.push_back(kHex[b & 0x0F]);
    }
    return out;
}

void sendHttpResponse(SOCKET s, int status, const char *statusText,
                      const char *contentType, std::string_view body) {
    std::string header = "HTTP/1.1 " + std::to_string(status) + " " + statusText +
                         "\r\nContent-Type: " + contentType +
                         "\r\nContent-Length: " + std::to_string(body.size()) +
                         "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n";
    ::send(s, header.data(), static_cast<int>(header.size()), 0);
    if (!body.empty()) {
        ::send(s, body.data(), static_cast<int>(body.size()), 0);
    }
}

std::string runModelTestJson(std::string_view profileJson) {
    // Wrap single profile object into temporary AppConfig to reuse parser
    AppConfig tmp;
    std::string wrapped = "{\"active\":\"\",\"profiles\":[" + std::string(profileJson) + "]}";
    if (!deserializeModelsJson(wrapped, tmp) || tmp.models.empty()) {
        return "{\"error\":\"参数格式无效\"}";
    }
    const auto &p = tmp.models.front();
    yuntype::LlmConfig cfg;
    cfg.baseUrl = p.baseUrl;
    cfg.model = p.model;
    cfg.apiKey = p.apiKey;
    cfg.thinkingMode = p.thinkingMode.empty() ? "default" : p.thinkingMode;
    cfg.extraRequestJson = p.extraRequestJson;
    cfg.shortCandidateThreshold = 0; // Test full sentence output cleanly
    if (!cfg.isReady()) {
        return "{\"error\":\"请先填写接口地址和模型名称\"}";
    }
    if (cfg.thinkingMode == "custom" && !cfg.extraRequestJson.empty() &&
        yuntype::extraRequestFieldsFragment(cfg).empty()) {
        return "{\"error\":\"自定义额外请求 JSON 格式无效，须为合法 JSON 对象\"}";
    }

    yuntype::LlmClient client;
    client.configure(cfg);
    const auto start = std::chrono::steady_clock::now();
    client.startSentence("nihao shijie", "nihao shijie", yuntype::SentenceContext{});
    while (true) {
        const auto snap = client.pollSentence();
        if (snap.state == yuntype::JobState::Done) {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
            return "{\"output\":" + yuntype::jsonQuote(snap.text) +
                   ",\"ms\":" + std::to_string(ms) + "}";
        }
        if (snap.state == yuntype::JobState::Error) {
            return "{\"error\":" + yuntype::jsonQuote(snap.error.empty() ? "请求失败" : snap.error) +
                   "}";
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(
                                 std::chrono::steady_clock::now() - start)
                                 .count();
        if (elapsed > 30) {
            client.cancelSentence();
            return "{\"error\":\"测试超时（30 秒）\"}";
        }
        ::Sleep(30);
    }
}

std::string runFetchModelsJson(std::string_view reqJson) {
    AppConfig tmp;
    std::string wrapped = "{\"active\":\"\",\"profiles\":[" + std::string(reqJson) + "]}";
    if (!deserializeModelsJson(wrapped, tmp) || tmp.models.empty()) {
        return "{\"error\":\"参数格式无效\"}";
    }
    const auto &p = tmp.models.front();
    yuntype::LlmConfig cfg;
    cfg.baseUrl = p.baseUrl;
    cfg.apiKey = p.apiKey;
    const auto fetched = yuntype::fetchAvailableModels(cfg);
    if (!fetched.ok) {
        return "{\"error\":" +
               yuntype::jsonQuote(fetched.error.empty() ? "获取模型列表失败" : fetched.error) +
               "}";
    }
    std::string out = "{\"models\":[";
    for (std::size_t i = 0; i < fetched.models.size(); ++i) {
        if (i > 0) {
            out.push_back(',');
        }
        out += yuntype::jsonQuote(fetched.models[i]);
    }
    out += "]}";
    return out;
}

void launchBrowserWindow(const std::wstring &url) {
    // Prefer Edge --app mode for a clean native-like settings window; fallback to default browser.
    const std::wstring edgeArgs = L"--app=" + url + L" --window-size=860,820";
    HINSTANCE res = ::ShellExecuteW(nullptr, L"open", L"msedge.exe", edgeArgs.c_str(), nullptr,
                                    SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(res) <= 32) {
        ::ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
}

} // namespace

SettingsServer::~SettingsServer() {
    stop();
}

bool SettingsServer::openSettingsUi(Callbacks callbacks) {
    callbacks_ = std::move(callbacks);
    if (!running_.load()) {
        WSADATA wsa{};
        ::WSAStartup(MAKEWORD(2, 2), &wsa);

        SOCKET ls = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        if (ls == INVALID_SOCKET) {
            return false;
        }
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);
        addr.sin_port = 0; // OS assigns ephemeral port
        if (::bind(ls, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) == SOCKET_ERROR ||
            ::listen(ls, 8) == SOCKET_ERROR) {
            ::closesocket(ls);
            return false;
        }
        int addrLen = sizeof(addr);
        if (::getsockname(ls, reinterpret_cast<sockaddr *>(&addr), &addrLen) == SOCKET_ERROR) {
            ::closesocket(ls);
            return false;
        }
        port_ = ::ntohs(addr.sin_port);
        token_ = generateRandomHexToken();
        listenSock_ = static_cast<std::uintptr_t>(ls);
        running_.store(true);
        thread_ = std::thread(&SettingsServer::serveLoop, this);
    }

    const std::wstring url =
        L"http://127.0.0.1:" + std::to_wstring(port_) + L"/?token=" + utf8ToWide(token_);
    launchBrowserWindow(url);
    return true;
}

void SettingsServer::stop() {
    if (!running_.exchange(false)) {
        return;
    }
    if (listenSock_ != static_cast<std::uintptr_t>(-1)) {
        ::closesocket(static_cast<SOCKET>(listenSock_));
        listenSock_ = static_cast<std::uintptr_t>(-1);
    }
    if (thread_.joinable()) {
        thread_.join();
    }
}

void SettingsServer::serveLoop() {
    while (running_.load()) {
        SOCKET ls = static_cast<SOCKET>(listenSock_);
        SOCKET client = ::accept(ls, nullptr, nullptr);
        if (client == INVALID_SOCKET) {
            break;
        }
        std::thread([this, client]() {
            handleClient(static_cast<std::uintptr_t>(client));
        }).detach();
    }
}

void SettingsServer::handleClient(std::uintptr_t sockVal) {
    SOCKET s = static_cast<SOCKET>(sockVal);
    DWORD timeoutMs = 5000;
    ::setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char *>(&timeoutMs),
                 sizeof(timeoutMs));

    std::string req;
    char buf[4096];
    std::size_t headerEnd = std::string::npos;
    std::size_t contentLength = 0;

    while (req.size() < (1U << 20)) {
        const int n = ::recv(s, buf, sizeof(buf), 0);
        if (n <= 0) {
            break;
        }
        req.append(buf, static_cast<std::size_t>(n));
        if (headerEnd == std::string::npos) {
            headerEnd = req.find("\r\n\r\n");
            if (headerEnd != std::string::npos) {
                std::string_view hdr(req.data(), headerEnd);
                std::size_t pos = 0;
                while (pos < hdr.size()) {
                    std::size_t eol = hdr.find("\r\n", pos);
                    if (eol == std::string_view::npos) eol = hdr.size();
                    std::string_view line = hdr.substr(pos, eol - pos);
                    pos = eol + 2;
                    if (line.size() > 15) {
                        std::string prefix(line.substr(0, 15));
                        for (char &c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                        if (prefix == "content-length:") {
                            contentLength = static_cast<std::size_t>(
                                std::strtoull(std::string(line.substr(15)).c_str(), nullptr, 10));
                        }
                    }
                }
            }
        }
        if (headerEnd != std::string::npos && req.size() >= headerEnd + 4 + contentLength) {
            break;
        }
    }

    if (headerEnd == std::string::npos) {
        ::closesocket(s);
        return;
    }

    const std::string_view headerPart(req.data(), headerEnd);
    const std::string body =
        req.substr(headerEnd + 4, std::min(contentLength, req.size() - (headerEnd + 4)));

    const std::size_t lineEnd = headerPart.find("\r\n");
    const std::string_view reqLine =
        lineEnd == std::string_view::npos ? headerPart : headerPart.substr(0, lineEnd);
    const std::size_t sp1 = reqLine.find(' ');
    const std::size_t sp2 = sp1 == std::string_view::npos ? std::string_view::npos : reqLine.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) {
        sendHttpResponse(s, 400, "Bad Request", "text/plain", "bad request");
        ::closesocket(s);
        return;
    }
    const std::string_view method = reqLine.substr(0, sp1);
    const std::string_view target = reqLine.substr(sp1 + 1, sp2 - sp1 - 1);
    const std::size_t qpos = target.find('?');
    const std::string_view path = qpos == std::string_view::npos ? target : target.substr(0, qpos);
    const std::string_view query = qpos == std::string_view::npos ? std::string_view{} : target.substr(qpos + 1);

    auto hasValidToken = [&]() -> bool {
        if (query.find("token=" + token_) != std::string_view::npos) {
            return true;
        }
        std::size_t pos = 0;
        while (pos < headerPart.size()) {
            std::size_t eol = headerPart.find("\r\n", pos);
            if (eol == std::string_view::npos) eol = headerPart.size();
            std::string_view line = headerPart.substr(pos, eol - pos);
            pos = eol + 2;
            if (line.size() > 16) {
                std::string prefix(line.substr(0, 16));
                for (char &c : prefix) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
                if (prefix == "x-yuntype-token:") {
                    std::string_view val = line.substr(16);
                    while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.remove_prefix(1);
                    if (val == token_) {
                        return true;
                    }
                }
            }
        }
        return false;
    };

    if (!hasValidToken()) {
        sendHttpResponse(s, 403, "Forbidden", "application/json", "{\"error\":\"forbidden\"}");
        ::closesocket(s);
        return;
    }

    if (method == "GET" && path == "/") {
        sendHttpResponse(s, 200, "OK", "text/html; charset=utf-8", kSettingsPageHtml);
    } else if (method == "GET" && path == "/api/state") {
        const std::string json = callbacks_.getStateJson ? callbacks_.getStateJson() : "{}";
        sendHttpResponse(s, 200, "OK", "application/json; charset=utf-8", json);
    } else if (method == "POST" && path == "/api/save") {
        if (callbacks_.saveStateJson) {
            auto [ok, resp] = callbacks_.saveStateJson(body);
            sendHttpResponse(s, ok ? 200 : 400, ok ? "OK" : "Bad Request",
                             "application/json; charset=utf-8", resp);
        } else {
            sendHttpResponse(s, 500, "Error", "application/json", "{\"error\":\"unavailable\"}");
        }
    } else if (method == "POST" && path == "/api/test") {
        const std::string res = runModelTestJson(body);
        const bool isErr = (res.find("\"error\"") != std::string::npos);
        sendHttpResponse(s, isErr ? 400 : 200, isErr ? "Bad Request" : "OK",
                         "application/json; charset=utf-8", res);
    } else if (method == "POST" && path == "/api/models") {
        const std::string res = runFetchModelsJson(body);
        const bool isErr = (res.find("\"error\"") != std::string::npos);
        sendHttpResponse(s, isErr ? 400 : 200, isErr ? "Bad Request" : "OK",
                         "application/json; charset=utf-8", res);
    } else if (method == "POST" && path == "/api/clear-history") {
        if (callbacks_.clearHistory) {
            callbacks_.clearHistory();
        }
        sendHttpResponse(s, 200, "OK", "application/json", "{\"ok\":true}");
    } else {
        sendHttpResponse(s, 404, "Not Found", "application/json", "{\"error\":\"not found\"}");
    }
    ::closesocket(s);
}

} // namespace yuntype::win
