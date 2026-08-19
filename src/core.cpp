#include "dispatch_organ/core.hpp"

#include <openssl/evp.h>

#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>

namespace dispatch_organ {
namespace {

const std::set<std::string> kEnvelopeKeys{
    "authority_mode", "capability_set", "epistemic_role", "job_epoch",
    "job_id", "member_id", "mount_id", "run_id", "seat_backend",
    "task_id", "thread_id",
};

std::string attempt_key(const Envelope& envelope) {
    return envelope.job_id + "/" + envelope.task_id;
}

void validate_id(const std::string& value, std::string_view field) {
    if (value.empty() || value.size() > 128) {
        throw Refusal("MALFORMED_ENVELOPE", std::string(field) + " must contain 1..128 characters");
    }
    const bool valid = std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.' || ch == ':';
    });
    if (!valid) {
        throw Refusal("MALFORMED_ENVELOPE", std::string(field) + " contains a forbidden character");
    }
}

std::vector<std::string> string_set(const json& value, std::string_view field) {
    if (!value.is_array()) {
        throw Refusal("MALFORMED_ENVELOPE", std::string(field) + " must be an array");
    }
    std::vector<std::string> output;
    std::set<std::string> seen;
    for (const auto& item : value) {
        if (!item.is_string() || item.get_ref<const std::string&>().empty()) {
            throw Refusal("MALFORMED_ENVELOPE", std::string(field) + " entries must be non-empty strings");
        }
        const auto text = item.get<std::string>();
        if (!seen.insert(text).second) {
            throw Refusal("MALFORMED_ENVELOPE", std::string(field) + " contains a duplicate entry");
        }
        output.push_back(text);
    }
    std::sort(output.begin(), output.end());
    return output;
}

void write_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto written = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (written < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error("cannot append Dispatch Journal");
        }
        offset += static_cast<std::size_t>(written);
    }
}

} // namespace

Refusal::Refusal(std::string code, std::string message)
    : std::runtime_error(std::move(message)), code_(std::move(code)) {}

json parse_strict_json(std::string_view text) {
    std::vector<std::unordered_set<std::string>> object_keys;
    const auto callback = [&](int, json::parse_event_t event, json& parsed) {
        if (event == json::parse_event_t::object_start) {
            object_keys.emplace_back();
        } else if (event == json::parse_event_t::key) {
            if (object_keys.empty()) {
                throw Refusal("MALFORMED_JSON", "JSON key appeared outside an object");
            }
            const auto key = parsed.get<std::string>();
            if (!object_keys.back().insert(key).second) {
                throw Refusal("DUPLICATE_KEY", "duplicate JSON key: " + key);
            }
        } else if (event == json::parse_event_t::object_end) {
            if (object_keys.empty()) {
                throw Refusal("MALFORMED_JSON", "JSON object stack underflow");
            }
            object_keys.pop_back();
        }
        return true;
    };
    try {
        return json::parse(text, callback, true, false);
    } catch (const Refusal&) {
        throw;
    } catch (const std::exception& error) {
        throw Refusal("MALFORMED_JSON", error.what());
    }
}

void require_exact_keys(
    const json& value,
    const std::set<std::string>& required,
    const std::set<std::string>& optional,
    std::string_view context) {
    if (!value.is_object()) {
        throw Refusal("MALFORMED_REQUEST", std::string(context) + " must be an object");
    }
    for (auto iterator = value.begin(); iterator != value.end(); ++iterator) {
        if (!required.contains(iterator.key()) && !optional.contains(iterator.key())) {
            throw Refusal("UNKNOWN_KEY", std::string(context) + " has unknown key: " + iterator.key());
        }
    }
    for (const auto& key : required) {
        if (!value.contains(key)) {
            throw Refusal("MISSING_KEY", std::string(context) + " is missing key: " + key);
        }
    }
}

Policy parse_policy(const json& value) {
    require_exact_keys(value, {
        "call_timeout_ms", "max_calls_per_job", "max_concurrent_dispatches",
        "max_rounds_per_job",
    }, {}, "policy");
    Policy policy;
    try {
        policy.max_concurrent_dispatches = value.at("max_concurrent_dispatches").get<int>();
        policy.max_calls_per_job = value.at("max_calls_per_job").get<int>();
        policy.max_rounds_per_job = value.at("max_rounds_per_job").get<int>();
        policy.call_timeout_ms = value.at("call_timeout_ms").get<int>();
    } catch (const std::exception& error) {
        throw Refusal("MALFORMED_POLICY", error.what());
    }
    if (policy.max_concurrent_dispatches < 1 || policy.max_calls_per_job < 1 ||
        policy.max_rounds_per_job < 1 || policy.call_timeout_ms < 1) {
        throw Refusal("MALFORMED_POLICY", "all local cap values must be positive");
    }
    return policy;
}

