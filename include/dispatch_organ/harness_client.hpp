#pragma once

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace dispatch_organ {

using json = nlohmann::json;

struct HarnessLaunch {
    std::filesystem::path binary;
    std::filesystem::path config;
    std::filesystem::path job;
    std::filesystem::path output;
    int timeout_ms{300000};
};

class HarnessClient {
public:
    explicit HarnessClient(HarnessLaunch launch);
    ~HarnessClient();

    HarnessClient(const HarnessClient&) = delete;
    HarnessClient& operator=(const HarnessClient&) = delete;

    json call_tool(const std::string& name, const json& arguments);
    const std::vector<std::string>& tools() const noexcept { return tools_; }

private:
    HarnessLaunch launch_;
    int child_stdin_{-1};
    int child_stdout_{-1};
    int child_pid_{-1};
    std::uint64_t next_id_{1};
    std::string buffered_output_;
    std::vector<std::string> tools_;

    json request(std::string method, json params = json::object());
    void write_line(const std::string& line);
    std::string read_line();
    void stop() noexcept;
};

} // namespace dispatch_organ

