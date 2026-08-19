#include "swarm_harness/core.hpp"

#include <curl/curl.h>
#include <openssl/sha.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <iostream>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>

namespace fs = std::filesystem;

namespace swarm {
namespace {

constexpr const char* kRoleBuilder = "Builder";
constexpr const char* kRoleScout = "Scout";
constexpr const char* kRoleAdversary = "Adversary";
constexpr const char* kRoleVerifier = "Verifier/Integrator";
constexpr std::size_t kMaxValidatorsPerContract = 8;
constexpr std::size_t kMaxArtifactNameBytes = 256;

const std::map<std::string, std::string> kRoleDefinitions{
    {kRoleBuilder, "implements the intended change."},
    {kRoleScout, "investigates the surrounding system, dependencies, and likely fracture points."},
    {kRoleAdversary, "tries to disprove the plan and expose regressions or false completion."},
    {kRoleVerifier, "tests the artifact against the actual mission and reconciles competing findings."},
};

bool terminal(TaskState state) {
    return state == TaskState::penned || state == TaskState::succeeded ||
           state == TaskState::gated || state == TaskState::failed ||
           state == TaskState::refused;
}

bool path_within(const fs::path& root, const fs::path& candidate) {
    const auto normalized_root = fs::weakly_canonical(root);
    const auto normalized_candidate = fs::weakly_canonical(candidate);
    auto root_it = normalized_root.begin();
    auto candidate_it = normalized_candidate.begin();
    for (; root_it != normalized_root.end(); ++root_it, ++candidate_it) {
        if (candidate_it == normalized_candidate.end() || *root_it != *candidate_it) {
            return false;
        }
    }
    return true;
}

std::string read_text(const fs::path& path, std::uintmax_t max_bytes = 16 * 1024 * 1024) {
    const auto size = fs::file_size(path);
    if (size > max_bytes) {
        throw std::runtime_error("file exceeds read limit: " + path.string());
    }
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        throw std::runtime_error("cannot open file: " + path.string());
    }
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

json read_json(const fs::path& path) {
    try {
        return json::parse(read_text(path));
    } catch (const std::exception& error) {
        throw std::runtime_error("invalid JSON in " + path.string() + ": " + error.what());
    }
}

std::string replace_all(std::string value, const std::string& needle, const std::string& replacement) {
    std::size_t offset = 0;
    while ((offset = value.find(needle, offset)) != std::string::npos) {
        value.replace(offset, needle.size(), replacement);
        offset += replacement.size();
    }
    return value;
}

std::vector<std::string> json_string_array(const json& value, const std::string& field) {
    std::vector<std::string> result;
    if (value.is_null()) {
        return result;
    }
    if (!value.is_array()) {
        throw std::runtime_error(field + " must be an array");
    }
    for (const auto& item : value) {
        if (!item.is_string()) {
            throw std::runtime_error(field + " entries must be strings");
        }
        result.push_back(item.get<std::string>());
    }
    return result;
}

void validate_id(const std::string& value, const std::string& field) {
    static const std::regex pattern("^[A-Za-z0-9][A-Za-z0-9_.-]{0,63}$");
    if (!std::regex_match(value, pattern)) {
        throw std::runtime_error(field + " must match [A-Za-z0-9][A-Za-z0-9_.-]{0,63}");
    }
}

void require_only_keys(const json& value, const std::vector<std::string>& allowed, const std::string& field) {
    if (!value.is_object()) {
        throw std::runtime_error(field + " must be an object");
    }
    for (const auto& [key, ignored] : value.items()) {
        (void)ignored;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            throw std::runtime_error(field + " has unknown key: " + key);
        }
    }
}

ArtifactFormat parse_artifact_format(const std::string& value) {
    if (value == "html") return ArtifactFormat::html;
    if (value == "json") return ArtifactFormat::json_document;
    if (value == "text") return ArtifactFormat::text;
    if (value == "file") return ArtifactFormat::file;
    throw std::runtime_error("unknown artifact format: " + value);
}

std::uintmax_t builtin_minimum(ArtifactFormat format) {
    switch (format) {
        case ArtifactFormat::html: return 256;
        case ArtifactFormat::json_document: return 2;
        case ArtifactFormat::text: return 1;
        case ArtifactFormat::file: return 1;
    }
    return 1;
}

bool valid_utf8_without_nul(const std::string& value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto byte = static_cast<unsigned char>(value[i]);
        if (byte == 0) return false;
        std::size_t continuation = 0;
        if (byte <= 0x7f) continuation = 0;
        else if (byte >= 0xc2 && byte <= 0xdf) continuation = 1;
        else if (byte >= 0xe0 && byte <= 0xef) continuation = 2;
        else if (byte >= 0xf0 && byte <= 0xf4) continuation = 3;
        else return false;
        if (i + continuation >= value.size()) return false;
        for (std::size_t j = 1; j <= continuation; ++j) {
            if ((static_cast<unsigned char>(value[i + j]) & 0xc0) != 0x80) return false;
        }
        if (continuation == 2) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((byte == 0xe0 && second < 0xa0) || (byte == 0xed && second >= 0xa0)) return false;
        } else if (continuation == 3) {
            const auto second = static_cast<unsigned char>(value[i + 1]);
            if ((byte == 0xf0 && second < 0x90) || (byte == 0xf4 && second >= 0x90)) return false;
        }
        i += continuation + 1;
    }
    return true;
}

bool html_prefix_matches(const std::string& content) {
    // Exact dependency-free sniff: after optional UTF-8 BOM and ASCII whitespace,
    // the first markup token must be <!doctype html...> or <html...>, and its
    // closing '>' must occur within the first 256 bytes. The 256-byte built-in
    // floor independently rejects trivial bodies such as the sealed 16-byte corpse.
    std::size_t offset = content.rfind("\xef\xbb\xbf", 0) == 0 ? 3 : 0;
    while (offset < content.size() && std::string(" \t\r\n\f").find(content[offset]) != std::string::npos) ++offset;
    const auto window = content.substr(offset, std::min<std::size_t>(256, content.size() - offset));
    std::string lowered = window;
    std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char c) { return std::tolower(c); });
    const bool shaped = lowered.rfind("<!doctype html", 0) == 0 || lowered.rfind("<html", 0) == 0;
    return shaped && lowered.find('>') != std::string::npos;
}

json artifact_checks_json(const std::vector<ArtifactCheck>& checks) {
    json result = json::array();
    for (const auto& check : checks) {
        result.push_back({
            {"name", check.name}, {"passed", check.passed}, {"exit_code", check.exit_code},
            {"timed_out", check.timed_out}, {"output", check.output},
        });
    }
    return result;
}

std::string curl_error(CURLcode code, const std::array<char, CURL_ERROR_SIZE>& buffer) {
    if (buffer[0] != '\0') {
        return buffer.data();
    }
    return curl_easy_strerror(code);
}

std::size_t curl_write(char* ptr, std::size_t size, std::size_t nmemb, void* userdata) {
    const auto count = size * nmemb;
    auto* output = static_cast<std::string*>(userdata);
    output->append(ptr, count);
    return count;
}

json tool_schema(
    const std::string& name,
    const std::string& description,
    json properties,
    json required,
    bool read_only = false) {
    return {
        {"name", name},
        {"description", description},
        {"inputSchema", {
            {"type", "object"},
            {"additionalProperties", false},
            {"properties", std::move(properties)},
            {"required", std::move(required)},
        }},
        {"annotations", {
            {"readOnlyHint", read_only},
            {"destructiveHint", false},
            {"openWorldHint", false},
        }},
    };
}

json openai_tool(const json& mcp_tool) {
    return {
        {"type", "function"},
        {"function", {
            {"name", mcp_tool.at("name")},
            {"description", mcp_tool.at("description")},
            {"parameters", mcp_tool.at("inputSchema")},
        }},
    };
}

std::string message_content(const json& response) {
    const auto& message = response.at("choices").at(0).at("message");
    if (!message.contains("content") || message.at("content").is_null()) {
        return {};
    }
    if (!message.at("content").is_string()) {
        throw std::runtime_error("model response content is not text");
    }
    return message.at("content").get<std::string>();
}

void compact_delivered_director_history(json& messages) {
    for (auto& message : messages) {
        if (message.value("role", "") == "tool" && message.contains("content") &&
            message.at("content").is_string()) {
            const auto raw = message.at("content").get<std::string>();
            try {
                auto payload = json::parse(raw);
                bool changed = false;
                if (payload.contains("content") && payload.at("content").is_string()) {
                    const auto content = payload.at("content").get<std::string>();
                    payload["delivered_content_sha256"] = sha256_text(content);
                    payload["delivered_content_bytes"] = content.size();
                    payload.erase("content");
                    changed = true;
                }
                if (payload.contains("evidence") && payload.at("evidence").is_string()) {
                    const auto evidence = payload.at("evidence").get<std::string>();
                    payload["delivered_evidence_sha256"] = sha256_text(evidence);
                    payload["delivered_evidence_bytes"] = evidence.size();
                    payload.erase("evidence");
                    changed = true;
                }
                if (changed) {
                    payload["history_compaction"] =
                        "complete body was delivered in the prior turn and remains in the journal/artifact store";
                    message["content"] = payload.dump();
                }
            } catch (const std::exception&) {
                if (raw.size() > 4096) {
                    message["content"] = json({
                        {"history_compaction", "non-JSON tool body was delivered in the prior turn"},
                        {"delivered_sha256", sha256_text(raw)},
                        {"delivered_bytes", raw.size()},
                    }).dump();
                }
            }
        }
        if (message.value("role", "") == "assistant" && message.contains("tool_calls") &&
            message.at("tool_calls").is_array()) {
            for (auto& call : message["tool_calls"]) {
                auto& arguments = call["function"]["arguments"];
                if (arguments.is_string() && arguments.get_ref<const std::string&>().size() > 4096) {
                    const auto raw = arguments.get<std::string>();
                    arguments = json({
                        {"history_compaction", "large tool arguments already executed or refused"},
                        {"delivered_sha256", sha256_text(raw)},
                        {"delivered_bytes", raw.size()},
                    }).dump();
                }
            }
        }
    }
}

