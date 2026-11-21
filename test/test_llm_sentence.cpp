#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/standardpath.h>
#include <fcitx/addonmanager.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>

#include "llm_client.h"
#include "llm_sentence.h"
#include "sentence_history.h"

using namespace fcitx5_llm;

#define CHECK(cond)                                                                           \
    do {                                                                                      \
        if (!(cond)) {                                                                        \
            std::cerr << "CHECK failed at " << __FILE__ << ":" << __LINE__ << ": " << #cond   \
                      << "\n";                                                                \
            std::abort();                                                                     \
        }                                                                                     \
    } while (0)

namespace {

void testPinyinSuffixExtraction() {
    CHECK(extractReplaceablePinyinSuffix("ni mingtian you shijian ma") ==
          "ni mingtian you shijian ma");
    CHECK(extractReplaceablePinyinSuffix("git commit -m \"xiufu zhengju shuru") ==
          "xiufu zhengju shuru");
    CHECK(extractReplaceablePinyinSuffix("echo 'women mingtian jian") ==
          "women mingtian jian");
    CHECK(extractReplaceablePinyinSuffix("12345 + 678") == "");
    CHECK(extractReplaceablePinyinSuffix("") == "");
}

void testJsonAndSseHelpers() {
    CHECK(jsonQuote("a\"b\nc") == "\"a\\\"b\\nc\"");
    auto [u, a] = parseTurnPair("[\"ni hao\",\"你好。\"]");
    CHECK(u == "ni hao");
    CHECK(a == "你好。");

    CHECK(extractSseDelta("data: {\"choices\":[{\"delta\":{\"content\":\"你好\"}}]}") == "你好");
    CHECK(extractSseDelta("data: [DONE]") == "");
    CHECK(stripThinkAndClean("<think>思考中</think>  我们明天见。 ", true) == "我们明天见。");
    CHECK(stripThinkAndClean("<think>还没结束", false) == "");
}

void testRequestBuilding() {
    LlmConfig cfg;
    cfg.baseUrl = "https://api.deepseek.com/v1";
    cfg.model = "deepseek-chat";
    cfg.prompt = "自定义提示词";

    SentenceContext ctx;
    ctx.summary = "项目青禾";
    ctx.turns = "[\"wangxiaoming\",\"王小明回来\"]\n";
    ctx.corrections = "「王小明回来」→「王晓明回来」";

    std::string userMsg;
    std::string body = buildSentenceRequestJson(cfg, "women mingtian jian", ctx, &userMsg);
    CHECK(body.find("\"model\":\"deepseek-chat\"") != std::string::npos);
    CHECK(body.find("自定义提示词") != std::string::npos);
    CHECK(body.find("项目青禾") != std::string::npos);
    CHECK(body.find("wangxiaoming") != std::string::npos);
    CHECK(body.find("王小明回来") != std::string::npos);
    CHECK(userMsg.find("women mingtian jian") != std::string::npos);
    CHECK(userMsg.find("王晓明回来") != std::string::npos);
}

class FakeInputContext : public fcitx::InputContext {
public:
    FakeInputContext(fcitx::InputContextManager &manager)
        : fcitx::InputContext(manager, "fake-terminal") {
        setCapabilityFlags(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Preedit});
        created();
    }
    ~FakeInputContext() override { destroy(); }

    const char *frontend() const override { return "fake"; }

    void commitStringImpl(const std::string &text) override {
        commits.push_back(text);
        terminalBuffer += text;
    }

    void deleteSurroundingTextImpl(int, unsigned int) override {}

    void forwardKeyImpl(const fcitx::ForwardKeyEvent &key) override {
        if (key.isRelease()) {
            return;
        }
        forwarded.push_back(key.rawKey());
        if (key.rawKey().sym() == FcitxKey_BackSpace && !terminalBuffer.empty()) {
            terminalBuffer.pop_back();
        }
    }

    void updatePreeditImpl() override {
        preedit = inputPanel().clientPreedit().toString();
    }

    bool sendKey(const fcitx::Key &k) {
        fcitx::KeyEvent press(this, k, false);
        bool accepted = keyEvent(press);
        fcitx::KeyEvent release(this, k, true);
        keyEvent(release);
        return accepted;
    }

    void typeAsciiToTerminal(const std::string &s) {
        for (char c : s) {
            fcitx::Key k(static_cast<fcitx::KeySym>(static_cast<unsigned char>(c)));
            if (!sendKey(k)) {
                terminalBuffer.push_back(c);
            }
        }
    }

    std::vector<std::string> commits;
    std::vector<fcitx::Key> forwarded;
    std::string terminalBuffer;
    std::string preedit;
};

