#include "dispatch_organ/harness_client.hpp"

#include "dispatch_organ/core.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <thread>

namespace dispatch_organ {
namespace {

void close_if_open(int& fd) {
    if (fd >= 0) {
        ::close(fd);
        fd = -1;
    }
}

} // namespace

HarnessClient::HarnessClient(HarnessLaunch launch) : launch_(std::move(launch)) {
    if (!std::filesystem::is_regular_file(launch_.binary)) {
        throw std::runtime_error("accepted swarm-harness binary is missing: " + launch_.binary.string());
    }
    if (!std::filesystem::is_regular_file(launch_.config) ||
        !std::filesystem::is_regular_file(launch_.job)) {
        throw std::runtime_error("swarm-harness config or job fixture is missing");
    }
    if (std::filesystem::exists(launch_.output)) {
        throw std::runtime_error("swarm-harness output path must be new: " + launch_.output.string());
    }
    if (launch_.timeout_ms < 1) throw std::runtime_error("harness timeout must be positive");

    int to_child[2]{};
    int from_child[2]{};
    if (::pipe(to_child) != 0 || ::pipe(from_child) != 0) {
        throw std::runtime_error("cannot create swarm-harness pipes");
    }
    const pid_t pid = ::fork();
    if (pid < 0) {
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);
        throw std::runtime_error("cannot fork swarm-harness");
    }
    if (pid == 0) {
        ::dup2(to_child[0], STDIN_FILENO);
        ::dup2(from_child[1], STDOUT_FILENO);
        ::close(to_child[0]);
        ::close(to_child[1]);
        ::close(from_child[0]);
        ::close(from_child[1]);

        const auto binary = launch_.binary.string();
        const auto config = launch_.config.string();
        const auto job = launch_.job.string();
        const auto output = launch_.output.string();
        std::array<char*, 9> arguments{
            const_cast<char*>(binary.c_str()),
            const_cast<char*>("mcp"),
            const_cast<char*>("--config"),
            const_cast<char*>(config.c_str()),
            const_cast<char*>("--job"),
            const_cast<char*>(job.c_str()),
            const_cast<char*>("--output"),
            const_cast<char*>(output.c_str()),
            nullptr,
        };
        ::execv(binary.c_str(), arguments.data());
        _exit(127);
    }

    child_pid_ = static_cast<int>(pid);
    child_stdin_ = to_child[1];
    child_stdout_ = from_child[0];
    ::close(to_child[0]);
    ::close(from_child[1]);

    try {
        const auto initialized = request("initialize", {
            {"protocolVersion", "2025-11-25"},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "dispatch-organ"}, {"version", "0.1.0"}}},
        });
        if (initialized.at("result").at("serverInfo").at("name") != "swarm-harness") {
            throw std::runtime_error("mounted engine did not identify as swarm-harness");
        }
        const auto listed = request("tools/list");
        for (const auto& tool : listed.at("result").at("tools")) {
            tools_.push_back(tool.at("name").get<std::string>());
        }
        const std::vector<std::string> expected{
            "list_workers", "dispatch_task", "get_result", "run_gate", "finish_job"};
        if (tools_ != expected) {
            throw std::runtime_error("swarm-harness tool surface differs from the accepted five-tool boundary");
        }
    } catch (...) {
        stop();
        throw;
    }
}

HarnessClient::~HarnessClient() {
    stop();
}

void HarnessClient::write_line(const std::string& line) {
    std::string bytes = line;
    bytes.push_back('\n');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(child_stdin_, bytes.data() + offset, bytes.size() - offset);
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot write to swarm-harness MCP subprocess");
        }
        offset += static_cast<std::size_t>(count);
    }
}

std::string HarnessClient::read_line() {
    while (true) {
        const auto newline = buffered_output_.find('\n');
        if (newline != std::string::npos) {
            auto line = buffered_output_.substr(0, newline);
            buffered_output_.erase(0, newline + 1);
            return line;
        }
        pollfd descriptor{child_stdout_, POLLIN, 0};
        int ready = 0;
        do {
            ready = ::poll(&descriptor, 1, launch_.timeout_ms);
        } while (ready < 0 && errno == EINTR);
        if (ready == 0) throw std::runtime_error("swarm-harness MCP response timed out");
        if (ready < 0) throw std::runtime_error("cannot poll swarm-harness MCP subprocess");
        std::array<char, 8192> buffer{};
        const auto count = ::read(child_stdout_, buffer.data(), buffer.size());
        if (count == 0) throw std::runtime_error("swarm-harness MCP subprocess closed stdout");
        if (count < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot read swarm-harness MCP response");
        }
        buffered_output_.append(buffer.data(), static_cast<std::size_t>(count));
    }
}

json HarnessClient::request(std::string method, json params) {
    const auto id = next_id_++;
    json request_value{{"jsonrpc", "2.0"}, {"id", id}, {"method", std::move(method)}};
    if (!params.empty()) request_value["params"] = std::move(params);
    write_line(request_value.dump());
    const auto response = parse_strict_json(read_line());
    if (response.value("id", 0ULL) != id) {
        throw std::runtime_error("swarm-harness MCP response id mismatch");
    }
    if (response.contains("error")) {
        throw Refusal("HARNESS_ERROR", response.at("error").dump());
    }
    return response;
}

json HarnessClient::call_tool(const std::string& name, const json& arguments) {
    if (std::find(tools_.begin(), tools_.end(), name) == tools_.end()) {
        throw Refusal("HARNESS_TOOL_ABSENT", "tool is absent from accepted swarm-harness surface: " + name);
    }
    const auto response = request("tools/call", {
        {"name", name},
        {"arguments", arguments},
    });
    const auto& result = response.at("result");
    const auto structured = result.at("structuredContent");
    if (result.value("isError", false) || !structured.value("ok", false)) {
        throw Refusal("HARNESS_REFUSAL", structured.dump());
    }
    return structured;
}

void HarnessClient::stop() noexcept {
    close_if_open(child_stdin_);
    close_if_open(child_stdout_);
    if (child_pid_ < 0) return;
    int status = 0;
    for (int attempt = 0; attempt < 20; ++attempt) {
        const auto waited = ::waitpid(static_cast<pid_t>(child_pid_), &status, WNOHANG);
        if (waited == child_pid_ || waited < 0) {
            child_pid_ = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    ::kill(static_cast<pid_t>(child_pid_), SIGTERM);
    (void)::waitpid(static_cast<pid_t>(child_pid_), &status, 0);
    child_pid_ = -1;
}

} // namespace dispatch_organ