void write_exclusive(const fs::path& path, const std::string& content) {
    const int descriptor = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
    if (descriptor < 0) {
        throw std::runtime_error("refusing to overwrite artifact " + path.string() + ": " + std::strerror(errno));
    }
    std::size_t written = 0;
    while (written < content.size()) {
        const auto count = ::write(descriptor, content.data() + written, content.size() - written);
        if (count < 0) {
            const auto reason = std::strerror(errno);
            ::close(descriptor);
            throw std::runtime_error("artifact write failed: " + std::string(reason));
        }
        written += static_cast<std::size_t>(count);
    }
    if (::fsync(descriptor) != 0) {
        const auto reason = std::strerror(errno);
        ::close(descriptor);
        throw std::runtime_error("artifact fsync failed: " + std::string(reason));
    }
    ::close(descriptor);
}

} // namespace

std::string to_string(Completeness value) {
    return value == Completeness::full ? "FULL" : "CLIPPED";
}

std::string to_string(ArtifactFormat value) {
    switch (value) {
        case ArtifactFormat::html: return "html";
        case ArtifactFormat::json_document: return "json";
        case ArtifactFormat::text: return "text";
        case ArtifactFormat::file: return "file";
    }
    return "unknown";
}

std::string to_string(TaskState value) {
    switch (value) {
        case TaskState::queued: return "QUEUED";
        case TaskState::running: return "RUNNING";
        case TaskState::penned: return "PENNED";
        case TaskState::succeeded: return "SUCCEEDED";
        case TaskState::gated: return "GATED";
        case TaskState::failed: return "FAILED";
        case TaskState::refused: return "ARTIFACT_REFUSED";
    }
    return "UNKNOWN";
}

std::string to_string(JobState value) {
    switch (value) {
        case JobState::running: return "RUNNING";
        case JobState::gated: return "GATED";
        case JobState::done: return "DONE";
        case JobState::failed: return "FAILED";
    }
    return "UNKNOWN";
}

bool is_judge_role(const std::string& role) {
    return role == kRoleAdversary || role == kRoleVerifier;
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&time, &tm);
    std::ostringstream output;
    output << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

std::string sha256_text(const std::string& text) {
    std::array<unsigned char, SHA256_DIGEST_LENGTH> digest{};
    SHA256(reinterpret_cast<const unsigned char*>(text.data()), text.size(), digest.data());
    std::ostringstream output;
    for (const auto byte : digest) {
        output << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return output.str();
}

std::string sha256_file(const fs::path& path) {
    return sha256_text(read_text(path));
}

Config load_config(const fs::path& path) {
    const auto data = read_json(path);
    Config config;
    config.organ_dir = fs::weakly_canonical(path).parent_path().parent_path();

    if (data.contains("director")) {
        const auto& director = data.at("director");
        config.director.endpoint = director.value("endpoint", "");
        config.director.model = director.value("model", "local");
        config.director.system_anchor = director.value("system_anchor", "You are the director. Use only the supplied tools.");
        config.director.max_tokens = director.value("max_tokens", 4096);
        config.director.temperature = director.value("temperature", 0.1);
        config.director.max_steps = director.value("max_steps", 40);
    }

    for (const auto& worker : data.at("workers")) {
        WorkerConfig entry;
        entry.id = worker.at("id").get<std::string>();
        entry.seat = worker.at("seat").get<std::string>();
        entry.endpoint = worker.at("endpoint").get<std::string>();
        entry.model = worker.value("model", "local");
        entry.system_anchor = worker.value(
            "system_anchor",
            "You are a text-only worker. Return only the requested result. Do not emit tool calls.");
        entry.max_concurrency = worker.value("max_concurrency", 1);
        validate_id(entry.id, "worker id");
        if (entry.endpoint.rfind("http://127.0.0.1:", 0) != 0 &&
            entry.endpoint.rfind("http://localhost:", 0) != 0) {
            throw std::runtime_error("worker endpoint is not loopback: " + entry.endpoint);
        }
        if (entry.max_concurrency < 1) {
            throw std::runtime_error("worker max_concurrency must be positive");
        }
        config.workers.push_back(std::move(entry));
    }
    if (config.workers.empty()) {
        throw std::runtime_error("config requires at least one worker");
    }

    if (data.contains("caps")) {
        const auto& caps = data.at("caps");
        config.caps.max_concurrent_workers = caps.value("max_concurrent_workers", 4);
        config.caps.max_total_tasks = caps.value("max_total_tasks", 32);
        config.caps.max_tokens_per_task = caps.value("max_tokens_per_task", 8192);
        config.caps.max_wall_seconds = caps.value("max_wall_seconds", 1800);
        config.caps.max_artifact_bytes = caps.value("max_artifact_bytes", 2 * 1024 * 1024);
    }
    if (config.caps.max_concurrent_workers < 1 || config.caps.max_total_tasks < 1 ||
        config.caps.max_tokens_per_task < 1 || config.caps.max_wall_seconds < 1) {
        throw std::runtime_error("all caps must be positive");
    }

    if (data.contains("gates")) {
        for (const auto& [name, value] : data.at("gates").items()) {
            validate_id(name, "gate name");
            GateConfig gate;
            gate.argv = json_string_array(value.at("argv"), "gate argv");
            gate.timeout_seconds = value.value("timeout_seconds", 60);
            if (gate.argv.empty() || gate.timeout_seconds < 1) {
                throw std::runtime_error("gate requires argv and a positive timeout: " + name);
            }
            config.gates.emplace(name, std::move(gate));
        }
    }
    return config;
}

JobSpec load_job(const fs::path& path) {
    const auto data = read_json(path);
    JobSpec job;
    job.job_id = data.at("job_id").get<std::string>();
    validate_id(job.job_id, "job_id");
    job.mission = data.at("mission").get<std::string>();
    job.feel_artifact = data.value("feel_artifact", false);
    if (data.contains("artifact_contracts")) {
        const auto& declarations = data.at("artifact_contracts");
        if (!declarations.is_array()) {
            throw std::runtime_error("artifact_contracts must be an array");
        }
        std::map<std::string, std::string> artifact_names;
        for (const auto& declaration : declarations) {
            require_only_keys(
                declaration,
                {"id", "artifact_name", "format", "minimum_bytes", "validators"},
                "artifact contract");
            ArtifactContract contract;
            contract.id = declaration.at("id").get<std::string>();
            contract.artifact_name = declaration.at("artifact_name").get<std::string>();
            contract.format = parse_artifact_format(declaration.at("format").get<std::string>());
            contract.minimum_bytes = declaration.value("minimum_bytes", builtin_minimum(contract.format));
            contract.validators = json_string_array(declaration.value("validators", json::array()), "contract validators");
            validate_id(contract.id, "artifact contract id");
            if (contract.artifact_name.empty() || contract.artifact_name.size() > kMaxArtifactNameBytes) {
                throw std::runtime_error("contract artifact_name must contain 1..256 bytes");
            }
            const auto artifact_path = fs::path(contract.artifact_name).lexically_normal();
            if (fs::path(contract.artifact_name).is_absolute() || artifact_path.empty() || artifact_path == "." ||
                std::any_of(artifact_path.begin(), artifact_path.end(), [](const auto& part) { return part == ".."; }) ||
                artifact_path.begin()->string().front() == '.' || artifact_path == "journal.jsonl") {
                throw std::runtime_error("contract artifact_name is not a safe relative artifact path");
            }
            if (contract.minimum_bytes < builtin_minimum(contract.format)) {
                throw std::runtime_error("contract minimum_bytes is below built-in " + to_string(contract.format) + " floor");
            }
            if (contract.validators.size() > kMaxValidatorsPerContract) {
                throw std::runtime_error("contract exceeds validator count cap");
            }
            std::vector<std::string> seen_validators;
            for (const auto& validator : contract.validators) {
                validate_id(validator, "validator name");
                if (std::find(seen_validators.begin(), seen_validators.end(), validator) != seen_validators.end()) {
                    throw std::runtime_error("duplicate validator in artifact contract: " + validator);
                }
                seen_validators.push_back(validator);
            }
            json canonical{
                {"artifact_name", contract.artifact_name}, {"format", to_string(contract.format)},
                {"id", contract.id}, {"minimum_bytes", contract.minimum_bytes},
                {"validators", contract.validators},
            };
            const auto canonical_payload = canonical.dump();
            if (canonical_payload.size() > 4096) {
                throw std::runtime_error("artifact contract exceeds 4096-byte configuration cap");
            }
            contract.sha256 = sha256_text(canonical_payload);
            if (job.artifact_contracts.contains(contract.id)) {
                throw std::runtime_error("duplicate artifact contract id: " + contract.id);
            }
            if (artifact_names.contains(contract.artifact_name)) {
                throw std::runtime_error("duplicate artifact contract for artifact_name: " + contract.artifact_name);
            }
            artifact_names.emplace(contract.artifact_name, contract.id);
            job.artifact_contracts.emplace(contract.id, std::move(contract));
        }
    }
    if (data.contains("tasks")) {
        if (!data.at("tasks").is_array()) {
            throw std::runtime_error("job tasks must be an array");
        }
        job.scripted_tasks = data.at("tasks");
    }
    return job;
}

json http_chat_completion(const std::string& endpoint, const json& request, long timeout_seconds) {
    CURL* handle = curl_easy_init();
    if (handle == nullptr) {
        throw std::runtime_error("curl_easy_init failed");
    }
    const std::string request_body = request.dump();
    std::string response_body;
    std::array<char, CURL_ERROR_SIZE> error_buffer{};
    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");

    curl_easy_setopt(handle, CURLOPT_URL, endpoint.c_str());
    curl_easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request_body.size()));
    curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, curl_write);
    curl_easy_setopt(handle, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error_buffer.data());
    curl_easy_setopt(handle, CURLOPT_TIMEOUT, timeout_seconds);
    curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);

    const auto code = curl_easy_perform(handle);
    long status = 0;
    curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(handle);

    if (code != CURLE_OK) {
        throw std::runtime_error("HTTP request failed: " + curl_error(code, error_buffer));
    }
    if (status < 200 || status >= 300) {
        throw std::runtime_error("HTTP " + std::to_string(status) + ": " + response_body.substr(0, 1000));
    }
    try {
        return json::parse(response_body);
    } catch (const std::exception& error) {
        throw std::runtime_error("endpoint returned invalid JSON: " + std::string(error.what()));
    }
}

