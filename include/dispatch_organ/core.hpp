#pragma once

#include <nlohmann/json.hpp>

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace dispatch_organ {

using json = nlohmann::json;

class Refusal final : public std::runtime_error {
public:
    Refusal(std::string code, std::string message);
    const std::string& code() const noexcept { return code_; }

private:
    std::string code_;
};

struct Policy {
    int max_concurrent_dispatches{4};
    int max_calls_per_job{24};
    int max_rounds_per_job{6};
    int call_timeout_ms{300000};
};

struct Envelope {
    std::string member_id;
    std::string seat_backend;
    std::string epistemic_role;
    std::string authority_mode;
    std::vector<std::string> capability_set;
    std::string thread_id;
    std::string job_id;
    std::string task_id;
    std::string run_id;
    std::string mount_id;
    std::uint64_t job_epoch{0};
};

struct CallReservation {
    std::string call_id;
    std::uint64_t state_version{0};
};

struct WorkroomPolicy {
    int max_payload_bytes{4096};
    int max_posts_per_member{32};
    int max_reads_per_member{32};
    int max_chatter_rounds{8};
    int max_unread_per_member{8};
    int max_group_tags_per_member{8};
    int max_delivery_batch{8};
};

struct LedgerPolicy {
    int max_learnings{1024};
    int max_relevance_keys{16};
    int max_evidence_refs{16};
    int max_claim_bytes{4096};
    int max_limits_bytes{2048};
    int max_injection_bytes{4096};
    int max_injection_tokens{1024};
    int contradiction_quarantine_threshold{2};
    double min_verified_confidence{0.5};
};

struct RoomPost {
    std::string event_type;
    std::optional<std::uint64_t> reply_to;
    std::string payload;
    std::vector<std::string> group_tags;
    int chatter_round{1};
};

json parse_strict_json(std::string_view text);
void require_exact_keys(
    const json& value,
    const std::set<std::string>& required,
    const std::set<std::string>& optional,
    std::string_view context);
Policy parse_policy(const json& value);
WorkroomPolicy parse_workroom_policy(const json& value);
LedgerPolicy parse_ledger_policy(const json& value);
Envelope parse_envelope(const json& value);
json envelope_json(const Envelope& envelope);
std::vector<std::string> role_tools(std::string_view role);
std::string sha256_text(std::string_view value);
std::string utc_now();

class Core {
public:
    Core(Policy policy, std::filesystem::path journal_path);
    ~Core();

    Core(const Core&) = delete;
    Core& operator=(const Core&) = delete;

    std::uint64_t version() const;
    const Policy& policy() const noexcept { return policy_; }

    json mount(std::uint64_t expected_version, const Envelope& envelope);
    json cast(
        std::uint64_t expected_version,
        const Envelope& envelope,
        std::string prompt,
        int round_index);
    json tools(const Envelope& envelope) const;
    json authorize_tool(const Envelope& envelope, const std::string& tool) const;

    CallReservation begin_dispatch(
        std::uint64_t expected_version,
        const Envelope& envelope);
    json acknowledge_dispatch(
        std::uint64_t expected_version,
        const Envelope& envelope,
        const std::string& call_id,
        const json& harness_receipt);
    CallReservation begin_result(
        std::uint64_t expected_version,
        const Envelope& envelope);
    json complete_result(
        std::uint64_t expected_version,
        const Envelope& envelope,
        const std::string& call_id,
        const json& harness_result);
    json fail_call(
        std::uint64_t expected_version,
        const Envelope& envelope,
        const std::string& call_id,
        const std::string& reason);
    CallReservation begin_gate(
        std::uint64_t expected_version,
        const Envelope& requester,
        const std::string& target_task_id,
        const std::string& gate_name);
    json complete_gate(
        std::uint64_t expected_version,
        const Envelope& requester,
        const std::string& call_id,
        const json& harness_receipt);
    json fail_gate(
        std::uint64_t expected_version,
        const Envelope& requester,
        const std::string& call_id,
        const std::string& reason);
    json dispose_late(
        std::uint64_t expected_version,
        const Envelope& envelope,
        const std::string& disposition);

    json status() const;
    const std::string& prompt_for(const Envelope& envelope) const;

    // Phase B Workroom. These methods extend the Phase A core beside its
    // accepted dispatch surfaces and share the same append authority.
    void enable_workroom(WorkroomPolicy policy);
    bool workroom_enabled() const noexcept { return workroom_policy_.has_value(); }
    json revoke_mount(std::uint64_t expected_version, const Envelope& envelope);
    json bump_job_epoch(std::uint64_t expected_version, const std::string& job_id);
    json prove_adapter(std::uint64_t expected_version, const std::string& adapter_id);
    json set_member_execution(
        std::uint64_t expected_version,
        const Envelope& envelope,
        const std::string& execution_state);
    json set_group_tags(
        std::uint64_t expected_version,
        const Envelope& envelope,
        std::vector<std::string> group_tags);
    json workroom_post(
        std::uint64_t expected_version,
        const Envelope& envelope,
        RoomPost post);
    json workroom_commutative_observation(
        const std::string& thread_id,
        const std::string& job_id,
        const std::string& task_id,
        const std::string& run_id,
        std::uint64_t job_epoch,
        const std::string& payload);
    json workroom_read(
        std::uint64_t expected_version,
        const Envelope& envelope,
        const std::string& adapter_id,
        const std::string& safe_boundary,
        int limit);
    json workroom_status() const;
    json gate_evidence_view() const;

    // Phase C Experience Ledger. The Ledger is structurally absent unless an
    // operator mounts an explicit policy, and every mutation shares the same
    // conditional Dispatch Journal as Phases A and B.
    void enable_ledger(LedgerPolicy policy);
    bool ledger_enabled() const noexcept { return ledger_policy_.has_value(); }
    json ledger_create_candidate(
        std::uint64_t expected_version,
        const Envelope& author,
        const json& candidate);
    json ledger_review(
        std::uint64_t expected_version,
        const Envelope& reviewer,
        const std::string& learning_id,
        const std::string& outcome,
        const std::string& basis,
        const std::string& conditions,
        std::vector<std::string> evidence_refs);
    json ledger_reconcile(
        std::uint64_t expected_version,
        const Envelope& verifier,
        std::vector<std::string> learning_ids,
        const std::string& conditions,
        std::vector<std::string> evidence_refs);
    json ledger_prepare_injection(
        std::uint64_t expected_version,
        const Envelope& envelope,
        const std::string& question_key,
        std::vector<std::string> relevance_keys,
        const std::string& memory_mode,
        int requested_max_bytes,
        std::vector<std::string> omit_learning_ids,
        const json& declared_omissions);
    json ledger_feedback(
        std::uint64_t expected_version,
        const Envelope& observer,
        const std::string& learning_id,
        const std::string& feedback,
        const std::string& evidence_ref);
    json ledger_status() const;
    [[noreturn]] void refuse_protected_namespace_write(
        const std::string& learning_id,
        const std::string& protected_namespace) const;

private:
    struct Attempt {
        Envelope envelope;
        std::string prompt;
        int round_index{0};
        std::string state;
        bool dispatch_acknowledged{false};
        std::optional<std::string> in_flight_call_id;
        std::optional<std::string> in_flight_call_kind;
        std::optional<json> result_receipt;
        bool recovered_ambiguous{false};
    };

    struct StoredRoomRecord {
        std::uint64_t room_seq{0};
        std::string thread_id;
        std::string job_id;
        std::string task_id;
        std::string run_id;
        std::string member_id;
        std::string mount_id;
        std::uint64_t job_epoch{0};
        std::string timestamp;
        std::string event_type;
        std::optional<std::uint64_t> reply_to;
        std::string bounded_payload;
        std::string payload_hash;
        std::vector<std::string> group_tags;
        int chatter_round{0};
        bool commutative{false};
    };

    struct StoredLearning {
        json object;
        std::string outcome;
        std::string maturity;
        int used_count{0};
        int contradicted_count{0};
        int irrelevant_count{0};
        int unobserved_count{0};
        std::uint64_t last_feedback_seq{0};
    };

    struct GateAttempt {
        Envelope requester;
        std::string call_id;
        std::string target_task_id;
        std::string gate_name;
        std::string state;
        std::optional<json> receipt;
        bool recovered_ambiguous{false};
    };

    Policy policy_;
    std::filesystem::path journal_path_;
    std::uint64_t sequence_{0};
    std::atomic<std::uint64_t> state_version_{0};
    std::uint64_t call_counter_{0};
    int writer_lock_fd_{-1};
    std::map<std::string, Envelope> mounts_;
    std::map<std::string, Attempt> attempts_;
    std::map<std::string, int> calls_by_job_;
    std::map<std::string, int> max_round_by_job_;
    std::map<std::string, GateAttempt> gate_attempts_;
    int active_dispatches_{0};
    std::vector<json> events_;

    std::optional<WorkroomPolicy> workroom_policy_;
    std::map<std::string, bool> mount_live_;
    std::map<std::string, std::uint64_t> mount_version_;
    std::map<std::string, std::uint64_t> current_job_epoch_;
    std::map<std::string, std::string> member_execution_;
    std::map<std::string, std::vector<std::string>> member_group_tags_;
    std::map<std::string, std::uint64_t> member_watermark_;
    std::map<std::string, int> member_posts_;
    std::map<std::string, int> member_reads_;
    std::map<std::string, bool> member_skipped_;
    std::set<std::string> proven_adapters_;
    std::vector<StoredRoomRecord> room_records_;

    std::optional<LedgerPolicy> ledger_policy_;
    std::map<std::string, StoredLearning> learnings_;
    std::vector<json> ledger_reconciliations_;
    std::vector<json> injection_receipts_;

    mutable std::mutex writer_gate_mutex_;
    mutable std::condition_variable writer_gate_cv_;
    bool writer_active_{false};
    std::size_t waiting_authority_writers_{0};

    void replay();
    void apply_event(const json& event, bool recovering);
    json append_event(json event, bool room_chatter_lane = false);
    const Envelope& require_mount(const Envelope& envelope) const;
    Attempt& require_attempt(const Envelope& envelope);
    const Attempt& require_attempt(const Envelope& envelope) const;
    static void require_same_envelope(const Envelope& expected, const Envelope& actual);
    std::string next_call_id(const Envelope& envelope, std::string_view kind);
    bool apply_workroom_event(const json& event);
    bool apply_ledger_event(const json& event);
    const WorkroomPolicy& require_workroom() const;
    const LedgerPolicy& require_ledger() const;
    void require_live_room_mount(const Envelope& envelope) const;
};

} // namespace dispatch_organ