std::vector<std::string> role_tools(std::string_view role) {
    if (role == "builder") return {"read_assignment", "submit_result"};
    if (role == "scout") return {"read_assignment", "submit_observation"};
    if (role == "adversary") return {"read_assignment", "submit_challenge"};
    if (role == "verifier") return {"read_assignment", "request_gate", "submit_verification"};
    throw Refusal("UNSUPPORTED_ROLE", "epistemic_role is outside the Phase A taxonomy");
}

Envelope parse_envelope(const json& value) {
    require_exact_keys(value, kEnvelopeKeys, {}, "envelope");
    Envelope envelope;
    try {
        envelope.member_id = value.at("member_id").get<std::string>();
        envelope.seat_backend = value.at("seat_backend").get<std::string>();
        envelope.epistemic_role = value.at("epistemic_role").get<std::string>();
        envelope.authority_mode = value.at("authority_mode").get<std::string>();
        envelope.capability_set = string_set(value.at("capability_set"), "capability_set");
        envelope.thread_id = value.at("thread_id").get<std::string>();
        envelope.job_id = value.at("job_id").get<std::string>();
        envelope.task_id = value.at("task_id").get<std::string>();
        envelope.run_id = value.at("run_id").get<std::string>();
        envelope.mount_id = value.at("mount_id").get<std::string>();
        envelope.job_epoch = value.at("job_epoch").get<std::uint64_t>();
    } catch (const Refusal&) {
        throw;
    } catch (const std::exception& error) {
        throw Refusal("MALFORMED_ENVELOPE", error.what());
    }

    validate_id(envelope.member_id, "member_id");
    validate_id(envelope.thread_id, "thread_id");
    validate_id(envelope.job_id, "job_id");
    validate_id(envelope.task_id, "task_id");
    validate_id(envelope.run_id, "run_id");
    validate_id(envelope.mount_id, "mount_id");
    if (envelope.job_epoch < 1) {
        throw Refusal("MALFORMED_ENVELOPE", "job_epoch must be positive");
    }
    if (envelope.seat_backend != "local_qwen_swarm") {
        throw Refusal("FORBIDDEN_BACKEND", "Phase A permits only local_qwen_swarm");
    }
    if (envelope.authority_mode != "bot") {
        throw Refusal("FORBIDDEN_AUTHORITY", "Phase A permits only bot authority");
    }
    const auto expected_tools = role_tools(envelope.epistemic_role);
    if (envelope.capability_set != expected_tools) {
        throw Refusal("CAPABILITY_MISMATCH", "capability_set must equal the role-shaped Phase A surface");
    }
    return envelope;
}

json envelope_json(const Envelope& envelope) {
    return {
        {"member_id", envelope.member_id},
        {"seat_backend", envelope.seat_backend},
        {"epistemic_role", envelope.epistemic_role},
        {"authority_mode", envelope.authority_mode},
        {"capability_set", envelope.capability_set},
        {"thread_id", envelope.thread_id},
        {"job_id", envelope.job_id},
        {"task_id", envelope.task_id},
        {"run_id", envelope.run_id},
        {"mount_id", envelope.mount_id},
        {"job_epoch", envelope.job_epoch},
    };
}

std::string sha256_text(std::string_view value) {
    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) throw std::runtime_error("cannot allocate SHA-256 context");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int digest_length = 0;
    const bool ok = EVP_DigestInit_ex(context, EVP_sha256(), nullptr) == 1 &&
        EVP_DigestUpdate(context, value.data(), value.size()) == 1 &&
        EVP_DigestFinal_ex(context, digest.data(), &digest_length) == 1;
    EVP_MD_CTX_free(context);
    if (!ok) throw std::runtime_error("SHA-256 failed");
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (unsigned int index = 0; index < digest_length; ++index) {
        output << std::setw(2) << static_cast<unsigned int>(digest[index]);
    }
    return output.str();
}

std::string utc_now() {
    const auto now = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(now);
    std::tm calendar{};
    gmtime_r(&time, &calendar);
    std::ostringstream output;
    output << std::put_time(&calendar, "%Y-%m-%dT%H:%M:%SZ");
    return output.str();
}

