#ifndef YUNTYPE_WIN_SETTINGS_SERVER_H
#define YUNTYPE_WIN_SETTINGS_SERVER_H

#include "win_config.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>

namespace yuntype::win {

class SettingsServer {
public:
    struct Callbacks {
        std::function<std::string()> getStateJson;
        std::function<std::pair<bool, std::string>(const std::string &body)> saveStateJson;
        std::function<void()> clearHistory;
    };

    SettingsServer() = default;
    ~SettingsServer();

    SettingsServer(const SettingsServer &) = delete;
    SettingsServer &operator=(const SettingsServer &) = delete;

    // Start the localhost-only HTTP server if not running, and open the Settings page.
    bool openSettingsUi(Callbacks callbacks);
    void stop();

private:
    void serveLoop();
    void handleClient(std::uintptr_t sockVal);

    Callbacks callbacks_;
    std::string token_;
    std::uint16_t port_ = 0;
    std::uintptr_t listenSock_ = static_cast<std::uintptr_t>(-1);
    std::atomic<bool> running_{false};
    std::thread thread_;
};

} // namespace yuntype::win

#endif