Harness::Harness(Config config, JobSpec job, fs::path output_root)
    : config_(std::move(config)),
      job_(std::move(job)),
      output_root_(fs::absolute(std::move(output_root)).lexically_normal()),
      started_(std::chrono::steady_clock::now()) {
    if (output_root_ == output_root_.root_path()) {
        throw std::runtime_error("output root may not be filesystem root");
    }
    for (const auto& [id, contract] : job_.artifact_contracts) {
        if (contract.minimum_bytes > config_.caps.max_artifact_bytes) {
            throw std::runtime_error("artifact contract minimum_bytes exceeds max_artifact_bytes: " + id);
        }
        long aggregate_timeout = 0;
        for (const auto& validator : contract.validators) {
            const auto found = config_.gates.find(validator);
            if (found == config_.gates.end()) {
                throw std::runtime_error("unknown artifact validator " + validator + " in contract " + id);
            }
            aggregate_timeout += found->second.timeout_seconds;
        }
        if (aggregate_timeout > config_.caps.max_wall_seconds) {
            throw std::runtime_error("artifact contract validator budget exceeds max_wall_seconds: " + id);
        }
    }
    fs::create_directories(output_root_);
    output_root_ = fs::weakly_canonical(output_root_);
    journal_path_ = output_root_ / "journal.jsonl";
    auto receipt_output_root = fs::relative(output_root_, fs::current_path()).lexically_normal();
    if (receipt_output_root.empty() || receipt_output_root.is_absolute() ||
        (!receipt_output_root.empty() && *receipt_output_root.begin() == "..")) {
        receipt_output_root = output_root_.filename();
    }
    journal({
        {"event", "job_started"},
        {"outcome", "RUNNING"},
        {"feel_artifact", job_.feel_artifact},
        {"artifact_contract_count", job_.artifact_contracts.size()},
        {"output_root", receipt_output_root.generic_string()},
        {"caps", {
            {"max_concurrent_workers", config_.caps.max_concurrent_workers},
            {"max_total_tasks", config_.caps.max_total_tasks},
            {"max_tokens_per_task", config_.caps.max_tokens_per_task},
            {"max_wall_seconds", config_.caps.max_wall_seconds},
        }},
    });
}

Harness::~Harness() {
    wait_all();
}

bool Harness::wall_clock_expired() const {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::steady_clock::now() - started_).count() >= config_.caps.max_wall_seconds;
}

void Harness::enforce_job_id(const json& arguments) const {
    if (!arguments.contains("job_id") || !arguments.at("job_id").is_string() ||
        arguments.at("job_id").get<std::string>() != job_.job_id) {
        throw std::runtime_error("job_id does not match this harness process");
    }
}

void Harness::enforce_caps_locked() const {
    if (task_count_ >= config_.caps.max_total_tasks) {
        throw std::runtime_error("max_total_tasks cap reached");
    }
    if (wall_clock_expired()) {
        throw std::runtime_error("max_wall_seconds cap reached");
    }
}

void Harness::journal(json event) {
    event["timestamp"] = utc_now();
    event["job_id"] = job_.job_id;
    std::lock_guard lock(journal_mutex_);
    std::ofstream output(journal_path_, std::ios::app);
    if (!output) {
        throw std::runtime_error("cannot append journal: " + journal_path_.string());
    }
    output << event.dump() << '\n';
    output.flush();
}

void Harness::journal_refusal(const std::string& tool, const std::string& reason, const json& arguments) {
    json safe_arguments = arguments;
    if (safe_arguments.contains("prompt") && safe_arguments.at("prompt").is_string()) {
        const auto prompt = safe_arguments.at("prompt").get<std::string>();
        safe_arguments["prompt_sha256"] = sha256_text(prompt);
        safe_arguments["prompt_bytes"] = prompt.size();
        safe_arguments.erase("prompt");
    }
    journal({
        {"event", "tool_refused"},
        {"tool", tool},
        {"outcome", "REFUSED"},
        {"reason", reason},
        {"arguments", safe_arguments},
    });
}

json Harness::call_tool(const std::string& name, const json& arguments) {
    try {
        if (name == "list_workers") return list_workers(arguments);
        if (name == "dispatch_task") return dispatch_task(arguments);
        if (name == "get_result") return get_result(arguments);
        if (name == "run_gate") return run_gate(arguments);
        if (name == "finish_job") return finish_job(arguments);
        throw std::runtime_error("unknown tool: " + name);
    } catch (const std::exception& error) {
        journal_refusal(name, error.what(), arguments);
        return {
            {"ok", false},
            {"outcome", "REFUSED"},
            {"error", error.what()},
            {"job_id", job_.job_id},
        };
    }
}

json Harness::report_malformed_tool_call(
    const std::string& name,
    const std::string& reason,
    const std::string& raw_arguments) {
    journal({
        {"event", "tool_refused"},
        {"tool", name},
        {"outcome", "REFUSED"},
        {"reason", reason},
        {"raw_arguments_sha256", sha256_text(raw_arguments)},
        {"raw_arguments_bytes", raw_arguments.size()},
    });
    return {
        {"ok", false},
        {"outcome", "REFUSED"},
        {"error", reason},
        {"job_id", job_.job_id},
        {"retryable", true},
        {"instruction", "Resend valid compact JSON. Reference task and artifact ids instead of pasting their contents."},
    };
}

json Harness::mcp_tool_definitions() const {
    const json job_id = {{"type", "string"}, {"description", "Explicit job handle returned by the job launcher."}};
    json tools = json::array();
    tools.push_back(tool_schema(
        "list_workers",
        "List configured worker identities, semantic seats, and in-process capacity. Does not probe, start, or stop servers.",
        {{"job_id", job_id}},
        {"job_id"},
        true));
    tools.push_back(tool_schema(
        "dispatch_task",
        "Dispatch one role-scoped task. Judge roles mechanically require hash-verified FULL gated artifacts. Repairs require a Verifier/Integrator receipt and must pen a new artifact name. Artifact paths are relative to the declared output root. No shell is accepted.",
        {
            {"job_id", job_id},
            {"task_id", {{"type", "string"}}},
            {"role", {{"type", "string"}, {"enum", {kRoleBuilder, kRoleScout, kRoleAdversary, kRoleVerifier}}}},
            {"seat", {{"type", "string"}, {"description", "Semantic seat name such as solo or swarm."}}},
            {"prompt", {{"type", "string"}}},
            {"artifact_name", {{"type", "string"}, {"description", "Optional relative output filename. Absolute paths and traversal are refused."}}},
            {"artifact_contract_id", {{"type", "string"}, {"description", "Required operator-owned contract reference when artifact_name is present; forbidden otherwise."}}},
            {"input_artifact_ids", {{"type", "array"}, {"items", {{"type", "string"}}}}},
            {"input_result_task_ids",
             {{"type", "array"},
              {"items", {{"type", "string"}}},
              {"description", "Completed results the harness carries whole into the worker prompt by immutable task id and hash."}}},
            {"repair_of",
             {{"type", "string"},
              {"description", "Artifact id to repair. It must also appear in input_artifact_ids and already be gated."}}},
            {"error_receipt_task_id",
             {{"type", "string"},
              {"description", "Completed Verifier/Integrator task whose exact result becomes the repair receipt; preferred to pasting a long receipt."}}},
            {"exact_error_receipt", {{"type", "string"}}},
        },
        {"job_id", "task_id", "role", "seat", "prompt"}));
    tools.push_back(tool_schema(
        "get_result",
        "Retrieve complete task output and evidence. Artifact output includes identity, SHA-256, byte count, and FULL/CLIPPED state.",
        {
            {"job_id", job_id},
            {"task_id", {{"type", "string"}}},
            {"wait_ms", {{"type", "integer"}, {"minimum", 0}, {"maximum", 30000}}},
        },
        {"job_id", "task_id"},
        true));
    tools.push_back(tool_schema(
        "run_gate",
        "Run one operator-configured gate against a verified artifact. The interface accepts only a gate name, never a command.",
        {
            {"job_id", job_id},
            {"task_id", {{"type", "string"}}},
            {"gate_name", {{"type", "string"}}},
        },
        {"job_id", "task_id", "gate_name"}));
    tools.push_back(tool_schema(
        "finish_job",
        "Request terminal evaluation. Feel-artifact jobs can reach GATED but this tool can never produce DONE; DONE requires the operator's external adjudication outside this surface.",
        {
            {"job_id", job_id},
            {"summary", {{"type", "string"}}},
            {"contradictions", {{"type", "array"}, {"items", {{"type", "object"}}}}},
        },
        {"job_id", "summary"}));
    return tools;
}