Core::Core(Policy policy, std::filesystem::path journal_path)
    : policy_(policy), journal_path_(std::move(journal_path)) {
    const auto parent = journal_path_.parent_path();
    if (!parent.empty()) std::filesystem::create_directories(parent);
    const auto lock_path = journal_path_.string() + ".lock";
    writer_lock_fd_ = ::open(lock_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (writer_lock_fd_ < 0) throw std::runtime_error("cannot open Dispatch Journal writer lock");
    if (::flock(writer_lock_fd_, LOCK_EX | LOCK_NB) != 0) {
        ::close(writer_lock_fd_);
        writer_lock_fd_ = -1;
        throw Refusal("JOURNAL_BUSY", "another Dispatch Organ owns the journal append authority");
    }
    try {
        replay();
    } catch (...) {
        ::flock(writer_lock_fd_, LOCK_UN);
        ::close(writer_lock_fd_);
        writer_lock_fd_ = -1;
        throw;
    }
}

Core::~Core() {
    if (writer_lock_fd_ >= 0) {
        (void)::flock(writer_lock_fd_, LOCK_UN);
        ::close(writer_lock_fd_);
    }
}

std::uint64_t Core::version() const {
    return state_version_.load(std::memory_order_acquire);
}

void Core::require_same_envelope(const Envelope& expected, const Envelope& actual) {
    if (envelope_json(expected) != envelope_json(actual)) {
        throw Refusal("IDENTITY_MISMATCH", "request envelope does not match its mounted identity lattice");
    }
}

const Envelope& Core::require_mount(const Envelope& envelope) const {
    const auto found = mounts_.find(envelope.mount_id);
    if (found == mounts_.end()) throw Refusal("UNKNOWN_MOUNT", "mount_id is not live");
    require_same_envelope(found->second, envelope);
    return found->second;
}

Core::Attempt& Core::require_attempt(const Envelope& envelope) {
    require_mount(envelope);
    const auto found = attempts_.find(attempt_key(envelope));
    if (found == attempts_.end()) throw Refusal("UNKNOWN_TASK", "task identity has not been cast");
    require_same_envelope(found->second.envelope, envelope);
    return found->second;
}

const Core::Attempt& Core::require_attempt(const Envelope& envelope) const {
    require_mount(envelope);
    const auto found = attempts_.find(attempt_key(envelope));
    if (found == attempts_.end()) throw Refusal("UNKNOWN_TASK", "task identity has not been cast");
    require_same_envelope(found->second.envelope, envelope);
    return found->second;
}

void Core::replay() {
    if (!std::filesystem::exists(journal_path_)) return;
    std::ifstream input(journal_path_);
    if (!input) throw std::runtime_error("cannot open Dispatch Journal for replay");
    std::string line;
    while (std::getline(input, line)) {
        if (line.empty()) continue;
        const auto event = parse_strict_json(line);
        if (!event.is_object() || !event.contains("record_sha256")) {
            throw std::runtime_error("Dispatch Journal record lacks integrity hash");
        }
        auto unhashed = event;
        const auto expected_hash = unhashed.at("record_sha256").get<std::string>();
        unhashed.erase("record_sha256");
        if (sha256_text(unhashed.dump()) != expected_hash) {
            throw std::runtime_error("Dispatch Journal record hash mismatch");
        }
        const auto expected_sequence = sequence_ + 1;
        if (event.at("seq").get<std::uint64_t>() != expected_sequence) {
            throw std::runtime_error("Dispatch Journal sequence discontinuity");
        }
        apply_event(event, true);
    }
    for (auto& [key, attempt] : attempts_) {
        (void)key;
        if (attempt.in_flight_call_id) {
            attempt.state = "PENDING_LATE";
            attempt.recovered_ambiguous = true;
            if (active_dispatches_ > 0) --active_dispatches_;
        }
    }
    for (auto& [call_id, attempt] : gate_attempts_) {
        (void)call_id;
        if (attempt.state == "CALLING") {
            attempt.state = "PENDING_LATE";
            attempt.recovered_ambiguous = true;
        }
    }
}

json Core::append_event(json event, bool room_chatter_lane) {
    std::unique_lock gate(writer_gate_mutex_);
    if (!room_chatter_lane) ++waiting_authority_writers_;
    writer_gate_cv_.wait(gate, [&] {
        return !writer_active_ && (!room_chatter_lane || waiting_authority_writers_ == 0);
    });
    if (!room_chatter_lane) --waiting_authority_writers_;
    writer_active_ = true;
    gate.unlock();

    const auto release_writer = [&] {
        std::lock_guard release_lock(writer_gate_mutex_);
        writer_active_ = false;
        writer_gate_cv_.notify_all();
    };

    int journal_fd = -1;
    try {
        if (event.contains("read_set")) {
            for (const auto& precondition : event.at("read_set")) {
                if (precondition.contains("state_version")) {
                    const auto expected = precondition.at("state_version").get<std::uint64_t>();
                    const auto committed = version();
                    if (expected == committed) continue;
                    bool only_commutative_intervened = !room_chatter_lane && expected < committed;
                    for (const auto& prior : events_) {
                        if (prior.at("state_version").get<std::uint64_t>() <= expected) continue;
                        if (!prior.at("read_set").empty()) {
                            only_commutative_intervened = false;
                            break;
                        }
                        for (const auto& write : prior.at("write_set")) {
                            if (!write.is_string() || write.get_ref<const std::string&>().rfind("commutative:", 0) != 0) {
                                only_commutative_intervened = false;
                                break;
                            }
                        }
                        if (!only_commutative_intervened) break;
                    }
                    if (!only_commutative_intervened) {
                        throw Refusal(
                            "STALE_PRECONDITION",
                            "state version changed before the conditional append reached committed head");
                    }
                    event["rebased_from_state_version"] = expected;
                    event["revalidated_at_head"] = committed;
                }
            }
        }
        event["seq"] = sequence_ + 1;
        event["state_version"] = version() + 1;
        event["timestamp"] = utc_now();
        if (event.value("event_type", "") == "ROOM_POST" && event.contains("record")) {
            event["record"]["room_seq"] = event["seq"];
            event["record"]["timestamp"] = event["timestamp"];
        }
        if (event.value("event_type", "") == "LEARNING_CANDIDATE" && event.contains("learning")) {
            event["learning"]["created_seq"] = event["seq"];
        }
        event["record_sha256"] = sha256_text(event.dump());
        const auto line = event.dump() + "\n";
        const auto parent = journal_path_.parent_path();
        if (!parent.empty()) std::filesystem::create_directories(parent);
        journal_fd = ::open(journal_path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0644);
        if (journal_fd < 0) throw std::runtime_error("cannot open Dispatch Journal for append");
        write_all(journal_fd, line);
        if (::fsync(journal_fd) != 0) {
            ::close(journal_fd);
            journal_fd = -1;
            throw std::runtime_error("cannot fsync Dispatch Journal");
        }
        ::close(journal_fd);
        journal_fd = -1;
        apply_event(event, false);
        release_writer();
        return event;
    } catch (...) {
        if (journal_fd >= 0) ::close(journal_fd);
        release_writer();
        throw;
    }
}

void Core::apply_event(const json& event, bool) {
    sequence_ = event.at("seq").get<std::uint64_t>();
    state_version_ = event.at("state_version").get<std::uint64_t>();
    const auto type = event.at("event_type").get<std::string>();
    events_.push_back(event);
    if (type == "MOUNTED") {
        const auto envelope = parse_envelope(event.at("envelope"));
        mounts_[envelope.mount_id] = envelope;
        mount_live_[envelope.mount_id] = true;
        mount_version_[envelope.mount_id] = event.at("state_version").get<std::uint64_t>();
        const auto found_epoch = current_job_epoch_.find(envelope.job_id);
        if (found_epoch == current_job_epoch_.end()) {
            current_job_epoch_[envelope.job_id] = envelope.job_epoch;
        } else if (found_epoch->second != envelope.job_epoch) {
            throw std::runtime_error("mounted envelope job_epoch conflicts with committed job epoch");
        }
    } else if (type == "CAST") {
        const auto envelope = parse_envelope(event.at("envelope"));
        Attempt attempt;
        attempt.envelope = envelope;
        attempt.prompt = event.at("prompt").get<std::string>();
        attempt.round_index = event.at("round_index").get<int>();
        attempt.state = "CAST";
        attempts_[attempt_key(envelope)] = std::move(attempt);
        max_round_by_job_[envelope.job_id] = std::max(
            max_round_by_job_[envelope.job_id], event.at("round_index").get<int>());
    } else if (type == "CALL_INTENT") {
        const auto envelope = parse_envelope(event.at("envelope"));
        auto& attempt = attempts_.at(attempt_key(envelope));
        attempt.in_flight_call_id = event.at("call_id").get<std::string>();
        attempt.in_flight_call_kind = event.at("call_kind").get<std::string>();
        attempt.state = "RUNNING";
        ++calls_by_job_[envelope.job_id];
        if (*attempt.in_flight_call_kind == "dispatch") ++active_dispatches_;
    } else if (type == "DISPATCH_ACK") {
        const auto envelope = parse_envelope(event.at("envelope"));
        auto& attempt = attempts_.at(attempt_key(envelope));
        attempt.in_flight_call_id.reset();
        attempt.in_flight_call_kind.reset();
        attempt.dispatch_acknowledged = true;
        attempt.state = "RUNNING";
    } else if (type == "RESULT") {
        const auto envelope = parse_envelope(event.at("envelope"));
        auto& attempt = attempts_.at(attempt_key(envelope));
        attempt.in_flight_call_id.reset();
        attempt.in_flight_call_kind.reset();
        attempt.result_receipt = event.at("receipt");
        attempt.state = "RESULT";
        if (active_dispatches_ > 0) --active_dispatches_;
    } else if (type == "CALL_FAILED") {
        const auto envelope = parse_envelope(event.at("envelope"));
        auto& attempt = attempts_.at(attempt_key(envelope));
        attempt.in_flight_call_id.reset();
        attempt.in_flight_call_kind.reset();
        attempt.state = "STRANDED";
        if (active_dispatches_ > 0) --active_dispatches_;
    } else if (type == "GATE_CALL_INTENT") {
        const auto envelope = parse_envelope(event.at("envelope"));
        GateAttempt attempt;
        attempt.requester = envelope;
        attempt.call_id = event.at("call_id").get<std::string>();
        attempt.target_task_id = event.at("target_task_id").get<std::string>();
        attempt.gate_name = event.at("gate_name").get<std::string>();
        attempt.state = "CALLING";
        gate_attempts_[attempt.call_id] = std::move(attempt);
        ++calls_by_job_[envelope.job_id];
    } else if (type == "GATE_RESULT") {
        auto& attempt = gate_attempts_.at(event.at("call_id").get<std::string>());
        attempt.state = "RESULT";
        attempt.receipt = event.at("receipt");
    } else if (type == "GATE_FAILED") {
        auto& attempt = gate_attempts_.at(event.at("call_id").get<std::string>());
        attempt.state = "STRANDED";
    } else if (type == "LATE_DISPOSITION") {
        const auto envelope = parse_envelope(event.at("envelope"));
        auto& attempt = attempts_.at(attempt_key(envelope));
        attempt.state = event.at("disposition").get<std::string>();
        attempt.in_flight_call_id.reset();
        attempt.in_flight_call_kind.reset();
        attempt.recovered_ambiguous = false;
    } else if (!apply_workroom_event(event) && !apply_ledger_event(event)) {
        throw std::runtime_error("unknown Dispatch Journal event_type: " + type);
    }
    call_counter_ = std::max(call_counter_, sequence_);
}

json Core::mount(std::uint64_t expected_version, const Envelope& envelope) {
    if (mounts_.contains(envelope.mount_id)) {
        throw Refusal("CONFLICT", "mount_id already exists");
    }
    return append_event({
        {"event_type", "MOUNTED"},
        {"envelope", envelope_json(envelope)},
        {"read_set", json::array({{{"state_version", expected_version}}})},
        {"write_set", json::array({"mount:" + envelope.mount_id})},
    });
}

json Core::cast(
    std::uint64_t expected_version,
    const Envelope& envelope,
    std::string prompt,
    int round_index) {
    require_mount(envelope);
    if (attempts_.contains(attempt_key(envelope))) {
        throw Refusal("CONFLICT", "task_id already exists in this job");
    }
    if (prompt.empty() || prompt.size() > 65536) {
        throw Refusal("MALFORMED_CAST", "prompt must contain 1..65536 bytes");
    }
    if (round_index < 1 || round_index > policy_.max_rounds_per_job) {
        throw Refusal("CAP_EXCEEDED", "round_index exceeds max_rounds_per_job");
    }
    const auto prompt_hash = sha256_text(prompt);
    const auto prompt_bytes = prompt.size();
    return append_event({
        {"event_type", "CAST"},
        {"envelope", envelope_json(envelope)},
        {"prompt", std::move(prompt)},
        {"prompt_sha256", prompt_hash},
        {"prompt_bytes", prompt_bytes},
        {"round_index", round_index},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount", envelope.mount_id}},
            {{"job_epoch", envelope.job_epoch}},
            {{"task_id_free", envelope.task_id}},
        })},
        {"write_set", json::array({"task:" + attempt_key(envelope)})},
    });
}

