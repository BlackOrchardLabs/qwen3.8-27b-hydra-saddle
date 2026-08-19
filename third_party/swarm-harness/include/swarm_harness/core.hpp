#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace swarm {

using json = nlohmann::json;

enum class Completeness { full, clipped };
enum class ArtifactFormat { html, json_document, text, file };
enum class TaskState { queued, running, penned, succeeded, gated, failed, refused };
enum class JobState { running, gated, done, failed };

struct WorkerConfig {
    std::string id;
    std::string seat;
    std::string endpoint;
    std::string model;
    std::string system_anchor;
    int max_concurrency{1};
    int active{0};
};

struct DirectorConfig {
    std::string endpoint;
    std::string model;
    std::string system_anchor;
    int max_tokens{4096};
    double temperature{0.1};
    int max_steps{40};
};

struct GateConfig {
    std::vector<std::string> argv;
    int timeout_seconds{60};
};

struct Caps {
    int max_concurrent_workers{4};
    int max_total_tasks{32};
    int max_tokens_per_task{8192};
    int max_wall_seconds{1800};
    std::uintmax_t max_artifact_bytes{2 * 1024 * 1024};
};

struct Config {
    DirectorConfig director;
    std::vector<WorkerConfig> workers;
    std::map<std::string, GateConfig> gates;
    Caps caps;
    std::filesystem::path organ_dir;
};

struct ArtifactContract {
    std::string id;
    std::string artifact_name;
    ArtifactFormat format{ArtifactFormat::file};
    std::uintmax_t minimum_bytes{1};
    std::vector<std::string> validators;
    std::string sha256;
};

struct JobSpec {
    std::string job_id;
    std::string mission;
    bool feel_artifact{false};
    json scripted_tasks{json::array()};
    std::map<std::string, ArtifactContract> artifact_contracts;
};

struct ArtifactRecord {
    std::string id;
    std::string task_id;
    std::filesystem::path path;
    std::string sha256;
    std::uintmax_t byte_count{0};
    Completeness completeness{Completeness::full};
};

struct GateResult {
    std::string name;
    bool passed{false};
    int exit_code{-1};
    bool timed_out{false};
    std::string output;
};

struct ArtifactCheck {
    std::string name;
    bool passed{false};
    int exit_code{-1};
    bool timed_out{false};
    std::string output;
};

struct TaskRecord {
    std::string id;
    std::string role;
    std::string seat;
    std::string worker_id;
    std::string prompt;
    std::vector<std::string> input_artifact_ids;
    std::vector<std::string> input_result_task_ids;
    std::optional<std::string> artifact_name;
    std::optional<std::string> artifact_contract_id;
    std::string artifact_contract_sha256;
    std::optional<std::string> repair_of;
    std::optional<std::string> error_receipt_task_id;
    std::string exact_error_receipt;
    TaskState state{TaskState::queued};
    std::string content;
    std::optional<ArtifactRecord> artifact;
    std::vector<GateResult> gates;
    std::vector<ArtifactCheck> artifact_checks;
    std::string failed_artifact_check;
    int prompt_tokens{0};
    int completion_tokens{0};
    long duration_ms{0};
    std::string error;
    mutable std::condition_variable cv;
};

struct ProcessResult {
    int exit_code{-1};
    bool timed_out{false};
    std::string output;
};

class Harness {
public:
    Harness(Config config, JobSpec job, std::filesystem::path output_root);
    ~Harness();

    Harness(const Harness&) = delete;
    Harness& operator=(const Harness&) = delete;

    json call_tool(const std::string& name, const json& arguments);
    json report_malformed_tool_call(
        const std::string& name,
        const std::string& reason,
        const std::string& raw_arguments);
    json tool_definitions() const;
    json mcp_tool_definitions() const;
    json status() const;
    void wait_all();

    const Config& config() const { return config_; }
    const JobSpec& job() const { return job_; }
    const std::filesystem::path& output_root() const { return output_root_; }

private:
    Config config_;
    JobSpec job_;
    std::filesystem::path output_root_;
    std::filesystem::path journal_path_;
    std::chrono::steady_clock::time_point started_;
    mutable std::mutex mutex_;
    mutable std::mutex journal_mutex_;
    std::condition_variable worker_cv_;
    std::map<std::string, std::shared_ptr<TaskRecord>> tasks_;
    std::map<std::string, ArtifactRecord> artifacts_;
    std::vector<std::future<void>> futures_;
    int active_workers_{0};
    int task_count_{0};
    JobState job_state_{JobState::running};
    int artifact_refusal_count_{0};
    std::string artifact_refusal_task_id_;
    std::string artifact_refusal_check_;

    json list_workers(const json& arguments);
    json dispatch_task(const json& arguments);
    json get_result(const json& arguments);
    json run_gate(const json& arguments);
    json finish_job(const json& arguments);

    void execute_task(const std::shared_ptr<TaskRecord>& task);
    WorkerConfig* acquire_worker(const std::string& seat);
    void release_worker(WorkerConfig* worker);
    void enforce_job_id(const json& arguments) const;
    void enforce_caps_locked() const;
    std::vector<ArtifactRecord> verify_judge_inputs_locked(
        const std::string& role,
        const std::vector<std::string>& artifact_ids,
        const std::string& task_id);
    std::filesystem::path resolve_artifact_path(const std::string& relative) const;
    ArtifactRecord persist_artifact(
        const std::shared_ptr<TaskRecord>& task,
        const std::string& content,
        Completeness completeness);
    std::vector<ArtifactCheck> validate_artifact(
        const ArtifactRecord& artifact,
        const ArtifactContract& contract);
    json task_result_locked(const std::shared_ptr<TaskRecord>& task, bool include_content);
    json verify_artifact_locked(const ArtifactRecord& artifact) const;
    void journal(json event);
    void journal_refusal(const std::string& tool, const std::string& reason, const json& arguments);
    bool wall_clock_expired() const;
};

Config load_config(const std::filesystem::path& path);
JobSpec load_job(const std::filesystem::path& path);
json http_chat_completion(const std::string& endpoint, const json& request, long timeout_seconds);
std::string sha256_file(const std::filesystem::path& path);
std::string sha256_text(const std::string& text);
std::string utc_now();
std::string to_string(Completeness value);
std::string to_string(ArtifactFormat value);
std::string to_string(TaskState value);
std::string to_string(JobState value);
bool is_judge_role(const std::string& role);
ProcessResult run_process(
    const std::vector<std::string>& argv,
    const std::filesystem::path& cwd,
    int timeout_seconds);

int run_scripted(Harness& harness);
int run_director(Harness& harness);
int run_mcp(Harness& harness);

} // namespace swarm
