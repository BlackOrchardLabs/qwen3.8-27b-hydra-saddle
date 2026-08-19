#include "dispatch_organ/core.hpp"

#include <unistd.h>

#include <algorithm>
#include <filesystem>
#include <functional>
#include <iostream>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

using dispatch_organ::Core;
using dispatch_organ::Envelope;
using dispatch_organ::LedgerPolicy;
using dispatch_organ::Policy;
using dispatch_organ::Refusal;
using dispatch_organ::RoomPost;
using dispatch_organ::WorkroomPolicy;
using dispatch_organ::json;

class TempDirectory {
public:
    explicit TempDirectory(std::string name) {
        path_ = std::filesystem::temp_directory_path() /
            ("dispatch-organ-ledger-" + std::to_string(::getpid()) + "-" + std::move(name));
        std::filesystem::remove_all(path_);
        std::filesystem::create_directories(path_);
    }
    ~TempDirectory() { std::filesystem::remove_all(path_); }
    const std::filesystem::path& path() const { return path_; }

private:
    std::filesystem::path path_;
};

void check(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename Callable>
void expect_refusal(const std::string& code, Callable&& callable) {
    try {
        callable();
    } catch (const Refusal& refusal) {
        check(refusal.code() == code, "expected " + code + ", got " + refusal.code());
        return;
    }
    throw std::runtime_error("expected refusal " + code + " but operation succeeded");
}

Envelope identity(
    std::string member,
    std::string task,
    std::string mount,
    std::string role = "builder",
    std::string run = "") {
    if (run.empty()) run = "run_" + task;
    return {
        std::move(member), "local_qwen_swarm", role, "bot",
        dispatch_organ::role_tools(role), "thread_ledger", "job_ledger",
        std::move(task), std::move(run), std::move(mount), 1,
    };
}

WorkroomPolicy workroom_policy() {
    WorkroomPolicy policy;
    policy.max_payload_bytes = 512;
    policy.max_posts_per_member = 32;
    policy.max_reads_per_member = 32;
    policy.max_chatter_rounds = 8;
    policy.max_unread_per_member = 8;
    policy.max_group_tags_per_member = 8;
    policy.max_delivery_batch = 8;
    return policy;
}

LedgerPolicy ledger_policy() {
    LedgerPolicy policy;
    policy.max_learnings = 64;
    policy.max_relevance_keys = 8;
    policy.max_evidence_refs = 16;
    policy.max_claim_bytes = 1024;
    policy.max_limits_bytes = 512;
    policy.max_injection_bytes = 2048;
    policy.max_injection_tokens = 512;
    policy.contradiction_quarantine_threshold = 2;
    policy.min_verified_confidence = 0.5;
    return policy;
}

json candidate(
    std::string id,
    std::string claim,
    std::uint64_t review_after = 100000,
    std::vector<std::string> keys = {"format:hex", "domain:cirrus"}) {
    return {
        {"learning_id", std::move(id)},
        {"question_key", "cirrus.checksum"},
        {"role_scope", "builder"},
        {"relevance_keys", std::move(keys)},
        {"claim", std::move(claim)},
        {"limits", "Use only for the cirrus checksum fixture."},
        {"evidence_refs", json::array({"artifact:fixture/cirrus-v1"})},
        {"outcome", "unknown"},
        {"confidence", 0.95},
        {"review_after", review_after},
        {"injection_budget", 1024},
    };
}

const json& learning_by_id(const json& status, const std::string& id) {
    const auto& learnings = status.at("learnings");
    const auto found = std::find_if(learnings.begin(), learnings.end(), [&](const auto& item) {
        return item.at("learning_id") == id;
    });
    if (found == learnings.end()) throw std::runtime_error("learning missing from status: " + id);
    return *found;
}

void candidate_promotion_and_nonvacuous_canary() {
    TempDirectory temp("canary");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(workroom_policy());
    core.enable_ledger(ledger_policy());
    const auto author = identity("author", "task_author", "mount_author", "verifier", "run_shared_author");
    const auto verifier = identity("independent", "task_verify", "mount_verify", "verifier");
    const auto target = identity("worker", "task_target", "mount_target", "builder");
    core.mount(core.version(), author);
    core.mount(core.version(), verifier);
    core.mount(core.version(), target);
    core.cast(core.version(), target, "Return the cirrus checksum.", 1);

    auto missing_keys = candidate("missing_keys", "checksum is 7F3A", 100000, {});
    const auto before_refusal = core.version();
    expect_refusal("MISSING_RETRIEVAL_KEYS", [&] {
        core.ledger_create_candidate(core.version(), author, missing_keys);
    });
    check(core.version() == before_refusal, "retrieval-key refusal mutated the journal");

    const auto created = core.ledger_create_candidate(
        core.version(), author, candidate("canary_learning", "The cirrus checksum is 7F3A."));
    check(created.at("learning").at("maturity") == "candidate", "candidate was prematurely canonized");
    check(created.at("learning").at("created_seq") == created.at("seq"), "created_seq is not journal-derived");

    expect_refusal("SELF_PROMOTION", [&] {
        core.ledger_review(
            core.version(), author, "canary_learning", "confirmed",
            "independent_verification", "same-run self review must fail", {"test:self"});
    });
    const auto reviewed = core.ledger_review(
        core.version(), verifier, "canary_learning", "confirmed",
        "mechanical_outcome", "fixture output equals 7F3A", {"test:oracle-7F3A"});
    check(reviewed.at("review").at("resulting_maturity") == "verified", "independent review did not verify");

    // This is the independent oracle: a literal hand-constructed fixture set,
    // not an invocation of the selector or its relevance function.
    const std::set<std::string> independently_known_eligible{"canary_learning"};
    check(!independently_known_eligible.empty(), "canary oracle fixture became vacuous");
    const auto injection = core.ledger_prepare_injection(
        core.version(), target, "cirrus.checksum", {"domain:cirrus", "format:hex"},
        "normal", 2048, {}, json::array());
    const auto& receipt = injection.at("receipt");
    const std::set<std::string> selected(
        receipt.at("selected").begin(), receipt.at("selected").end());
    check(selected == independently_known_eligible,
        "non-empty oracle eligible set was not selected; non-vacuous canary tripped");
    const auto packet = receipt.at("exact_packet").get<std::string>();
    check(!packet.empty(), "selected canary produced an empty packet");
    check(receipt.at("exact_packet_hash") == dispatch_organ::sha256_text(packet),
        "exact injected packet hash does not verify");
    check(receipt.at("injected_bytes") == packet.size(), "injected byte receipt is wrong");

    const auto control = core.ledger_prepare_injection(
        core.version(), target, "cirrus.checksum", {"domain:cirrus", "format:hex"},
        "no_memory_replay", 2048, {}, json::array());
    check(control.at("receipt").at("selected").empty(), "no-memory control selected a learning");
    check(control.at("receipt").at("rejected").at(0).at("named_reason") == "no-memory replay",
        "no-memory control omission was not named");

    const auto clean_room = core.ledger_prepare_injection(
        core.version(), target, "cirrus.checksum", {"domain:cirrus", "format:hex"},
        "clean_room", 2048, {}, json::array());
    check(clean_room.at("receipt").at("rejected").at(0).at("named_reason") ==
        "clean-room/naïve baseline", "clean-room omission reason was not preserved exactly");

    const auto before_silent = core.version();
    expect_refusal("SILENT_OMISSION", [&] {
        core.ledger_prepare_injection(
            core.version(), target, "cirrus.checksum", {"domain:cirrus", "format:hex"},
            "normal", 2048, {"canary_learning"}, json::array());
    });
    check(core.version() == before_silent, "silent omission refusal mutated the journal");

    const auto legal_omission = core.ledger_prepare_injection(
        core.version(), target, "cirrus.checksum", {"domain:cirrus", "format:hex"},
        "normal", 2048, {"canary_learning"},
        json::array({{{"learning_id", "canary_learning"}, {"named_reason", "context budget"}}}));
    check(legal_omission.at("receipt").at("selected").empty(), "declared omission unexpectedly selected");
    check(legal_omission.at("receipt").at("rejected").at(0).at("named_reason") == "context budget",
        "legal omission reason was rewritten");

    expect_refusal("PROTECTED_NAMESPACE_FORBIDDEN", [&] {
        core.refuse_protected_namespace_write("canary_learning", "protected/operator/archive");
    });
}

void room_origin_consensus_never_canonizes() {
    TempDirectory temp("room-origin");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(workroom_policy());
    core.enable_ledger(ledger_policy());
    const auto author = identity("room_author", "task_room_author", "mount_room_author", "builder");
    const auto echo1 = identity("echo_1", "task_echo_1", "mount_echo_1", "scout");
    const auto echo2 = identity("echo_2", "task_echo_2", "mount_echo_2", "scout");
    const auto verifier = identity("room_verifier", "task_room_verify", "mount_room_verify", "verifier");
    for (const auto& mounted : {author, echo1, echo2, verifier}) core.mount(core.version(), mounted);

    const auto origin = core.workroom_post(core.version(), author, {
        "OBSERVATION", std::nullopt, "The false checksum is 0000.", {}, 1,
    });
    core.workroom_post(core.version(), echo1, {
        "ACK", origin.at("record").at("room_seq").get<std::uint64_t>(), "I echo 0000.", {}, 1,
    });
    core.workroom_post(core.version(), echo2, {
        "ACK", origin.at("record").at("room_seq").get<std::uint64_t>(), "I also echo 0000.", {}, 1,
    });
    auto room_candidate = candidate("room_false", "The cirrus checksum is 0000.");
    room_candidate["origin_room_seq"] = origin.at("record").at("room_seq");
    room_candidate["evidence_refs"] = json::array({"room:echo-1", "room:echo-2", "room:origin"});
    core.ledger_create_candidate(core.version(), author, room_candidate);
    const auto status = core.ledger_status();
    check(learning_by_id(status, "room_false").at("current_maturity") == "candidate",
        "N room echoes canonized an unverified claim");

    core.ledger_review(
        core.version(), verifier, "room_false", "contradicted", "independent_verification",
        "mechanical checksum fixture returns 7F3A", {"test:checksum-oracle"});
    check(learning_by_id(core.ledger_status(), "room_false").at("current_maturity") == "quarantined",
        "provenance-backed contradiction was not quarantined");
}

void contradiction_lineage_and_feedback_are_append_only() {
    TempDirectory temp("contradiction");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(workroom_policy());
    core.enable_ledger(ledger_policy());
    const auto author_a = identity("author_a", "task_a", "mount_a", "builder");
    const auto author_b = identity("author_b", "task_b", "mount_b", "builder");
    const auto verifier = identity("verifier", "task_v", "mount_v", "verifier");
    for (const auto& mounted : {author_a, author_b, verifier}) core.mount(core.version(), mounted);

    core.ledger_create_candidate(core.version(), author_a,
        candidate("claim_a", "The cirrus checksum is 7F3A."));
    core.ledger_create_candidate(core.version(), author_b,
        candidate("claim_not_a", "The cirrus checksum is not 7F3A.", 1));
    core.ledger_review(core.version(), verifier, "claim_a", "confirmed", "mechanical_outcome",
        "fixture v1", {"test:v1"});
    core.ledger_review(core.version(), verifier, "claim_not_a", "qualified", "independent_verification",
        "legacy fixture only", {"test:legacy"});
    core.ledger_reconcile(core.version(), verifier, {"claim_not_a", "claim_a"},
        "A holds for fixture v1; not-A is limited to the retired legacy fixture.", {"test:v1", "test:legacy"});

    auto status = core.ledger_status();
    check(status.at("reconciliations").size() == 1, "reconciliation link is absent");
    check(status.at("reconciliations").at(0).at("preserves_originals") == true,
        "reconciliation does not declare preserved originals");
    check(learning_by_id(status, "claim_a").at("claim") == "The cirrus checksum is 7F3A.",
        "claim A was rewritten");
    check(learning_by_id(status, "claim_not_a").at("claim") == "The cirrus checksum is not 7F3A.",
        "claim not-A was rewritten");

    core.ledger_feedback(core.version(), verifier, "claim_a", "CONTRADICTED", "run:first-counterexample");
    status = core.ledger_status();
    check(learning_by_id(status, "claim_a").at("current_maturity") == "verified",
        "single contradiction quarantined before the declared threshold");
    core.ledger_feedback(core.version(), verifier, "claim_a", "CONTRADICTED", "run:second-counterexample");
    status = core.ledger_status();
    const auto& quarantined = learning_by_id(status, "claim_a");
    check(quarantined.at("current_maturity") == "quarantined",
        "repeated contradiction did not quarantine");
    check(quarantined.at("feedback_counts").at("CONTRADICTED") == 2,
        "contradiction feedback history was flattened");
    check(quarantined.at("claim") == "The cirrus checksum is 7F3A.",
        "feedback silently edited immutable claim content");
    check(quarantined.at("zero_use") == true, "zero-use review surface is absent");
    core.ledger_feedback(core.version(), verifier, "claim_not_a", "USED", "run:used");
    core.ledger_feedback(core.version(), verifier, "claim_not_a", "IRRELEVANT", "run:irrelevant");
    core.ledger_feedback(core.version(), verifier, "claim_not_a", "UNOBSERVED", "run:unobserved");
    expect_refusal("MALFORMED_FEEDBACK", [&] {
        core.ledger_feedback(core.version(), verifier, "claim_not_a", "ENDORSED", "run:invalid");
    });
    status = core.ledger_status();
    const auto& feedback_surface = learning_by_id(status, "claim_not_a").at("feedback_counts");
    check(feedback_surface.at("USED") == 1 && feedback_surface.at("IRRELEVANT") == 1 &&
        feedback_surface.at("UNOBSERVED") == 1, "feedback enum events were not preserved");
    check(learning_by_id(status, "claim_not_a").at("stale_for_review") == true,
        "stale learning review surface is absent");
}

void restart_replays_learning_receipts_exactly_once() {
    TempDirectory temp("restart");
    const auto journal = temp.path() / "journal.jsonl";
    const auto author = identity("restart_author", "task_ra", "mount_ra", "builder");
    const auto verifier = identity("restart_verifier", "task_rv", "mount_rv", "verifier");
    const auto target = identity("restart_target", "task_rt", "mount_rt", "builder");
    std::uint64_t saved_version = 0;
    {
        Core core(Policy{}, journal);
        core.enable_workroom(workroom_policy());
        core.enable_ledger(ledger_policy());
        for (const auto& mounted : {author, verifier, target}) core.mount(core.version(), mounted);
        core.cast(core.version(), target, "Return checksum.", 1);
        core.ledger_create_candidate(core.version(), author,
            candidate("restart_learning", "The cirrus checksum is 7F3A."));
        core.ledger_review(core.version(), verifier, "restart_learning", "confirmed",
            "mechanical_outcome", "fixture v1", {"test:v1"});
        core.ledger_prepare_injection(core.version(), target, "cirrus.checksum",
            {"format:hex", "domain:cirrus"}, "normal", 2048, {}, json::array());
        saved_version = core.version();
    }
    {
        Core core(Policy{}, journal);
        core.enable_workroom(workroom_policy());
        core.enable_ledger(ledger_policy());
        const auto status = core.ledger_status();
        check(core.version() == saved_version, "restart changed committed state version");
        check(status.at("learnings").size() == 1, "restart duplicated or lost learning");
        check(status.at("injection_receipts").size() == 1, "restart duplicated or lost injection receipt");
        const auto& receipt = status.at("injection_receipts").at(0);
        check(receipt.at("exact_packet_hash") ==
            dispatch_organ::sha256_text(receipt.at("exact_packet").get<std::string>()),
            "replayed packet hash does not verify");
    }
}

} // namespace

int main() {
    try {
        candidate_promotion_and_nonvacuous_canary();
        std::cout << "PASS paired_candidate_refusal_promotion_nonvacuous_canary_exact_packet\n";
        room_origin_consensus_never_canonizes();
        std::cout << "PASS room_origin_consensus_never_canonizes_provenance_review_controls\n";
        contradiction_lineage_and_feedback_are_append_only();
        std::cout << "PASS contradictions_survive_reconciliation_feedback_quarantine_review_surface\n";
        restart_replays_learning_receipts_exactly_once();
        std::cout << "PASS ledger_restart_exactly_once_receipts\n";
        std::cout << "phase_c_ledger: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "phase_c_ledger: FAIL: " << error.what() << '\n';
        return 1;
    }
}