json Harness::tool_definitions() const {
    json tools = json::array();
    for (const auto& definition : mcp_tool_definitions()) {
        tools.push_back(openai_tool(definition));
    }
    return tools;
}

json Harness::list_workers(const json& arguments) {
    enforce_job_id(arguments);
    json workers = json::array();
    std::lock_guard lock(mutex_);
    for (const auto& worker : config_.workers) {
        workers.push_back({
            {"id", worker.id},
            {"seat", worker.seat},
            {"endpoint", worker.endpoint},
            {"active", worker.active},
            {"capacity", worker.max_concurrency},
            {"available", worker.active < worker.max_concurrency},
            {"lifecycle_control", false},
        });
    }
    return {{"ok", true}, {"job_id", job_.job_id}, {"workers", workers}};
}

std::vector<ArtifactRecord> Harness::verify_judge_inputs_locked(
    const std::string& role,
    const std::vector<std::string>& artifact_ids,
    const std::string& task_id) {
    std::vector<ArtifactRecord> verified;
    if (is_judge_role(role) && artifact_ids.empty()) {
        throw std::runtime_error("judge dispatch " + task_id + " requires at least one FULL artifact");
    }
    for (const auto& id : artifact_ids) {
        const auto found = artifacts_.find(id);
        if (found == artifacts_.end()) {
            throw std::runtime_error("unknown input artifact: " + id);
        }
        const auto check = verify_artifact_locked(found->second);
        if (!check.at("verified").get<bool>()) {
            throw std::runtime_error("input artifact failed identity verification: " + id);
        }
        if (is_judge_role(role) && found->second.completeness != Completeness::full) {
            throw std::runtime_error("FULL-ARTIFACT LAW: judge dispatch refused CLIPPED input " + id);
        }
        if (is_judge_role(role)) {
            const auto source_task = tasks_.find(found->second.task_id);
            if (source_task == tasks_.end() || source_task->second->state != TaskState::gated) {
                throw std::runtime_error("GATE LAW: judge dispatch refused ungated input " + id);
            }
        }
        verified.push_back(found->second);
    }
    return verified;
}

json Harness::dispatch_task(const json& arguments) {
    enforce_job_id(arguments);
    require_only_keys(arguments, {
        "job_id", "task_id", "role", "seat", "prompt", "artifact_name", "artifact_contract_id",
        "input_artifact_ids", "input_result_task_ids", "repair_of", "error_receipt_task_id",
        "exact_error_receipt",
    }, "dispatch_task arguments");
    const auto task_id = arguments.at("task_id").get<std::string>();
    const auto role = arguments.at("role").get<std::string>();
    const auto seat = arguments.at("seat").get<std::string>();
    const auto prompt = arguments.at("prompt").get<std::string>();
    validate_id(task_id, "task_id");
    if (!kRoleDefinitions.contains(role)) {
        throw std::runtime_error("role is not in the operator's four-role taxonomy");
    }
    if (prompt.empty()) {
        throw std::runtime_error("prompt may not be empty");
    }
    const auto artifact_ids = json_string_array(arguments.value("input_artifact_ids", json::array()), "input_artifact_ids");
    const auto input_result_task_ids = json_string_array(
        arguments.value("input_result_task_ids", json::array()),
        "input_result_task_ids");
    std::optional<std::string> artifact_name;
    std::optional<std::string> artifact_contract_id;
    if (arguments.contains("artifact_name")) {
        artifact_name = arguments.at("artifact_name").get<std::string>();
        (void)resolve_artifact_path(*artifact_name);
        if (!arguments.contains("artifact_contract_id") || !arguments.at("artifact_contract_id").is_string()) {
            throw std::runtime_error("artifact dispatch requires exactly one non-empty artifact_contract_id");
        }
        artifact_contract_id = arguments.at("artifact_contract_id").get<std::string>();
        if (artifact_contract_id->empty()) {
            throw std::runtime_error("artifact dispatch requires exactly one non-empty artifact_contract_id");
        }
        validate_id(*artifact_contract_id, "artifact_contract_id");
        const auto contract = job_.artifact_contracts.find(*artifact_contract_id);
        if (contract == job_.artifact_contracts.end()) {
            throw std::runtime_error("unknown artifact_contract_id: " + *artifact_contract_id);
        }
        if (contract->second.artifact_name != *artifact_name) {
            throw std::runtime_error(
                "artifact contract name mismatch: " + *artifact_contract_id + " expects " +
                contract->second.artifact_name);
        }
    } else if (arguments.contains("artifact_contract_id")) {
        throw std::runtime_error("artifact_contract_id is forbidden without artifact_name");
    }
    std::optional<std::string> repair_of;
    std::optional<std::string> error_receipt_task_id;
    std::string exact_error_receipt;
    if (arguments.contains("repair_of")) {
        repair_of = arguments.at("repair_of").get<std::string>();
        exact_error_receipt = arguments.value("exact_error_receipt", "");
        if (arguments.contains("error_receipt_task_id")) {
            error_receipt_task_id = arguments.at("error_receipt_task_id").get<std::string>();
        }
        if (exact_error_receipt.empty() && !error_receipt_task_id) {
            throw std::runtime_error("repair dispatch requires exact_error_receipt or error_receipt_task_id");
        }
        if (artifact_ids.empty()) {
            throw std::runtime_error("repair dispatch requires the exact input artifact");
        }
        if (role != kRoleBuilder) {
            throw std::runtime_error("repair dispatch requires the Builder role");
        }
        if (!artifact_name) {
            throw std::runtime_error("repair dispatch requires a new artifact_name");
        }
        if (std::find(artifact_ids.begin(), artifact_ids.end(), *repair_of) == artifact_ids.end()) {
            throw std::runtime_error("repair_of must name an artifact in input_artifact_ids");
        }
    }

    auto task = std::make_shared<TaskRecord>();
    task->id = task_id;
    task->role = role;
    task->seat = seat;
    task->prompt = prompt;
    task->input_artifact_ids = artifact_ids;
    task->input_result_task_ids = input_result_task_ids;
    task->artifact_name = artifact_name;
    task->artifact_contract_id = artifact_contract_id;
    if (artifact_contract_id) task->artifact_contract_sha256 = job_.artifact_contracts.at(*artifact_contract_id).sha256;
    task->repair_of = repair_of;
    task->error_receipt_task_id = error_receipt_task_id;
    task->exact_error_receipt = exact_error_receipt;

    {
        std::lock_guard lock(mutex_);
        enforce_caps_locked();
        if (job_state_ != JobState::running) {
            auto reason = "job is already terminal: " + to_string(job_state_);
            if (!artifact_refusal_task_id_.empty()) {
                reason += "; originating artifact refusal task=" + artifact_refusal_task_id_ +
                          " failed_check=" + artifact_refusal_check_;
            }
            throw std::runtime_error(reason);
        }
        if (tasks_.contains(task_id)) {
            throw std::runtime_error("task_id already exists: " + task_id);
        }
        const bool seat_exists = std::any_of(config_.workers.begin(), config_.workers.end(), [&](const auto& worker) {
            return worker.seat == seat;
        });
        if (!seat_exists) {
            throw std::runtime_error("unknown semantic seat: " + seat);
        }
        (void)verify_judge_inputs_locked(role, artifact_ids, task_id);
        if (repair_of) {
            const auto source = artifacts_.find(*repair_of);
            if (source == artifacts_.end()) {
                throw std::runtime_error("repair source artifact is missing: " + *repair_of);
            }
            const auto source_task = tasks_.find(source->second.task_id);
            if (source_task == tasks_.end() || source_task->second->state != TaskState::gated) {
                throw std::runtime_error("repair source artifact is not gated: " + *repair_of);
            }
            if (resolve_artifact_path(*artifact_name) == source->second.path) {
                throw std::runtime_error("repair must pen a new immutable artifact_name");
            }
        }
        for (const auto& result_task_id : input_result_task_ids) {
            const auto result_task = tasks_.find(result_task_id);
            if (result_task == tasks_.end() || !terminal(result_task->second->state) ||
                result_task->second->state == TaskState::failed ||
                result_task->second->state == TaskState::refused) {
                throw std::runtime_error("input result task is missing, active, or failed: " + result_task_id);
            }
        }
        if (error_receipt_task_id) {
            const auto receipt_task = tasks_.find(*error_receipt_task_id);
            if (receipt_task == tasks_.end() || !terminal(receipt_task->second->state) ||
                receipt_task->second->state == TaskState::failed ||
                receipt_task->second->state == TaskState::refused) {
                throw std::runtime_error("error receipt task is missing, active, or failed: " + *error_receipt_task_id);
            }
            if (receipt_task->second->role != kRoleVerifier) {
                throw std::runtime_error(
                    "error receipt task must be a completed Verifier/Integrator task: " +
                    *error_receipt_task_id);
            }
            const auto& canonical_receipt = receipt_task->second->content;
            if (!exact_error_receipt.empty() && exact_error_receipt != canonical_receipt) {
                throw std::runtime_error("pasted exact_error_receipt does not match error_receipt_task_id content");
            }
            task->exact_error_receipt = canonical_receipt;
        }
        tasks_.emplace(task_id, task);
        ++task_count_;
        futures_.push_back(std::async(std::launch::async, [this, task] { execute_task(task); }));
    }

    journal({
        {"event", "task_dispatched"},
        {"task_id", task_id},
        {"role", role},
        {"seat", seat},
        {"outcome", "QUEUED"},
        {"prompt_sha256", sha256_text(prompt)},
        {"prompt_bytes", prompt.size()},
        {"input_artifact_ids", artifact_ids},
        {"input_result_task_ids", input_result_task_ids},
        {"artifact_name", artifact_name ? json(*artifact_name) : json(nullptr)},
        {"artifact_contract_id", artifact_contract_id ? json(*artifact_contract_id) : json(nullptr)},
        {"artifact_contract_sha256", task->artifact_contract_sha256},
        {"repair_of", repair_of ? json(*repair_of) : json(nullptr)},
        {"error_receipt_task_id", error_receipt_task_id ? json(*error_receipt_task_id) : json(nullptr)},
        {"error_receipt_sha256", task->exact_error_receipt.empty() ? "" : sha256_text(task->exact_error_receipt)},
    });
    return {
        {"ok", true}, {"job_id", job_.job_id}, {"task_id", task_id}, {"state", "QUEUED"},
        {"artifact_contract_id", artifact_contract_id ? json(*artifact_contract_id) : json(nullptr)},
        {"artifact_contract_sha256", task->artifact_contract_sha256},
    };
}