void testEndToEndFcitx5Module() {
    namespace fs = std::filesystem;
    const fs::path tmp = fs::temp_directory_path() / ("fcitx5-llm-test-" + std::to_string(::getpid()));
    fs::create_directories(tmp / "config" / "fcitx5" / "conf");

    // Start a tiny Python mock OpenAI SSE server
    const fs::path mockScript = tmp / "mock_server.py";
    const fs::path portFile = tmp / "port";
    {
        std::ofstream out(mockScript);
        out << R"PY(
import json, os, sys, time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

REPLIES = {
    "ni mingtian you shijian ma": "你明天有时间吗？",
    "xiufu zhengju shuru": "修复整句输入",
    "women mingtian jian": "我们明天见。",
}

class H(BaseHTTPRequestHandler):
    def do_POST(self):
        n = int(self.headers.get("Content-Length", "0"))
        body = json.loads(self.rfile.read(n))
        src = body["messages"][-1]["content"]
        reply = REPLIES.get(src, "未知:" + src)
        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.end_headers()
        for ch in reply:
            chunk = {"choices": [{"delta": {"content": ch}}]}
            self.wfile.write(f"data: {json.dumps(chunk, ensure_ascii=False)}\n\n".encode())
            self.wfile.flush()
            time.sleep(0.01)
        self.wfile.write(b"data: [DONE]\n\n")
    def log_message(self, *args):
        pass

srv = ThreadingHTTPServer(("127.0.0.1", 0), H)
with open(sys.argv[1], "w") as f:
    f.write(f"{srv.server_address[1]} {os.getpid()}")
srv.serve_forever()
)PY";
    }

    const std::string cmd = "python3 \"" + mockScript.string() + "\" \"" + portFile.string() + "\" &";
    CHECK(std::system(cmd.c_str()) == 0);
    std::string port;
    pid_t serverPid = -1;
    for (int i = 0; i < 100; ++i) {
        if (fs::exists(portFile)) {
            std::ifstream in(portFile);
            in >> port >> serverPid;
            if (!port.empty() && serverPid > 0) {
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(!port.empty() && serverPid > 0);

    const std::string baseUrl = "http://127.0.0.1:" + port + "/v1";
    ::setenv("XDG_CONFIG_HOME", (tmp / "config").c_str(), 1);
    ::setenv("FCITX5_LLM_SENTENCE_URL", baseUrl.c_str(), 1);
    ::setenv("FCITX5_LLM_SENTENCE_MODEL", "mock-llm", 1);

    char arg0[] = "fcitx5";
    char arg1[] = "--disable=all";
    char *argv[] = {arg0, arg1, nullptr};
    fcitx::Instance instance(2, argv);

    std::unique_ptr<LlmSentenceAddon> addon;
    std::unique_ptr<FakeInputContext> ic;

    int stage = 0;
    int ticks = 0;
    auto timer = instance.eventLoop().addTimeEvent(
        CLOCK_MONOTONIC, fcitx::now(CLOCK_MONOTONIC) + 1000, 1000,
        [&](fcitx::EventSourceTime *src, uint64_t) {
            CHECK(++ticks < 400);
            if (stage == 0) {
                addon = std::make_unique<LlmSentenceAddon>(&instance);
                ic = std::make_unique<FakeInputContext>(instance.inputContextManager());
                ic->focusIn();
                // Case 1: Type pinyin directly into terminal, then press Alt+u ONCE -> auto-backspaces and replaces!
                ic->typeAsciiToTerminal("ni mingtian you shijian ma");
                CHECK(ic->terminalBuffer == "ni mingtian you shijian ma");
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_u, fcitx::KeyState::Alt)));
                CHECK(ic->forwarded.size() == 26);
                CHECK(ic->terminalBuffer.empty());
                stage = 1;
            } else if (stage == 1 && ic->commits.size() >= 1) {
                CHECK(ic->commits.back() == "你明天有时间吗？");
                CHECK(ic->terminalBuffer == "你明天有时间吗？");

                // Case 2: Type shell command with opening quote `git commit -m "xiufu zhengju shuru`, then press Alt+U ONCE
                // -> only backspaces `xiufu zhengju shuru` after the opening quote!
                ic->terminalBuffer.clear();
                ic->forwarded.clear();
                ic->typeAsciiToTerminal("git commit -m \"xiufu zhengju shuru");
                CHECK(ic->terminalBuffer == "git commit -m \"xiufu zhengju shuru");
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                CHECK(ic->forwarded.size() == std::string("xiufu zhengju shuru").size());
                CHECK(ic->terminalBuffer == "git commit -m \"");
                stage = 2;
            } else if (stage == 2 && ic->commits.size() >= 2) {
                CHECK(ic->commits.back() == "修复整句输入");
                CHECK(ic->terminalBuffer == "git commit -m \"修复整句输入");

                // Case 3: Press Alt+U first when nothing replaceable is at cursor -> enters draft mode, type & press Return
                CHECK(ic->sendKey(fcitx::Key("Alt+U")));
                ic->typeAsciiToTerminal("women mingtian jian");
                CHECK(ic->preedit == "women mingtian jian");
                CHECK(ic->sendKey(fcitx::Key(FcitxKey_Return)));
                stage = 3;
            } else if (stage == 3 && ic->commits.size() >= 3) {
                CHECK(ic->commits.back() == "我们明天见。");
                ic.reset();
                addon.reset();
                instance.exit();
                return true;
            }
            src->setNextInterval(15000);
            src->setOneShot();
            return true;
        });

    instance.exec();

    if (serverPid > 0) {
        ::kill(serverPid, SIGTERM);
    }
    fs::remove_all(tmp);
}

} // namespace

int main() {
    testPinyinSuffixExtraction();
    testJsonAndSseHelpers();
    testRequestBuilding();
    testEndToEndFcitx5Module();
    std::cout << "test_llm_sentence unit & end-to-end tests ok\n";
    return 0;
}