json Core::tools(const Envelope& envelope) const {
    require_mount(envelope);
    return {
        {"ok", true},
        {"mount_id", envelope.mount_id},
        {"role", envelope.epistemic_role},
        {"tools", role_tools(envelope.epistemic_role)},
        {"state_version", version()},
    };
}

json Core::authorize_tool(const Envelope& envelope, const std::string& tool) const {
    require_mount(envelope);
    const auto allowed = role_tools(envelope.epistemic_role);
    if (std::find(allowed.begin(), allowed.end(), tool) == allowed.end()) {
        throw Refusal("FORBIDDEN_TOOL", "tool is absent from the mounted role surface and refused by dispatcher: " + tool);
    }
    return {
        {"ok", true},
        {"outcome", "AUTHORIZED"},
        {"tool", tool},
        {"mount_id", envelope.mount_id},
        {"state_version", version()},
    };
}

std::string Core::next_call_id(const Envelope& envelope, std::string_view kind) {
    ++call_counter_;
    return envelope.run_id + ":" + std::string(kind) + ":" + std::to_string(call_counter_);
}

CallReservation Core::begin_dispatch(
    std::uint64_t expected_version,
    const Envelope& envelope) {
    auto& attempt = require_attempt(envelope);
    if (attempt.state != "CAST" || attempt.in_flight_call_id) {
        throw Refusal("CONFLICT", "attempt is not dispatchable from CAST");
    }
    if (active_dispatches_ >= policy_.max_concurrent_dispatches) {
        throw Refusal("CAP_EXCEEDED", "max_concurrent_dispatches reached");
    }
    if (calls_by_job_[envelope.job_id] >= policy_.max_calls_per_job) {
        throw Refusal("CAP_EXCEEDED", "max_calls_per_job reached");
    }
    const auto call_id = next_call_id(envelope, "dispatch");
    const auto event = append_event({
        {"event_type", "CALL_INTENT"},
        {"call_kind", "dispatch"},
        {"call_id", call_id},
        {"call_in_flight", true},
        {"envelope", envelope_json(envelope)},
        {"seat_backend", envelope.seat_backend},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount", envelope.mount_id}},
            {{"job_epoch", envelope.job_epoch}},
            {{"attempt_state", "CAST"}},
            {{"local_call_count", calls_by_job_[envelope.job_id]}},
        })},
        {"write_set", json::array({"call:" + call_id, "attempt:" + envelope.run_id, "local_caps:" + envelope.job_id})},
    });
    return {call_id, event.at("state_version").get<std::uint64_t>()};
}

