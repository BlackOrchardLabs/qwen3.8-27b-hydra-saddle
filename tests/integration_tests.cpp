#include "dispatch_organ/core.hpp"

#include <unistd.h>

#include <filesystem>
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
            ("dispatch-organ-integration-" + std::to_string(::getpid()) + "-" + std::move(name));
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
    throw std::runtime_error("expected refusal " + code);
}

Envelope identity(std::string member, std::string task, std::string run,
                  std::string mount, std::string role) {
    return {
        std::move(member), "local_qwen_swarm", role, "bot",
        dispatch_organ::role_tools(role), "integration_thread", "integration_job",
        std::move(task), std::move(run), std::move(mount), 1,
    };
}

void gate_capability_and_receipt_are_journaled() {
    TempDirectory temp("gate-receipt");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    const auto builder = identity("builder", "builder_task", "builder_run", "builder_mount", "builder");
    const auto verifier = identity("verifier", "verifier_task", "verifier_run", "verifier_mount", "verifier");
    core.mount(core.version(), builder);
    core.mount(core.version(), verifier);

    expect_refusal("FORBIDDEN_TOOL", [&] {
        (void)core.begin_gate(core.version(), builder, "builder_task", "static_html");
    });
    const auto before = core.version();
    const auto reservation = core.begin_gate(before, verifier, "builder_task", "static_html");
    check(core.version() == before + 1, "gate intent did not commit before the external gate call");
    core.complete_gate(reservation.state_version, verifier, reservation.call_id, json{
        {"ok", true}, {"job_id", "integration_job"}, {"task_id", "builder_task"},
        {"gate", "static_html"}, {"passed", true}, {"exit_code", 0},
        {"timed_out", false}, {"evidence", "fixture gate passed"}, {"task_state", "GATED"},
    });
    const auto status = core.status();
    check(status.at("gate_attempts").size() == 1, "gate receipt is absent from status");
    check(status.at("gate_attempts").at(0).at("state") == "RESULT", "gate did not reach RESULT");
    check(status.at("gate_attempts").at(0).at("receipt").at("passed") == true,
        "passing gate receipt was not retained");
}

void gate_crash_exposes_pending_late_without_replay() {
    TempDirectory temp("gate-crash");
    const auto journal = temp.path() / "journal.jsonl";
    const auto verifier = identity("verifier", "verifier_task", "verifier_run", "verifier_mount", "verifier");
    std::string call_id;
    {
        Core core(Policy{}, journal);
        core.mount(core.version(), verifier);
        const auto reservation = core.begin_gate(core.version(), verifier, "builder_task", "static_html");
        call_id = reservation.call_id;
    }
    Core recovered(Policy{}, journal);
    const auto status = recovered.status();
    check(status.at("gate_attempts").size() == 1, "recovery duplicated or lost gate intent");
    const auto& attempt = status.at("gate_attempts").at(0);
    check(attempt.at("call_id") == call_id, "recovery changed gate call identity");
    check(attempt.at("state") == "PENDING_LATE", "ambiguous gate was not exposed pending-late");
    check(attempt.at("recovered_ambiguous") == true, "gate ambiguity marker is absent");
    check(recovered.version() == 2, "recovery fabricated a gate acknowledgement event");
}

void nonterminal_harness_snapshot_cannot_become_result() {
    TempDirectory temp("nonterminal-result");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    const auto builder = identity("builder", "builder_task", "builder_run", "builder_mount", "builder");
    core.mount(core.version(), builder);
    core.cast(core.version(), builder, "perform a bounded task", 1);
    const auto dispatch = core.begin_dispatch(core.version(), builder);
    core.acknowledge_dispatch(dispatch.state_version, builder, dispatch.call_id, json{
        {"ok", true}, {"task_id", "builder_task"}, {"state", "QUEUED"},
    });
    const auto result = core.begin_result(core.version(), builder);
    expect_refusal("NONTERMINAL_EVIDENCE", [&] {
        (void)core.complete_result(result.state_version, builder, result.call_id, json{
            {"ok", true}, {"task_id", "builder_task"}, {"state", "RUNNING"},
            {"content", ""},
        });
    });
    core.fail_call(result.state_version, builder, result.call_id, "nonterminal external snapshot");
    check(core.status().at("attempts").at(0).at("state") == "STRANDED",
        "nonterminal external snapshot did not leave honest stranded lineage");
}

} // namespace

int main() {
    try {
        gate_capability_and_receipt_are_journaled();
        std::cout << "PASS journaled_harness_gate_capability_and_receipt\n";
        gate_crash_exposes_pending_late_without_replay();
        std::cout << "PASS gate_crash_pending_late_without_false_replay\n";
        nonterminal_harness_snapshot_cannot_become_result();
        std::cout << "PASS nonterminal_harness_snapshot_refused_as_result\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