WorkerConfig* Harness::acquire_worker(const std::string& seat) {
    std::unique_lock lock(mutex_);
    worker_cv_.wait(lock, [&] {
        if (wall_clock_expired()) return true;
        if (active_workers_ >= config_.caps.max_concurrent_workers) return false;
        return std::any_of(config_.workers.begin(), config_.workers.end(), [&](const auto& worker) {
            return worker.seat == seat && worker.active < worker.max_concurrency;
        });
    });
    if (wall_clock_expired()) {
        throw std::runtime_error("max_wall_seconds cap reached while waiting for worker");
    }
    auto found = std::find_if(config_.workers.begin(), config_.workers.end(), [&](const auto& worker) {
        return worker.seat == seat && worker.active < worker.max_concurrency;
    });
    if (found == config_.workers.end()) {
        throw std::runtime_error("no worker available for seat: " + seat);
    }
    ++found->active;
    ++active_workers_;
    return &*found;
}

void Harness::release_worker(WorkerConfig* worker) {
    {
        std::lock_guard lock(mutex_);
        --worker->active;
        --active_workers_;
    }
    worker_cv_.notify_all();
}

void Harness::execute_task(const std::shared_ptr<TaskRecord>& task) {
    WorkerConfig* worker = nullptr;
    const auto task_started = std::chrono::steady_clock::now();
    try {
        worker = acquire_worker(task->seat);
        {
            std::lock_guard lock(mutex_);
            task->worker_id = worker->id;
            task->state = TaskState::running;
        }
        task->cv.notify_all();

        std::ostringstream user;
        user << "MISSION:\n" << job_.mission << "\n\n";
        user << "ROLE: " << task->role << " - " << kRoleDefinitions.at(task->role) << "\n";
        user << "TASK ID: " << task->id << "\n\n";
        if (task->repair_of) {
            user << "REPAIR CONTRACT: Repair only the smallest defect justified by the exact receipt. "
                 << "Preserve everything else.\nREPAIR OF: " << *task->repair_of
                 << "\nEXACT ERROR/RECEIPT:\n" << task->exact_error_receipt << "\n\n";
        }
        user << "TASK:\n" << task->prompt << "\n";

        std::vector<ArtifactRecord> inputs;
        json result_receipts = json::array();
        {
            std::lock_guard lock(mutex_);
            inputs = verify_judge_inputs_locked(task->role, task->input_artifact_ids, task->id);
            for (const auto& result_task_id : task->input_result_task_ids) {
                const auto result_task = tasks_.find(result_task_id);
                if (result_task == tasks_.end() || !terminal(result_task->second->state) ||
                    result_task->second->state == TaskState::failed ||
                    result_task->second->state == TaskState::refused) {
                    throw std::runtime_error("input result changed before execution: " + result_task_id);
                }
                result_receipts.push_back({
                    {"task_id", result_task_id},
                    {"sha256", sha256_text(result_task->second->content)},
                    {"byte_count", result_task->second->content.size()},
                    {"content", result_task->second->content},
                });
            }
        }
        for (const auto& artifact : inputs) {
            user << "\n--- FULL ARTIFACT " << artifact.id
                 << " sha256=" << artifact.sha256
                 << " bytes=" << artifact.byte_count
                 << " completeness=" << to_string(artifact.completeness) << " ---\n";
            user << read_text(artifact.path, config_.caps.max_artifact_bytes);
            user << "\n--- END FULL ARTIFACT " << artifact.id << " ---\n";
        }
        for (const auto& receipt : result_receipts) {
            user << "\n--- COMPLETE RESULT RECEIPT " << receipt.at("task_id").get<std::string>()
                 << " sha256=" << receipt.at("sha256").get<std::string>()
                 << " bytes=" << receipt.at("byte_count") << " ---\n"
                 << receipt.at("content").get<std::string>()
                 << "\n--- END COMPLETE RESULT RECEIPT "
                 << receipt.at("task_id").get<std::string>() << " ---\n";
        }
        if (task->artifact_name) {
            user << "\nReturn the COMPLETE artifact only, with no Markdown fence or commentary. "
                 << "If you cannot fit the complete artifact, stop rather than pretending it is complete.";
        } else {
            user << "\nReturn a complete plain-text result. Do not emit tool calls or tool-call-shaped JSON.";
        }

        const int remaining_wall = std::max(1, config_.caps.max_wall_seconds - static_cast<int>(
            std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - started_).count()));
        json request{
            {"model", worker->model},
            {"messages", {
                {{"role", "system"}, {"content", worker->system_anchor}},
                {{"role", "user"}, {"content", user.str()}},
            }},
            {"temperature", 0.2},
            {"max_tokens", config_.caps.max_tokens_per_task},
            {"stream", false},
            {"chat_template_kwargs", {{"enable_thinking", false}}},
        };
        const auto response = http_chat_completion(worker->endpoint, request, remaining_wall);
        const auto content = message_content(response);
        const auto finish_reason = response.at("choices").at(0).value("finish_reason", "unknown");
        const auto completeness = finish_reason == "length" ? Completeness::clipped : Completeness::full;
        const int prompt_tokens = response.value("usage", json::object()).value("prompt_tokens", 0);
        const int completion_tokens = response.value("usage", json::object()).value("completion_tokens", 0);
        if (completion_tokens > config_.caps.max_tokens_per_task) {
            throw std::runtime_error("endpoint exceeded max_tokens_per_task cap");
        }

        std::optional<ArtifactRecord> artifact;
        std::optional<fs::path> result_path;
        std::vector<ArtifactCheck> artifact_checks;
        std::string failed_artifact_check;
        if (task->artifact_name) {
            artifact = persist_artifact(task, content, completeness);
            const auto& contract = job_.artifact_contracts.at(*task->artifact_contract_id);
            artifact_checks = validate_artifact(*artifact, contract);
            const auto failed = std::find_if(artifact_checks.begin(), artifact_checks.end(), [](const auto& check) {
                return !check.passed;
            });
            if (failed != artifact_checks.end()) failed_artifact_check = failed->name;
        } else {
            const auto results_dir = output_root_ / ".results";
            fs::create_directories(results_dir);
            result_path = results_dir / (task->id + ".txt");
            write_exclusive(*result_path, content);
        }
        {
            std::lock_guard lock(mutex_);
            task->content = content;
            task->prompt_tokens = prompt_tokens;
            task->completion_tokens = completion_tokens;
            task->duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - task_started).count();
            task->artifact = artifact;
            task->artifact_checks = artifact_checks;
            task->failed_artifact_check = failed_artifact_check;
            if (artifact) {
                if (!failed_artifact_check.empty()) {
                    task->state = TaskState::refused;
                    task->error = "artifact contract refused output at " + failed_artifact_check;
                    job_state_ = JobState::failed;
                    ++artifact_refusal_count_;
                    if (artifact_refusal_task_id_.empty()) {
                        artifact_refusal_task_id_ = task->id;
                        artifact_refusal_check_ = failed_artifact_check;
                    }
                } else {
                    artifacts_[artifact->id] = *artifact;
                    task->state = TaskState::penned;
                }
            } else if (completeness == Completeness::clipped) {
                task->state = TaskState::failed;
                task->error = "non-artifact result was clipped at the token cap";
            } else {
                task->state = TaskState::succeeded;
            }
        }
        json completion_event{
            {"event", failed_artifact_check.empty() ? "task_completed" : "artifact_refused"},
            {"task_id", task->id},
            {"role", task->role},
            {"seat", task->seat},
            {"worker", worker->id},
            {"outcome", to_string(task->state)},
            {"prompt_tokens", prompt_tokens},
            {"completion_tokens", completion_tokens},
            {"duration_ms", task->duration_ms},
            {"finish_reason", finish_reason},
            {"result_sha256", sha256_text(content)},
            {"result_bytes", content.size()},
            {"result_path", result_path ? json(fs::relative(*result_path, output_root_).string()) : json(nullptr)},
            {"completeness", to_string(completeness)},
            {"artifact_contract_id", task->artifact_contract_id ? json(*task->artifact_contract_id) : json(nullptr)},
            {"artifact_contract_sha256", task->artifact_contract_sha256},
            {"artifact_checks", artifact_checks_json(artifact_checks)},
            {"failed_check", failed_artifact_check.empty() ? json(nullptr) : json(failed_artifact_check)},
            {"artifact", artifact ? json{
                {"id", artifact->id},
                {"path", fs::relative(artifact->path, output_root_).string()},
                {"sha256", artifact->sha256},
                {"byte_count", artifact->byte_count},
                {"completeness", to_string(artifact->completeness)},
            } : json(nullptr)},
            {"task_state", to_string(task->state)},
            {"job_state", to_string(job_state_)},
            {"artifact_refusal_count", artifact_refusal_count_},
        };
        journal(std::move(completion_event));
        if (!failed_artifact_check.empty()) {
            journal({
                {"event", "job_failed"}, {"outcome", "FAILED"},
                {"reason", "fail-closed artifact contract refusal"},
                {"task_id", task->id}, {"failed_check", failed_artifact_check},
                {"artifact_refusal_count", artifact_refusal_count_},
            });
        }
    } catch (const std::exception& error) {
        {
            std::lock_guard lock(mutex_);
            task->state = TaskState::failed;
            task->error = error.what();
            task->duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - task_started).count();
        }
        try {
            journal({
                {"event", "task_failed"},
                {"task_id", task->id},
                {"role", task->role},
                {"seat", task->seat},
                {"worker", worker ? worker->id : "none"},
                {"outcome", "FAILED"},
                {"reason", error.what()},
                {"duration_ms", task->duration_ms},
            });
        } catch (...) {
        }
    }
    if (worker != nullptr) {
        release_worker(worker);
    }
    task->cv.notify_all();
}