json Core::acknowledge_dispatch(
    std::uint64_t expected_version,
    const Envelope& envelope,
    const std::string& call_id,
    const json& harness_receipt) {
    auto& attempt = require_attempt(envelope);
    if (!attempt.in_flight_call_id || *attempt.in_flight_call_id != call_id ||
        !attempt.in_flight_call_kind || *attempt.in_flight_call_kind != "dispatch") {
        throw Refusal("CONFLICT", "dispatch acknowledgement does not match the in-flight intent");
    }
    return append_event({
        {"event_type", "DISPATCH_ACK"},
        {"call_id", call_id},
        {"envelope", envelope_json(envelope)},
        {"harness_receipt", harness_receipt},
        {"read_set", json::array({{{"state_version", expected_version}}, {{"call_in_flight", call_id}}})},
        {"write_set", json::array({"attempt:" + envelope.run_id})},
    });
}

CallReservation Core::begin_result(
    std::uint64_t expected_version,
    const Envelope& envelope) {
    auto& attempt = require_attempt(envelope);
    if (attempt.state != "RUNNING" || !attempt.dispatch_acknowledged || attempt.in_flight_call_id) {
        throw Refusal("CONFLICT", "attempt is not ready for result retrieval");
    }
    if (calls_by_job_[envelope.job_id] >= policy_.max_calls_per_job) {
        throw Refusal("CAP_EXCEEDED", "max_calls_per_job reached");
    }
    const auto call_id = next_call_id(envelope, "result");
    const auto event = append_event({
        {"event_type", "CALL_INTENT"},
        {"call_kind", "result"},
        {"call_id", call_id},
        {"call_in_flight", true},
        {"envelope", envelope_json(envelope)},
        {"seat_backend", envelope.seat_backend},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount", envelope.mount_id}},
            {{"job_epoch", envelope.job_epoch}},
            {{"attempt_state", "RUNNING"}},
            {{"local_call_count", calls_by_job_[envelope.job_id]}},
        })},
        {"write_set", json::array({"call:" + call_id, "local_caps:" + envelope.job_id})},
    });
    return {call_id, event.at("state_version").get<std::uint64_t>()};
}

