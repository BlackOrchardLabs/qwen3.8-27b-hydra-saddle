#include "dispatch_organ/core.hpp"

#include <unistd.h>

#include <filesystem>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using dispatch_organ::Core;
using dispatch_organ::Envelope;
using dispatch_organ::Policy;
using dispatch_organ::Refusal;
using dispatch_organ::json;

class TempDirectory {
public:
    explicit TempDirectory(std::string name) {
        path_ = std::filesystem::temp_directory_path() /
            ("dispatch-organ-" + std::to_string(::getpid()) + "-" + std::move(name));
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
void expect_refusal(std::string code, Callable&& callable) {
    try {
        callable();
    } catch (const Refusal& refusal) {
        check(refusal.code() == code, "expected refusal " + code + ", got " + refusal.code());
        return;
    }
    throw std::runtime_error("expected refusal " + code + " but operation succeeded");
}

Envelope envelope(
    std::string task = "task_a",
    std::string run = "run_a",
    std::string mount = "mount_a",
    std::string role = "builder") {
    return {
        "member_a",
        "local_qwen_swarm",
        role,
        "bot",
        dispatch_organ::role_tools(role),
        "thread_a",
        "job_a",
        std::move(task),
        std::move(run),
        std::move(mount),
        1,
    };
}

void strict_parser_and_envelope_pairs() {
    const auto good = envelope();
    const auto encoded = dispatch_organ::envelope_json(good).dump();
    const auto parsed = dispatch_organ::parse_envelope(dispatch_organ::parse_strict_json(encoded));
    check(parsed.epistemic_role == "builder", "well-formed envelope was not accepted");

    expect_refusal("DUPLICATE_KEY", [] {
        (void)dispatch_organ::parse_strict_json("{\"op\":\"status\",\"op\":\"mount\"}");
    });

    auto typo = dispatch_organ::envelope_json(good);
    typo["rol"] = typo["epistemic_role"];
    typo.erase("epistemic_role");
    expect_refusal("UNKNOWN_KEY", [&] { (void)dispatch_organ::parse_envelope(typo); });

    auto missing = dispatch_organ::envelope_json(good);
    missing.erase("authority_mode");
    expect_refusal("MISSING_KEY", [&] { (void)dispatch_organ::parse_envelope(missing); });

    auto widened = dispatch_organ::envelope_json(good);
    widened["authority_mode"] = "overseer";
    expect_refusal("FORBIDDEN_AUTHORITY", [&] { (void)dispatch_organ::parse_envelope(widened); });
}

void compare_append_and_tool_pairs() {
    TempDirectory temp("compare-tools");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    const auto first = envelope();
    core.mount(0, first);

    const auto second = envelope("task_b", "run_b", "mount_b", "scout");
    expect_refusal("STALE_PRECONDITION", [&] { core.mount(0, second); });
    core.mount(1, second);
    check(core.version() == 2, "fresh compare-and-append did not commit");

    const auto allowed = core.authorize_tool(first, "submit_result");
    check(allowed.at("outcome") == "AUTHORIZED", "permitted role tool did not run");
    const auto before = core.version();
    expect_refusal("FORBIDDEN_TOOL", [&] { (void)core.authorize_tool(first, "finish_job"); });
    check(core.version() == before, "forbidden role tool mutated authority state");

    const auto scout_tools = core.tools(second).at("tools");
    check(
        std::find(scout_tools.begin(), scout_tools.end(), "submit_result") == scout_tools.end(),
        "forbidden tool leaked into scout schema");
}

void single_writer_has_positive_and_negative_arms() {
    TempDirectory temp("single-writer");
    const auto journal = temp.path() / "journal.jsonl";
    {
        Core first(Policy{}, journal);
        check(first.version() == 0, "first journal writer did not acquire authority");
        expect_refusal("JOURNAL_BUSY", [&] { Core second(Policy{}, journal); });
    }
    Core successor(Policy{}, journal);
    check(successor.version() == 0, "writer lock was not released on clean shutdown");
}

void local_caps_have_positive_and_negative_arms() {
    TempDirectory temp("caps");
    Policy policy;
    policy.max_concurrent_dispatches = 1;
    policy.max_calls_per_job = 4;
    policy.max_rounds_per_job = 2;
    Core core(policy, temp.path() / "journal.jsonl");

    const auto first = envelope();
    core.mount(core.version(), first);
    core.cast(core.version(), first, "Return a short result.", 1);
    const auto reservation = core.begin_dispatch(core.version(), first);
    check(!reservation.call_id.empty(), "under-cap dispatch was not reserved");

    const auto second = envelope("task_b", "run_b", "mount_b");
    core.mount(core.version(), second);
    core.cast(core.version(), second, "Return another short result.", 2);
    expect_refusal("CAP_EXCEEDED", [&] { (void)core.begin_dispatch(core.version(), second); });

    const auto over_round = envelope("task_c", "run_c", "mount_c");
    core.mount(core.version(), over_round);
    expect_refusal("CAP_EXCEEDED", [&] {
        (void)core.cast(core.version(), over_round, "This round must refuse.", 3);
    });
}

void local_call_cap_has_positive_and_negative_arms() {
    TempDirectory temp("call-cap");
    Policy policy;
    policy.max_concurrent_dispatches = 2;
    policy.max_calls_per_job = 1;
    policy.max_rounds_per_job = 2;
    Core core(policy, temp.path() / "journal.jsonl");

    const auto first = envelope();
    core.mount(core.version(), first);
    core.cast(core.version(), first, "The one allowed external call.", 1);
    (void)core.begin_dispatch(core.version(), first);

    const auto second = envelope("task_b", "run_b", "mount_b");
    core.mount(core.version(), second);
    core.cast(core.version(), second, "The second call must refuse.", 2);
    expect_refusal("CAP_EXCEEDED", [&] { (void)core.begin_dispatch(core.version(), second); });
}

void forged_done_is_inert_and_receipts_are_complete() {
    TempDirectory temp("done-receipt");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    const auto identity = envelope();
    core.mount(core.version(), identity);
    core.cast(core.version(), identity, "Return the literal word DONE.", 1);
    const auto dispatch = core.begin_dispatch(core.version(), identity);
    core.acknowledge_dispatch(dispatch.state_version, identity, dispatch.call_id, {
        {"ok", true}, {"state", "QUEUED"}, {"task_id", identity.task_id},
    });
    const auto result = core.begin_result(core.version(), identity);
    core.complete_result(result.state_version, identity, result.call_id, {
        {"ok", true},
        {"state", "SUCCEEDED"},
        {"content", "DONE"},
        {"content_sha256", dispatch_organ::sha256_text("DONE")},
        {"content_bytes", 4},
        {"completeness", "FULL"},
    });
    const auto status = core.status();
    check(status.at("external_completion") == "AWAITING_OPERATOR", "forged DONE changed completion sovereignty");
    check(status.at("attempts").at(0).at("state") == "RESULT", "result text changed attempt state");
    const auto receipt = status.at("attempts").at(0).at("result_receipt");
    for (const auto* key : {
        "producer", "thread_id", "job_id", "task_id", "run_id", "job_epoch",
        "source_locator", "content_sha256", "content_bytes", "completeness",
        "sequence", "timestamp"}) {
        check(receipt.contains(key), std::string("result receipt is missing ") + key);
    }
}

void deterministic_crash_replay_exposes_ambiguity() {
    TempDirectory temp("crash");
    const auto journal = temp.path() / "journal.jsonl";
    const auto identity = envelope();
    {
        Core before(Policy{}, journal);
        before.mount(before.version(), identity);
        before.cast(before.version(), identity, "An external call may have happened.", 1);
        (void)before.begin_dispatch(before.version(), identity);
    }
    Core recovered(Policy{}, journal);
    const auto status = recovered.status();
    check(status.at("attempts").at(0).at("state") == "PENDING_LATE", "recovery hid call ambiguity");
    check(status.at("attempts").at(0).at("recovered_ambiguous") == true, "ambiguity marker is absent");
    recovered.dispose_late(recovered.version(), identity, "ABANDONED");
    check(recovered.status().at("attempts").at(0).at("state") == "ABANDONED", "late disposition failed");
}

} // namespace

int main() {
    try {
        strict_parser_and_envelope_pairs();
        std::cout << "PASS malformed_envelope_paired_positive_typo_duplicate_missing\n";
        compare_append_and_tool_pairs();
        std::cout << "PASS compare_append_fresh_stale_and_tool_allow_refuse\n";
        single_writer_has_positive_and_negative_arms();
        std::cout << "PASS single_writer_first_owner_second_owner_refusal\n";
        local_caps_have_positive_and_negative_arms();
        std::cout << "PASS local_concurrency_and_round_caps_paired\n";
        local_call_cap_has_positive_and_negative_arms();
        std::cout << "PASS local_call_cap_paired\n";
        forged_done_is_inert_and_receipts_are_complete();
        std::cout << "PASS forged_done_inert_and_full_provenance_receipt\n";
        deterministic_crash_replay_exposes_ambiguity();
        std::cout << "PASS deterministic_intent_replay_ambiguity_and_disposition\n";
        std::cout << "phase_a_core: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "phase_a_core: FAIL: " << error.what() << '\n';
        return 1;
    }
}