fs::path Harness::resolve_artifact_path(const std::string& relative) const {
    if (relative.empty()) {
        throw std::runtime_error("artifact_name may not be empty");
    }
    const fs::path requested(relative);
    if (requested.is_absolute()) {
        throw std::runtime_error("absolute artifact paths are forbidden");
    }
    const auto normalized = requested.lexically_normal();
    if (normalized.empty() || normalized == "." ||
        std::any_of(normalized.begin(), normalized.end(), [](const auto& part) { return part == ".."; })) {
        throw std::runtime_error("artifact path traversal is forbidden");
    }
    const auto first = normalized.begin()->string();
    if (first.empty() || first.front() == '.' || normalized == "journal.jsonl") {
        throw std::runtime_error("artifact path collides with a reserved harness path");
    }
    const auto candidate = output_root_ / normalized;
    const auto parent = candidate.parent_path();
    fs::create_directories(parent);
    if (!path_within(output_root_, parent)) {
        throw std::runtime_error("artifact path escapes declared output root");
    }
    if (fs::exists(candidate) && fs::is_symlink(fs::symlink_status(candidate))) {
        throw std::runtime_error("artifact path may not be a symlink");
    }
    return candidate;
}

ArtifactRecord Harness::persist_artifact(
    const std::shared_ptr<TaskRecord>& task,
    const std::string& content,
    Completeness completeness) {
    if (content.size() > config_.caps.max_artifact_bytes) {
        throw std::runtime_error("artifact exceeds max_artifact_bytes cap");
    }
    const auto path = resolve_artifact_path(*task->artifact_name);
    write_exclusive(path, content);
    ArtifactRecord artifact;
    artifact.id = task->id;
    artifact.task_id = task->id;
    artifact.path = fs::weakly_canonical(path);
    artifact.sha256 = sha256_file(artifact.path);
    artifact.byte_count = fs::file_size(artifact.path);
    artifact.completeness = completeness;
    return artifact;
}

std::vector<ArtifactCheck> Harness::validate_artifact(
    const ArtifactRecord& artifact,
    const ArtifactContract& contract) {
    std::vector<ArtifactCheck> checks;
    const auto add = [&](std::string name, bool passed, std::string output = {}) {
        ArtifactCheck check;
        check.name = std::move(name);
        check.passed = passed;
        check.exit_code = passed ? 0 : 1;
        check.output = std::move(output);
        checks.push_back(std::move(check));
    };
    add("completeness", artifact.completeness == Completeness::full, to_string(artifact.completeness));
    add(
        "minimum_bytes",
        artifact.byte_count >= contract.minimum_bytes,
        std::to_string(artifact.byte_count) + " >= " + std::to_string(contract.minimum_bytes));

    bool format_passed = false;
    std::string format_output;
    try {
        const auto content = read_text(artifact.path, config_.caps.max_artifact_bytes);
        switch (contract.format) {
            case ArtifactFormat::html:
                // The format tap is the deterministic markup-shaped prefix described
                // above. Operator-declared invariant hooks run next as validators.
                format_passed = valid_utf8_without_nul(content) && html_prefix_matches(content);
                format_output = format_passed ? "UTF-8 HTML prefix accepted" : "invalid UTF-8 or HTML prefix";
                break;
            case ArtifactFormat::json_document: {
                const auto parsed = json::parse(content);
                (void)parsed;
                format_passed = true;
                format_output = "JSON parsed";
                break;
            }
            case ArtifactFormat::text:
                format_passed = valid_utf8_without_nul(content);
                format_output = format_passed ? "UTF-8 text accepted" : "invalid UTF-8 text or NUL byte";
                break;
            case ArtifactFormat::file:
                format_passed = true;
                format_output = "opaque file accepted";
                break;
        }
    } catch (const std::exception& error) {
        format_output = error.what();
    }
    add("format:" + to_string(contract.format), format_passed, format_output);

    for (const auto& validator_name : contract.validators) {
        ArtifactCheck check;
        check.name = "validator:" + validator_name;
        try {
            const auto elapsed = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - started_).count());
            const auto remaining = config_.caps.max_wall_seconds - elapsed;
            if (remaining <= 0) throw std::runtime_error("aggregate validation wall budget exhausted");
            const auto& validator = config_.gates.at(validator_name);
            const auto validator_dir = output_root_ / ".validators" / artifact.task_id / validator_name;
            fs::create_directories(validator_dir);
            if (!path_within(output_root_, validator_dir)) {
                throw std::runtime_error("internal validator directory escaped output root");
            }
            std::vector<std::string> argv;
            argv.reserve(validator.argv.size());
            for (auto argument : validator.argv) {
                argument = replace_all(argument, "{artifact}", artifact.path.string());
                argument = replace_all(argument, "{output_dir}", output_root_.string());
                argument = replace_all(argument, "{gate_dir}", validator_dir.string());
                argument = replace_all(argument, "{organ_dir}", config_.organ_dir.string());
                argv.push_back(std::move(argument));
            }
            const auto process = run_process(argv, output_root_, std::min(validator.timeout_seconds, remaining));
            check.exit_code = process.exit_code;
            check.timed_out = process.timed_out;
            check.output = process.output;
            bool receipt_valid = false;
            try {
                const auto receipt = json::parse(process.output);
                receipt_valid = receipt.is_object() && receipt.contains("ok") && receipt.at("ok").is_boolean() &&
                                receipt.at("ok").get<bool>() == (process.exit_code == 0 && !process.timed_out);
            } catch (const std::exception&) {
            }
            check.passed = process.exit_code == 0 && !process.timed_out && receipt_valid;
            if (!receipt_valid) check.output += "\n[malformed validator receipt]";
        } catch (const std::exception& error) {
            check.output = error.what();
            check.exit_code = -1;
            check.passed = false;
        }
        checks.push_back(std::move(check));
    }
    return checks;
}

json Harness::verify_artifact_locked(const ArtifactRecord& artifact) const {
    if (!path_within(output_root_, artifact.path) || !fs::is_regular_file(artifact.path)) {
        return {{"verified", false}, {"reason", "artifact path missing or outside output root"}};
    }
    const auto bytes = fs::file_size(artifact.path);
    const auto hash = sha256_file(artifact.path);
    const bool verified = bytes == artifact.byte_count && hash == artifact.sha256;
    return {
        {"verified", verified},
        {"sha256", hash},
        {"byte_count", bytes},
        {"expected_sha256", artifact.sha256},
        {"expected_byte_count", artifact.byte_count},
        {"completeness", to_string(artifact.completeness)},
    };
}

json Harness::task_result_locked(const std::shared_ptr<TaskRecord>& task, bool include_content) {
    json result{
        {"ok", task->state != TaskState::failed && task->state != TaskState::refused},
        {"job_id", job_.job_id},
        {"task_id", task->id},
        {"role", task->role},
        {"seat", task->seat},
        {"worker", task->worker_id},
        {"state", to_string(task->state)},
        {"prompt_tokens", task->prompt_tokens},
        {"completion_tokens", task->completion_tokens},
        {"duration_ms", task->duration_ms},
        {"error", task->error},
        {"artifact_contract_id", task->artifact_contract_id ? json(*task->artifact_contract_id) : json(nullptr)},
        {"artifact_contract_sha256", task->artifact_contract_sha256},
        {"artifact_checks", artifact_checks_json(task->artifact_checks)},
        {"failed_check", task->failed_artifact_check.empty() ? json(nullptr) : json(task->failed_artifact_check)},
        {"job_state", to_string(job_state_)},
        {"artifact_refusal_count", artifact_refusal_count_},
    };
    if (include_content && terminal(task->state)) {
        result["content"] = task->content;
        result["content_sha256"] = sha256_text(task->content);
        result["content_bytes"] = task->content.size();
    }
    if (task->artifact) {
        const auto verified = verify_artifact_locked(*task->artifact);
        result["artifact"] = {
            {"id", task->artifact->id},
            {"path", fs::relative(task->artifact->path, output_root_).string()},
            {"sha256", task->artifact->sha256},
            {"byte_count", task->artifact->byte_count},
            {"completeness", to_string(task->artifact->completeness)},
            {"identity_check", verified},
        };
        if (!verified.at("verified").get<bool>()) {
            result["ok"] = false;
            result["error"] = "artifact identity verification failed";
        }
    }
    json gates = json::array();
    for (const auto& gate : task->gates) {
        gates.push_back({
            {"name", gate.name},
            {"passed", gate.passed},
            {"exit_code", gate.exit_code},
            {"timed_out", gate.timed_out},
            {"output", gate.output},
        });
    }
    result["gates"] = gates;
    return result;
}