json Core::complete_result(
    std::uint64_t expected_version,
    const Envelope& envelope,
    const std::string& call_id,
    const json& harness_result) {
    auto& attempt = require_attempt(envelope);
    if (!attempt.in_flight_call_id || *attempt.in_flight_call_id != call_id ||
        !attempt.in_flight_call_kind || *attempt.in_flight_call_kind != "result") {
        throw Refusal("CONFLICT", "result does not match the in-flight intent");
    }
    if (!harness_result.is_object()) {
        throw Refusal("MALFORMED_EVIDENCE", "harness result must be an object");
    }
    if (harness_result.contains("ok") && !harness_result.value("ok", false)) {
        throw Refusal("MALFORMED_EVIDENCE", "harness result does not carry an accepted outcome");
    }
    const auto harness_state = harness_result.value("state", "");
    const std::set<std::string> terminal_harness_states{
        "GATED", "PENNED", "SUCCEEDED",
    };
    if (harness_result.contains("state") && !terminal_harness_states.contains(harness_state)) {
        throw Refusal(
            "NONTERMINAL_EVIDENCE",
            "harness result snapshot is not terminal and cannot become Organ evidence: " + harness_state);
    }
    if (harness_result.contains("content") && !harness_result.at("content").is_string()) {
        throw Refusal("MALFORMED_EVIDENCE", "terminal harness result must carry string content");
    }
    const auto content = harness_result.value("content", "");
    const auto content_hash = sha256_text(content);
    if (harness_result.contains("content_sha256") &&
        harness_result.at("content_sha256").get<std::string>() != content_hash) {
        throw Refusal("HASH_MISMATCH", "harness result hash does not match its content");
    }
    std::string completeness = harness_result.value("completeness", "FULL");
    if (harness_result.contains("artifact") && harness_result.at("artifact").is_object()) {
        completeness = harness_result.at("artifact").value("completeness", completeness);
    }
    if (completeness != "FULL" && completeness != "CLIPPED") {
        throw Refusal("MALFORMED_EVIDENCE", "completeness must be FULL or CLIPPED");
    }
    const auto timestamp = utc_now();
    json receipt{
        {"producer", {{"member_id", envelope.member_id}, {"mount_id", envelope.mount_id}}},
        {"thread_id", envelope.thread_id},
        {"job_id", envelope.job_id},
        {"task_id", envelope.task_id},
        {"run_id", envelope.run_id},
        {"job_epoch", envelope.job_epoch},
        {"source_locator", "swarm-harness:mcp:get_result"},
        {"content_sha256", content_hash},
        {"content_bytes", content.size()},
        {"completeness", completeness},
        {"sequence", sequence_ + 1},
        {"timestamp", timestamp},
        {"harness", harness_result},
    };
    return append_event({
        {"event_type", "RESULT"},
        {"call_id", call_id},
        {"envelope", envelope_json(envelope)},
        {"receipt", std::move(receipt)},
        {"external_completion", "AWAITING_OPERATOR"},
        {"read_set", json::array({{{"state_version", expected_version}}, {{"call_in_flight", call_id}}})},
        {"write_set", json::array({"attempt:" + envelope.run_id, "evidence:" + envelope.task_id})},
    });
}