json Harness::get_result(const json& arguments) {
    enforce_job_id(arguments);
    const auto task_id = arguments.at("task_id").get<std::string>();
    const auto wait_ms = std::clamp(arguments.value("wait_ms", 0), 0, 30000);
    std::shared_ptr<TaskRecord> task;
    std::unique_lock lock(mutex_);
    const auto found = tasks_.find(task_id);
    if (found == tasks_.end()) {
        throw std::runtime_error("unknown task_id: " + task_id);
    }
    task = found->second;
    if (!terminal(task->state) && wait_ms > 0) {
        task->cv.wait_for(lock, std::chrono::milliseconds(wait_ms), [&] { return terminal(task->state); });
    }
    const auto result = task_result_locked(task, true);
    lock.unlock();
    journal({
        {"event", "result_retrieved"},
        {"task_id", task_id},
        {"outcome", result.at("state")},
        {"content_sha256", result.value("content_sha256", "")},
        {"content_bytes", result.value("content_bytes", 0)},
    });
    return result;
}

ProcessResult run_process(const std::vector<std::string>& argv, const fs::path& cwd, int timeout_seconds) {
    if (argv.empty()) {
        throw std::runtime_error("cannot run empty argv");
    }
    int pipe_fds[2]{};
    if (::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) != 0) {
        throw std::runtime_error("pipe2 failed: " + std::string(std::strerror(errno)));
    }
    const pid_t child = ::fork();
    if (child < 0) {
        ::close(pipe_fds[0]);
        ::close(pipe_fds[1]);
        throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    }
    if (child == 0) {
        ::close(pipe_fds[0]);
        ::dup2(pipe_fds[1], STDOUT_FILENO);
        ::dup2(pipe_fds[1], STDERR_FILENO);
        ::close(pipe_fds[1]);
        if (::chdir(cwd.c_str()) != 0) {
            _exit(126);
        }
        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& argument : argv) {
            args.push_back(const_cast<char*>(argument.c_str()));
        }
        args.push_back(nullptr);
        ::execvp(args[0], args.data());
        _exit(127);
    }

    ::close(pipe_fds[1]);
    ProcessResult result;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout_seconds);
    int status = 0;
    bool finished = false;
    std::array<char, 4096> buffer{};
    while (!finished) {
        while (true) {
            const auto count = ::read(pipe_fds[0], buffer.data(), buffer.size());
            if (count > 0) {
                result.output.append(buffer.data(), static_cast<std::size_t>(count));
            } else {
                break;
            }
        }
        const auto waited = ::waitpid(child, &status, WNOHANG);
        if (waited == child) {
            finished = true;
            break;
        }
        if (waited < 0) {
            ::close(pipe_fds[0]);
            throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            result.timed_out = true;
            ::kill(child, SIGKILL);
            ::waitpid(child, &status, 0);
            finished = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    while (true) {
        const auto count = ::read(pipe_fds[0], buffer.data(), buffer.size());
        if (count > 0) result.output.append(buffer.data(), static_cast<std::size_t>(count));
        else break;
    }
    ::close(pipe_fds[0]);
    if (result.timed_out) {
        result.exit_code = 124;
    } else if (WIFEXITED(status)) {
        result.exit_code = WEXITSTATUS(status);
    } else if (WIFSIGNALED(status)) {
        result.exit_code = 128 + WTERMSIG(status);
    }
    if (result.output.size() > 64 * 1024) {
        result.output.resize(64 * 1024);
        result.output += "\n[gate output clipped at 65536 bytes]";
    }
    return result;
}

json Harness::run_gate(const json& arguments) {
    enforce_job_id(arguments);
    const auto task_id = arguments.at("task_id").get<std::string>();
    const auto gate_name = arguments.at("gate_name").get<std::string>();
    std::shared_ptr<TaskRecord> task;
    ArtifactRecord artifact;
    GateConfig gate;
    {
        std::lock_guard lock(mutex_);
        if (job_state_ != JobState::running) {
            throw std::runtime_error("fail-closed: job is already terminal: " + to_string(job_state_));
        }
        const auto task_found = tasks_.find(task_id);
        if (task_found == tasks_.end()) {
            throw std::runtime_error("unknown task_id: " + task_id);
        }
        task = task_found->second;
        if (task->state != TaskState::penned && task->state != TaskState::gated) {
            throw std::runtime_error("gate requires a completed artifact task; current state is " + to_string(task->state));
        }
        if (!task->artifact) {
            throw std::runtime_error("task has no artifact to gate");
        }
        const auto verified = verify_artifact_locked(*task->artifact);
        if (!verified.at("verified").get<bool>()) {
            task->state = TaskState::failed;
            throw std::runtime_error("artifact identity changed before gate");
        }
        const auto gate_found = config_.gates.find(gate_name);
        if (gate_found == config_.gates.end()) {
            throw std::runtime_error("unknown configured gate: " + gate_name);
        }
        artifact = *task->artifact;
        gate = gate_found->second;
    }

    const auto gate_dir = output_root_ / ".gates" / task_id / gate_name;
    fs::create_directories(gate_dir);
    if (!path_within(output_root_, gate_dir)) {
        throw std::runtime_error("internal gate directory escaped output root");
    }
    std::vector<std::string> argv;
    argv.reserve(gate.argv.size());
    for (auto argument : gate.argv) {
        argument = replace_all(argument, "{artifact}", artifact.path.string());
        argument = replace_all(argument, "{output_dir}", output_root_.string());
        argument = replace_all(argument, "{gate_dir}", gate_dir.string());
        argument = replace_all(argument, "{organ_dir}", config_.organ_dir.string());
        argv.push_back(std::move(argument));
    }
    const auto process = run_process(argv, output_root_, gate.timeout_seconds);
    GateResult result;
    result.name = gate_name;
    result.exit_code = process.exit_code;
    result.timed_out = process.timed_out;
    result.output = process.output;
    result.passed = process.exit_code == 0 && !process.timed_out;
    {
        std::lock_guard lock(mutex_);
        task->gates.push_back(result);
        task->state = result.passed ? TaskState::gated : TaskState::failed;
        if (!result.passed) {
            job_state_ = JobState::failed;
        }
    }
    journal({
        {"event", "gate_completed"},
        {"task_id", task_id},
        {"artifact_id", artifact.id},
        {"artifact_sha256", artifact.sha256},
        {"artifact_byte_count", artifact.byte_count},
        {"gate", gate_name},
        {"exit_code", result.exit_code},
        {"timed_out", result.timed_out},
        {"outcome", result.passed ? "PASSED" : "FAILED"},
        {"evidence", result.output},
    });
    if (!result.passed) {
        journal({
            {"event", "job_failed"},
            {"outcome", "FAILED"},
            {"reason", "fail-closed gate failure"},
            {"task_id", task_id},
            {"gate", gate_name},
        });
    }
    return {
        {"ok", result.passed},
        {"job_id", job_.job_id},
        {"task_id", task_id},
        {"gate", gate_name},
        {"passed", result.passed},
        {"exit_code", result.exit_code},
        {"timed_out", result.timed_out},
        {"evidence", result.output},
        {"task_state", to_string(task->state)},
    };
}

json Harness::finish_job(const json& arguments) {
    enforce_job_id(arguments);
    const auto summary = arguments.at("summary").get<std::string>();
    const auto contradictions = arguments.value("contradictions", json::array());
    if (!contradictions.is_array()) {
        throw std::runtime_error("contradictions must be an array and remain unflattened");
    }
    json ungated = json::array();
    json failed = json::array();
    json refused = json::array();
    {
        std::lock_guard lock(mutex_);
        if (tasks_.empty()) {
            throw std::runtime_error("cannot finish an empty job");
        }
        bool has_artifact = false;
        for (const auto& [id, task] : tasks_) {
            if (task->state == TaskState::queued || task->state == TaskState::running) {
                throw std::runtime_error("cannot finish while task is active: " + id);
            }
            if (task->artifact) {
                has_artifact = true;
                if (task->state == TaskState::refused) {
                    // Contract refusal is already terminal evidence; finish_job
                    // must never rewrite it into the older generic FAILED state.
                } else if (task->artifact->completeness != Completeness::full) {
                    task->state = TaskState::failed;
                    task->error = "artifact is CLIPPED and cannot reach a successful terminal state";
                    ungated.push_back(id);
                } else if (task->state != TaskState::gated && task->state != TaskState::failed) {
                    task->state = TaskState::failed;
                    task->error = "artifact had no passing gate at finish_job";
                    ungated.push_back(id);
                }
            }
            if (task->state == TaskState::failed || task->state == TaskState::refused) {
                failed.push_back(id);
            }
            if (task->state == TaskState::refused) refused.push_back(id);
        }
        if (job_.feel_artifact && !has_artifact) {
            throw std::runtime_error("feel-artifact job requires at least one FULL gated artifact");
        }
        if (!failed.empty() || !ungated.empty()) {
            job_state_ = JobState::failed;
        } else if (job_.feel_artifact) {
            job_state_ = JobState::gated;
        } else {
            job_state_ = JobState::done;
        }
    }
    journal({
        {"event", "job_finished"},
        {"outcome", to_string(job_state_)},
        {"feel_artifact", job_.feel_artifact},
        {"summary", summary},
        {"contradictions", contradictions},
        {"failed_tasks", failed},
        {"refused_tasks", refused},
        {"artifact_refusal_count", artifact_refusal_count_},
        {"ungated_tasks_marked_failed", ungated},
        {"authority_invariant", job_.feel_artifact
            ? "finish_job cannot produce DONE; external operator adjudication is required"
            : "non-feel job may reach DONE after all configured gates pass"},
    });
    return {
        {"ok", job_state_ != JobState::failed},
        {"job_id", job_.job_id},
        {"state", to_string(job_state_)},
        {"feel_artifact", job_.feel_artifact},
        {"done_possible_through_this_surface", !job_.feel_artifact},
        {"failed_tasks", failed},
        {"refused_tasks", refused},
        {"artifact_refusal_count", artifact_refusal_count_},
        {"ungated_tasks_marked_failed", ungated},
        {"contradictions", contradictions},
    };
}

json Harness::status() const {
    std::lock_guard lock(mutex_);
    json tasks = json::array();
    for (const auto& [id, task] : tasks_) {
        tasks.push_back({{"task_id", id}, {"state", to_string(task->state)}, {"role", task->role}, {"seat", task->seat}});
    }
    return {
        {"job_id", job_.job_id},
        {"state", to_string(job_state_)},
        {"feel_artifact", job_.feel_artifact},
        {"task_count", task_count_},
        {"active_workers", active_workers_},
        {"artifact_refusal_count", artifact_refusal_count_},
        {"artifact_refusal_task_id", artifact_refusal_task_id_},
        {"artifact_refusal_failed_check", artifact_refusal_check_},
        {"tasks", tasks},
    };
}

void Harness::wait_all() {
    for (auto& future : futures_) {
        if (future.valid()) {
            future.get();
        }
    }
}

int run_scripted(Harness& harness) {
    const auto& tasks = harness.job().scripted_tasks;
    for (const auto& task : tasks) {
        json arguments = task;
        arguments["job_id"] = harness.job().job_id;
        arguments.erase("gates");
        const auto result = harness.call_tool("dispatch_task", arguments);
        if (!result.value("ok", false)) {
            std::cerr << result.dump(2) << '\n';
            return 2;
        }
    }
    harness.wait_all();
    for (const auto& task : tasks) {
        const auto task_id = task.at("task_id").get<std::string>();
        const auto result = harness.call_tool("get_result", {
            {"job_id", harness.job().job_id}, {"task_id", task_id}, {"wait_ms", 0}});
        std::cout << result.dump() << '\n';
        for (const auto& gate : task.value("gates", json::array())) {
            const auto gated = harness.call_tool("run_gate", {
                {"job_id", harness.job().job_id}, {"task_id", task_id}, {"gate_name", gate}});
            std::cout << gated.dump() << '\n';
        }
    }
    const auto finished = harness.call_tool("finish_job", {
        {"job_id", harness.job().job_id},
        {"summary", "Scripted task list completed; terminal state is gate-derived."},
        {"contradictions", json::array()},
    });
    std::cout << finished.dump(2) << '\n';
    return finished.value("ok", false) ? 0 : 3;
}

int run_director(Harness& harness) {
    const auto& director = harness.config().director;
    if (director.endpoint.empty()) {
        throw std::runtime_error("director endpoint is missing from config");
    }
    std::ostringstream contract_references;
    for (const auto& [id, contract] : harness.job().artifact_contracts) {
        contract_references << "\n- " << contract.artifact_name << " -> " << id;
    }
    json messages = json::array({
        {
            {"role", "system"},
            {"content", director.system_anchor +
                " You direct labor through exactly five tools. Never invent shell access. "
                "Use semantic seat solo for sequential judgment/repair and swarm for parallel builders. "
                "After every artifact pen, call run_gate before judging or repairing it. Retrieve complete results before judging. "
                "Every artifact dispatch must reference the exact predeclared artifact_contract_id for its artifact_name. "
                "Adversary and Verifier/Integrator must receive FULL gated artifact ids. "
                "Use input_result_task_ids to carry prior reports; never paste artifact or report bodies into tool arguments. "
                "Repair calls must use a completed Verifier/Integrator error_receipt_task_id, name the source in repair_of and input_artifact_ids, and pen a new unique artifact_name, plus a smallest-fix instruction. "
                "Preserve disagreements as unflattened contradictions in finish_job. "
                "For a feel-artifact, finish_job must end at GATED, never DONE."},
        },
        {
            {"role", "user"},
            {"content", "JOB ID: " + harness.job().job_id + "\nMISSION:\n" + harness.job().mission +
                "\nPREDECLARED ARTIFACT REFERENCES (ids only; checks remain outside model context):" +
                (contract_references.str().empty() ? " none" : contract_references.str()) +
                "\nDrive the job end to end. Start by calling list_workers."},
        },
    });

    for (int step = 0; step < director.max_steps; ++step) {
        json request{
            {"model", director.model},
            {"messages", messages},
            {"tools", harness.tool_definitions()},
            {"tool_choice", "auto"},
            {"parallel_tool_calls", true},
            {"temperature", director.temperature},
            {"max_tokens", director.max_tokens},
            {"stream", false},
            {"chat_template_kwargs", {{"enable_thinking", false}}},
        };
        const auto response = http_chat_completion(director.endpoint, request, harness.config().caps.max_wall_seconds);
        const auto message = response.at("choices").at(0).at("message");
        // Everything already in messages was delivered to the director in the
        // request that just completed. Retain immutable receipts in subsequent
        // turns, but do not repeatedly spend context on bodies that are already
        // journaled or stored as artifacts. The newly returned message is added
        // afterwards so its tool arguments remain complete for execution below.
        compact_delivered_director_history(messages);
        messages.push_back(message);
        if (!message.contains("tool_calls") || !message.at("tool_calls").is_array() || message.at("tool_calls").empty()) {
            const auto current = harness.status();
            if (current.at("state") != "RUNNING") {
                std::cout << current.dump(2) << '\n';
                return current.at("state") == "FAILED" ? 4 : 0;
            }
            messages.push_back({
                {"role", "user"},
                {"content", "The job is still RUNNING. Continue with the supplied tools; do not merely narrate."},
            });
            continue;
        }
        for (const auto& call : message.at("tool_calls")) {
            const auto name = call.at("function").at("name").get<std::string>();
            const auto raw_arguments = call.at("function").at("arguments");
            const auto raw_text = raw_arguments.is_string()
                ? raw_arguments.get<std::string>()
                : raw_arguments.dump();
            json result;
            try {
                const auto arguments = raw_arguments.is_string()
                    ? json::parse(raw_text)
                    : raw_arguments;
                result = harness.call_tool(name, arguments);
            } catch (const std::exception& error) {
                result = harness.report_malformed_tool_call(
                    name,
                    "malformed tool arguments: " + std::string(error.what()),
                    raw_text);
            }
            messages.push_back({
                {"role", "tool"},
                {"tool_call_id", call.at("id")},
                {"name", name},
                {"content", result.dump()},
            });
            if (harness.status().at("state") == "FAILED") {
                break;
            }
        }
        const auto current = harness.status();
        if (current.at("state") != "RUNNING") {
            std::cout << current.dump(2) << '\n';
            return current.at("state") == "FAILED" ? 4 : 0;
        }
    }
    throw std::runtime_error("director max_steps cap reached before finish_job");
}

int run_mcp(Harness& harness) {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;
        json response{{"jsonrpc", "2.0"}};
        try {
            const auto request = json::parse(line);
            if (request.contains("id")) response["id"] = request.at("id");
            const auto method = request.at("method").get<std::string>();
            if (method == "initialize") {
                response["result"] = {
                    {"protocolVersion", "2025-11-25"},
                    {"capabilities", {{"tools", {{"listChanged", false}}}}},
                    {"serverInfo", {{"name", "swarm-harness"}, {"version", "0.1.0"}}},
                    {"instructions", "Five bounded tools only. This process cannot start servers or expose a shell."},
                };
            } else if (method == "server/discover") {
                response["result"] = {
                    {"protocolVersion", "2026-07-28"},
                    {"capabilities", {{"tools", true}}},
                    {"serverInfo", {{"name", "swarm-harness"}, {"version", "0.1.0"}}},
                };
            } else if (method == "tools/list") {
                response["result"] = {{"tools", harness.mcp_tool_definitions()}};
            } else if (method == "tools/call") {
                const auto& params = request.at("params");
                const auto name = params.at("name").get<std::string>();
                const auto arguments = params.value("arguments", json::object());
                const auto structured = harness.call_tool(name, arguments);
                const bool is_error = !structured.value("ok", false);
                response["result"] = {
                    {"content", json::array({{{"type", "text"}, {"text", structured.dump()}}})},
                    {"structuredContent", structured},
                    {"isError", is_error},
                };
            } else if (method == "notifications/initialized") {
                continue;
            } else {
                response["error"] = {{"code", -32601}, {"message", "Method not found: " + method}};
            }
        } catch (const std::exception& error) {
            response["error"] = {{"code", -32602}, {"message", error.what()}};
        }
        std::cout << response.dump() << '\n' << std::flush;
    }
    harness.wait_all();
    return 0;
}

} // namespace swarm