json Core::fail_call(
    std::uint64_t expected_version,
    const Envelope& envelope,
    const std::string& call_id,
    const std::string& reason) {
    auto& attempt = require_attempt(envelope);
    if (!attempt.in_flight_call_id || *attempt.in_flight_call_id != call_id) {
        throw Refusal("CONFLICT", "failure does not match the in-flight intent");
    }
    return append_event({
        {"event_type", "CALL_FAILED"},
        {"call_id", call_id},
        {"envelope", envelope_json(envelope)},
        {"reason", reason},
        {"read_set", json::array({{{"state_version", expected_version}}, {{"call_in_flight", call_id}}})},
        {"write_set", json::array({"attempt:" + envelope.run_id})},
    });
}

CallReservation Core::begin_gate(
    std::uint64_t expected_version,
    const Envelope& requester,
    const std::string& target_task_id,
    const std::string& gate_name) {
    require_mount(requester);
    const auto allowed = role_tools(requester.epistemic_role);
    if (std::find(allowed.begin(), allowed.end(), "request_gate") == allowed.end()) {
        throw Refusal("FORBIDDEN_TOOL", "request_gate is absent from the mounted role surface");
    }
    validate_id(target_task_id, "target_task_id");
    validate_id(gate_name, "gate_name");
    if (calls_by_job_[requester.job_id] >= policy_.max_calls_per_job) {
        throw Refusal("CAP_EXCEEDED", "max_calls_per_job reached");
    }
    const auto call_id = next_call_id(requester, "gate");
    const auto event = append_event({
        {"event_type", "GATE_CALL_INTENT"},
        {"record_class", "EVIDENCE"},
        {"call_id", call_id},
        {"call_in_flight", true},
        {"target_task_id", target_task_id},
        {"gate_name", gate_name},
        {"envelope", envelope_json(requester)},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount", requester.mount_id}},
            {{"job_epoch", requester.job_epoch}},
            {{"local_call_count", calls_by_job_[requester.job_id]}},
        })},
        {"write_set", json::array({"gate-call:" + call_id, "local_caps:" + requester.job_id})},
    });
    return {call_id, event.at("state_version").get<std::uint64_t>()};
}

json Core::complete_gate(
    std::uint64_t expected_version,
    const Envelope& requester,
    const std::string& call_id,
    const json& harness_receipt) {
    require_mount(requester);
    const auto found = gate_attempts_.find(call_id);
    if (found == gate_attempts_.end() || found->second.state != "CALLING") {
        throw Refusal("CONFLICT", "gate acknowledgement does not match an in-flight gate intent");
    }
    require_same_envelope(found->second.requester, requester);
    if (!harness_receipt.is_object() || !harness_receipt.value("ok", false) ||
        !harness_receipt.value("passed", false) ||
        harness_receipt.value("task_id", "") != found->second.target_task_id ||
        harness_receipt.value("gate", "") != found->second.gate_name) {
        throw Refusal("MALFORMED_EVIDENCE", "harness gate receipt does not prove the requested passing gate");
    }
    return append_event({
        {"event_type", "GATE_RESULT"},
        {"record_class", "EVIDENCE"},
        {"call_id", call_id},
        {"envelope", envelope_json(requester)},
        {"target_task_id", found->second.target_task_id},
        {"gate_name", found->second.gate_name},
        {"receipt", harness_receipt},
        {"read_set", json::array({{{"state_version", expected_version}}, {{"call_in_flight", call_id}}})},
        {"write_set", json::array({"gate-evidence:" + found->second.target_task_id + ":" + found->second.gate_name})},
    });
}

json Core::fail_gate(
    std::uint64_t expected_version,
    const Envelope& requester,
    const std::string& call_id,
    const std::string& reason) {
    require_mount(requester);
    const auto found = gate_attempts_.find(call_id);
    if (found == gate_attempts_.end() || found->second.state != "CALLING") {
        throw Refusal("CONFLICT", "gate failure does not match an in-flight gate intent");
    }
    require_same_envelope(found->second.requester, requester);
    return append_event({
        {"event_type", "GATE_FAILED"},
        {"record_class", "EVIDENCE"},
        {"call_id", call_id},
        {"envelope", envelope_json(requester)},
        {"target_task_id", found->second.target_task_id},
        {"gate_name", found->second.gate_name},
        {"reason", reason},
        {"read_set", json::array({{{"state_version", expected_version}}, {{"call_in_flight", call_id}}})},
        {"write_set", json::array({"gate-call:" + call_id})},
    });
}

json Core::dispose_late(
    std::uint64_t expected_version,
    const Envelope& envelope,
    const std::string& disposition) {
    auto& attempt = require_attempt(envelope);
    if (attempt.state != "PENDING_LATE" && attempt.state != "STRANDED") {
        throw Refusal("CONFLICT", "attempt is not pending late disposition");
    }
    if (disposition != "HARVESTED" && disposition != "SUPERSEDED" && disposition != "ABANDONED") {
        throw Refusal("MALFORMED_DISPOSITION", "late disposition is unsupported");
    }
    return append_event({
        {"event_type", "LATE_DISPOSITION"},
        {"envelope", envelope_json(envelope)},
        {"disposition", disposition},
        {"read_set", json::array({{{"state_version", expected_version}}, {{"attempt_state", attempt.state}}})},
        {"write_set", json::array({"attempt:" + envelope.run_id})},
    });
}

json Core::status() const {
    json attempts = json::array();
    for (const auto& [key, attempt] : attempts_) {
        attempts.push_back({
            {"identity", key},
            {"run_id", attempt.envelope.run_id},
            {"mount_id", attempt.envelope.mount_id},
            {"state", attempt.state},
            {"in_flight_call_id", attempt.in_flight_call_id ? json(*attempt.in_flight_call_id) : json(nullptr)},
            {"recovered_ambiguous", attempt.recovered_ambiguous},
            {"result_receipt", attempt.result_receipt ? *attempt.result_receipt : json(nullptr)},
        });
    }
    json calls = json::object();
    for (const auto& [job, count] : calls_by_job_) calls[job] = count;
    json gate_attempts = json::array();
    for (const auto& [call_id, attempt] : gate_attempts_) {
        gate_attempts.push_back({
            {"call_id", call_id},
            {"requester_member_id", attempt.requester.member_id},
            {"requester_run_id", attempt.requester.run_id},
            {"target_task_id", attempt.target_task_id},
            {"gate_name", attempt.gate_name},
            {"state", attempt.state},
            {"recovered_ambiguous", attempt.recovered_ambiguous},
            {"receipt", attempt.receipt ? *attempt.receipt : json(nullptr)},
        });
    }
    return {
        {"ok", true},
        {"state_version", version()},
        {"journal_sequence", sequence_},
        {"mount_count", mounts_.size()},
        {"active_dispatches", active_dispatches_},
        {"calls_by_job", calls},
        {"attempts", attempts},
        {"gate_attempts", gate_attempts},
        {"external_completion", "AWAITING_OPERATOR"},
    };
}

const std::string& Core::prompt_for(const Envelope& envelope) const {
    return require_attempt(envelope).prompt;
}

} // namespace dispatch_organ
