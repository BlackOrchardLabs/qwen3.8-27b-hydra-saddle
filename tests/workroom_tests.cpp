#include "dispatch_organ/core.hpp"

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

using dispatch_organ::Core;
using dispatch_organ::Envelope;
using dispatch_organ::Policy;
using dispatch_organ::Refusal;
using dispatch_organ::RoomPost;
using dispatch_organ::WorkroomPolicy;
using dispatch_organ::json;

class TempDirectory {
public:
    explicit TempDirectory(std::string name) {
        path_ = std::filesystem::temp_directory_path() /
            ("dispatch-organ-workroom-" + std::to_string(::getpid()) + "-" + std::move(name));
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
    std::uint64_t epoch = 1) {
    const auto run = "run_" + task;
    return {
        std::move(member),
        "local_qwen_swarm",
        role,
        "bot",
        dispatch_organ::role_tools(role),
        "thread_room",
        "job_room",
        std::move(task),
        run,
        std::move(mount),
        epoch,
    };
}

WorkroomPolicy generous_policy() {
    WorkroomPolicy policy;
    policy.max_payload_bytes = 128;
    policy.max_posts_per_member = 16;
    policy.max_reads_per_member = 16;
    policy.max_chatter_rounds = 4;
    policy.max_unread_per_member = 4;
    policy.max_group_tags_per_member = 4;
    policy.max_delivery_batch = 4;
    return policy;
}

const json& member_status(const json& status, const std::string& member_id) {
    const auto& members = status.at("members");
    const auto found = std::find_if(members.begin(), members.end(), [&](const auto& member) {
        return member.at("member_id") == member_id;
    });
    if (found == members.end()) throw std::runtime_error("member missing from Workroom status");
    return *found;
}

void exact_enum_record_and_bound_pairs() {
    TempDirectory temp("enum-bounds");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    auto policy = generous_policy();
    policy.max_payload_bytes = 12;
    policy.max_chatter_rounds = 2;
    core.enable_workroom(policy);
    const auto sender = identity("sender", "task_sender", "mount_sender");
    core.mount(core.version(), sender);

    const std::vector<std::string> types{
        "QUESTION", "ANSWER", "OBSERVATION", "BLOCKED", "CHALLENGE", "ACK"};
    std::optional<std::uint64_t> prior;
    for (const auto& type : types) {
        const auto event = core.workroom_post(core.version(), sender, {
            type, prior, "payload", {}, 1,
        });
        const auto& record = event.at("record");
        for (const auto* key : {
            "room_seq", "thread_id", "job_id", "task_id", "run_id", "member_id",
            "mount_id", "job_epoch", "timestamp", "event_type", "reply_to",
            "bounded_payload", "payload_hash"}) {
            check(record.contains(key), std::string("room record missing ") + key);
        }
        check(record.at("payload_hash") == dispatch_organ::sha256_text("payload"), "payload hash mismatch");
        prior = record.at("room_seq").get<std::uint64_t>();
    }
    const auto version = core.version();
    expect_refusal("ROOM_EVENT_FORBIDDEN", [&] {
        (void)core.workroom_post(version, sender, {"GATE", std::nullopt, "payload", {}, 1});
    });
    expect_refusal("ROOM_EVENT_TYPE", [&] {
        (void)core.workroom_post(version, sender, {"VERDICTISH", std::nullopt, "payload", {}, 1});
    });
    expect_refusal("ROOM_PAYLOAD_CAP_EXCEEDED", [&] {
        (void)core.workroom_post(version, sender, {"OBSERVATION", std::nullopt, "payload-too-long", {}, 1});
    });
    expect_refusal("ROOM_ROUND_CAP_EXCEEDED", [&] {
        (void)core.workroom_post(version, sender, {"OBSERVATION", std::nullopt, "payload", {}, 3});
    });
    expect_refusal("STALE_PRECONDITION", [&] {
        (void)core.workroom_post(version - 1, sender, {"OBSERVATION", std::nullopt, "payload", {}, 1});
    });
    check(core.version() == version, "refused Workroom posts mutated the journal");
}

void revoked_mount_and_stale_epoch_refuse_at_head() {
    TempDirectory temp("ordering");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(generous_policy());
    const auto revoked = identity("revoked", "task_revoked", "mount_revoked");
    core.mount(core.version(), revoked);
    core.workroom_post(core.version(), revoked, {"OBSERVATION", std::nullopt, "before revoke", {}, 1});
    core.revoke_mount(core.version(), revoked);
    expect_refusal("MOUNT_REVOKED", [&] {
        (void)core.workroom_post(core.version(), revoked, {"OBSERVATION", std::nullopt, "after revoke", {}, 1});
    });

    const auto stale = identity("stale", "task_stale", "mount_stale");
    core.mount(core.version(), stale);
    core.bump_job_epoch(core.version(), stale.job_id);
    expect_refusal("STALE_EPOCH", [&] {
        (void)core.workroom_post(core.version(), stale, {"OBSERVATION", std::nullopt, "stale epoch", {}, 1});
    });
    expect_refusal("STALE_EPOCH", [&] {
        (void)core.workroom_read(core.version(), stale, "opaque_local_qwen", "BETWEEN_TURNS", 1);
    });
}

void groups_are_tags_and_empty_group_vanishes() {
    TempDirectory temp("groups");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(generous_policy());
    const auto sender = identity("sender", "task_sender", "mount_sender");
    const auto receiver = identity("receiver", "task_receiver", "mount_receiver", "scout");
    core.mount(core.version(), sender);
    core.mount(core.version(), receiver);
    core.set_group_tags(core.version(), sender, {"alpha"});
    core.set_group_tags(core.version(), receiver, {"alpha"});
    check(core.workroom_status().at("derived_groups").at("alpha") == 2, "derived group count is wrong");
    core.workroom_post(core.version(), sender, {"QUESTION", std::nullopt, "group question", {"alpha"}, 1});
    expect_refusal("GROUP_MEMBERSHIP_REQUIRED", [&] {
        (void)core.workroom_post(core.version(), sender, {"QUESTION", std::nullopt, "wrong group", {"beta"}, 1});
    });
    core.set_group_tags(core.version(), sender, {});
    core.set_group_tags(core.version(), receiver, {});
    check(!core.workroom_status().at("derived_groups").contains("alpha"), "empty group survived as a registry entry");
}

void opaque_queue_delivers_once_at_proven_boundary() {
    TempDirectory temp("delivery");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(generous_policy());
    const auto sender = identity("sender", "task_sender", "mount_sender");
    const auto receiver = identity("receiver", "task_receiver", "mount_receiver", "scout");
    core.mount(core.version(), sender);
    core.mount(core.version(), receiver);
    expect_refusal("ADAPTER_CONFORMANCE_FAILED", [&] {
        (void)core.prove_adapter(core.version(), "lying_immediate");
    });
    const auto adapter_proof = core.prove_adapter(core.version(), "opaque_local_qwen");
    const auto measurements = adapter_proof.at("fixture_measurements");
    check(measurements.at("during_opaque") == 0, "adapter fixture delivered during opaque call");
    check(measurements.at("at_first_safe_boundary") == 1, "adapter fixture missed first safe boundary");
    check(measurements.at("at_second_safe_boundary") == 0, "adapter fixture duplicated delivery");
    check(measurements.at("after_epoch_cancel") == 0, "adapter fixture delivered after cancel");
    core.set_member_execution(core.version(), receiver, "OPAQUE_CALL");
    const auto post = core.workroom_post(core.version(), sender, {
        "OBSERVATION", std::nullopt, "queued during opaque call", {}, 1,
    });
    const auto post_seq = post.at("record").at("room_seq").get<std::uint64_t>();
    expect_refusal("NO_SAFE_BOUNDARY", [&] {
        (void)core.workroom_read(core.version(), receiver, "opaque_local_qwen", "BETWEEN_TURNS", 4);
    });
    const auto queued = member_status(core.workroom_status(), "receiver");
    check(queued.at("watermark") == 0, "opaque polling advanced the watermark");
    check(queued.at("unread_count") == 1 && queued.at("lag_visible") == true, "queued lag is not visible");

    core.set_member_execution(core.version(), receiver, "SAFE_BOUNDARY");
    const auto delivery = core.workroom_read(
        core.version(), receiver, "opaque_local_qwen", "BETWEEN_TURNS", 4);
    check(delivery.at("delivered").size() == 1, "queued record was not delivered");
    check(delivery.at("delivered").at(0).at("room_seq") == post_seq, "wrong record delivered");
    check(delivery.at("durably_observed") == true, "delivery did not durably observe");
    check(delivery.at("seen") == false && delivery.at("understood") == false,
        "delivery was falsely narrated as seen/understood");
    const auto second = core.workroom_read(
        core.version(), receiver, "opaque_local_qwen", "BETWEEN_TURNS", 4);
    check(second.at("delivered").empty(), "record delivered more than once");
}

void epoch_change_cancels_queued_old_epoch_delivery() {
    TempDirectory temp("epoch-cancel");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(generous_policy());
    const auto sender = identity("sender", "task_sender", "mount_sender");
    const auto old_receiver = identity("receiver", "task_receiver_old", "mount_receiver_old", "scout");
    core.mount(core.version(), sender);
    core.mount(core.version(), old_receiver);
    core.prove_adapter(core.version(), "opaque_local_qwen");
    core.set_member_execution(core.version(), old_receiver, "OPAQUE_CALL");
    core.workroom_post(core.version(), sender, {
        "OBSERVATION", std::nullopt, "cancel me at epoch change", {}, 1,
    });
    core.bump_job_epoch(core.version(), sender.job_id);
    const auto new_receiver = identity(
        "receiver", "task_receiver_new", "mount_receiver_new", "scout", 2);
    core.mount(core.version(), new_receiver);
    core.set_member_execution(core.version(), new_receiver, "SAFE_BOUNDARY");
    const auto delivery = core.workroom_read(
        core.version(), new_receiver, "opaque_local_qwen", "BETWEEN_TURNS", 4);
    check(delivery.at("delivered").empty(), "stale-epoch queued record survived adapter cancel semantics");
}

void nonreader_is_skipped_without_wedging_and_lag_stays_visible() {
    TempDirectory temp("nonreader");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    auto policy = generous_policy();
    policy.max_unread_per_member = 2;
    core.enable_workroom(policy);
    const auto sender = identity("sender", "task_sender", "mount_sender");
    const auto receiver = identity("receiver", "task_receiver", "mount_receiver", "scout");
    core.mount(core.version(), sender);
    core.mount(core.version(), receiver);
    core.workroom_post(core.version(), sender, {"OBSERVATION", std::nullopt, "one", {}, 1});
    const auto boundary = core.workroom_post(core.version(), sender, {"OBSERVATION", std::nullopt, "two", {}, 1});
    check(std::find(boundary.at("skipped_members").begin(), boundary.at("skipped_members").end(), "receiver") !=
        boundary.at("skipped_members").end(), "nonreader skip was not recorded at cap boundary");
    core.workroom_post(core.version(), sender, {"OBSERVATION", std::nullopt, "three", {}, 1});
    const auto state = member_status(core.workroom_status(), "receiver");
    check(state.at("skipped_at_cap") == true, "nonreader skip is not visible");
    check(state.at("unread_count") == 3 && state.at("lag_visible") == true, "nonreader lag was hidden");
}

void post_and_read_rate_caps_have_positive_and_negative_arms() {
    TempDirectory temp("rate-caps");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    auto policy = generous_policy();
    policy.max_posts_per_member = 1;
    policy.max_reads_per_member = 1;
    core.enable_workroom(policy);
    const auto sender = identity("sender", "task_sender", "mount_sender");
    const auto receiver = identity("receiver", "task_receiver", "mount_receiver", "scout");
    core.mount(core.version(), sender);
    core.mount(core.version(), receiver);
    core.prove_adapter(core.version(), "opaque_local_qwen");
    core.set_member_execution(core.version(), receiver, "SAFE_BOUNDARY");
    core.workroom_post(core.version(), sender, {"OBSERVATION", std::nullopt, "allowed post", {}, 1});
    expect_refusal("ROOM_POST_CAP_EXCEEDED", [&] {
        (void)core.workroom_post(core.version(), sender, {"OBSERVATION", std::nullopt, "second post", {}, 1});
    });
    core.workroom_read(core.version(), receiver, "opaque_local_qwen", "BETWEEN_TURNS", 1);
    expect_refusal("ROOM_READ_CAP_EXCEEDED", [&] {
        (void)core.workroom_read(core.version(), receiver, "opaque_local_qwen", "BETWEEN_TURNS", 1);
    });
}

void restart_preserves_room_sequence_and_watermark_without_duplicates() {
    TempDirectory temp("restart");
    const auto journal = temp.path() / "journal.jsonl";
    const auto policy = generous_policy();
    const auto sender = identity("sender", "task_sender", "mount_sender");
    const auto receiver = identity("receiver", "task_receiver", "mount_receiver", "scout");
    std::uint64_t room_seq = 0;
    std::uint64_t watermark = 0;
    std::size_t record_count = 0;
    {
        Core before(Policy{}, journal);
        before.enable_workroom(policy);
        before.mount(before.version(), sender);
        before.mount(before.version(), receiver);
        before.prove_adapter(before.version(), "opaque_local_qwen");
        before.set_member_execution(before.version(), receiver, "SAFE_BOUNDARY");
        before.workroom_post(before.version(), sender, {"ANSWER", std::nullopt, "durable", {}, 1});
        before.workroom_read(before.version(), receiver, "opaque_local_qwen", "AFTER_TOOL", 2);
        const auto status = before.workroom_status();
        room_seq = status.at("room_seq").get<std::uint64_t>();
        watermark = member_status(status, "receiver").at("watermark").get<std::uint64_t>();
        record_count = status.at("record_count").get<std::size_t>();
    }
    Core after(Policy{}, journal);
    after.enable_workroom(policy);
    const auto status = after.workroom_status();
    check(status.at("room_seq") == room_seq, "room_seq changed across replay");
    check(member_status(status, "receiver").at("watermark") == watermark, "watermark changed across replay");
    check(status.at("record_count") == record_count, "replay duplicated a Workroom record");
}

void room_records_never_enter_typed_gate_evidence() {
    TempDirectory temp("typed-gate");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(generous_policy());
    const auto first = identity("builder_one", "task_one", "mount_one");
    const auto second = identity("builder_two", "task_two", "mount_two");
    core.mount(core.version(), first);
    core.cast(core.version(), first, "first", 1);
    auto dispatch = core.begin_dispatch(core.version(), first);
    core.acknowledge_dispatch(dispatch.state_version, first, dispatch.call_id, {{"ok", true}});
    auto result = core.begin_result(core.version(), first);
    const auto first_result = core.complete_result(result.state_version, first, result.call_id, {
        {"content", "first evidence"}, {"content_sha256", dispatch_organ::sha256_text("first evidence")},
        {"completeness", "FULL"},
    });

    core.mount(core.version(), second);
    core.cast(core.version(), second, "second", 1);
    dispatch = core.begin_dispatch(core.version(), second);
    core.acknowledge_dispatch(dispatch.state_version, second, dispatch.call_id, {{"ok", true}});
    result = core.begin_result(core.version(), second);
    const auto room = core.workroom_post(core.version(), first, {
        "OBSERVATION", std::nullopt, "I claim every gate passed", {}, 1,
    });
    const auto second_result = core.complete_result(core.version(), second, result.call_id, {
        {"content", "second evidence"}, {"content_sha256", dispatch_organ::sha256_text("second evidence")},
        {"completeness", "FULL"},
    });
    check(first_result.at("seq") < room.at("seq") && room.at("seq") < second_result.at("seq"),
        "room record was not interleaved inside the evidence sequence range");
    const auto gate = core.gate_evidence_view();
    check(gate.at("range_fold") == false, "gate view range-folded the journal");
    check(gate.at("type_filter") == json::array({"RESULT"}), "gate filter is not exact");
    check(gate.at("evidence").size() == 2, "typed gate view did not return exactly two results");
    for (const auto& evidence : gate.at("evidence")) {
        check(evidence.at("event_type") == "RESULT", "room record reached gate evidence ingestion");
    }
}

void commutative_chatter_blind_appends_concurrently() {
    TempDirectory temp("commutative");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(generous_policy());
    constexpr int count = 12;
    std::mutex receipts_mutex;
    std::vector<json> receipts;
    std::vector<std::string> failures;
    std::vector<std::thread> threads;
    for (int index = 0; index < count; ++index) {
        threads.emplace_back([&, index] {
            try {
                auto receipt = core.workroom_commutative_observation(
                    "thread_room", "job_room", "comm_task_" + std::to_string(index),
                    "comm_run_" + std::to_string(index), 1, "commutative " + std::to_string(index));
                std::lock_guard lock(receipts_mutex);
                receipts.push_back(std::move(receipt));
            } catch (const std::exception& error) {
                std::lock_guard lock(receipts_mutex);
                failures.push_back(error.what());
            }
        });
    }
    for (auto& thread : threads) thread.join();
    check(failures.empty(), "concurrent commutative append failed");
    check(static_cast<int>(receipts.size()) == count, "not every commutative record committed");
    std::set<std::uint64_t> sequences;
    for (const auto& receipt : receipts) {
        check(receipt.at("read_set").empty(), "commutative record carried a version token");
        sequences.insert(receipt.at("seq").get<std::uint64_t>());
    }
    check(static_cast<int>(sequences.size()) == count, "commutative records shared a sequence");

    const auto sender = identity("sender", "task_sender", "mount_sender");
    const auto before_one_more_commutative = core.version();
    (void)core.workroom_commutative_observation(
        "thread_room", "job_room", "comm_rebase_task", "comm_rebase_run", 1, "rebase fixture");
    const auto mount_receipt = core.mount(before_one_more_commutative, sender);
    check(mount_receipt.at("rebased_from_state_version") == before_one_more_commutative,
        "authority mutation did not record narrow commutative rebase origin");
    check(mount_receipt.at("revalidated_at_head") == before_one_more_commutative + 1,
        "authority mutation did not record committed revalidation head");

    const auto stale_post_version = core.version();
    (void)core.workroom_commutative_observation(
        "thread_room", "job_room", "comm_stale_task", "comm_stale_run", 1, "stale fixture");
    expect_refusal("STALE_PRECONDITION", [&] {
        (void)core.workroom_post(stale_post_version, sender, {
            "OBSERVATION", std::nullopt, "conditional stale", {}, 1,
        });
    });
    const auto post = core.workroom_post(core.version(), sender, {
        "OBSERVATION", std::nullopt, "conditional", {}, 1,
    });
    check(!post.at("read_set").empty() && post.at("read_set").at(0).contains("state_version"),
        "non-commutative post lacked a version token");
}

void authority_writer_advances_before_queued_chatter_drains() {
    TempDirectory temp("writer-priority");
    Core core(Policy{}, temp.path() / "journal.jsonl");
    core.enable_workroom(generous_policy());
    constexpr int count = 64;
    std::atomic<int> ready{0};
    std::atomic<int> completed{0};
    std::atomic<bool> go{false};
    std::mutex failure_mutex;
    std::vector<std::string> failures;
    std::vector<std::thread> threads;
    for (int index = 0; index < count; ++index) {
        threads.emplace_back([&, index] {
            ++ready;
            while (!go.load()) std::this_thread::yield();
            try {
                (void)core.workroom_commutative_observation(
                    "thread_room", "job_room", "priority_task_" + std::to_string(index),
                    "priority_run_" + std::to_string(index), 1, "priority chatter");
                ++completed;
            } catch (const std::exception& error) {
                std::lock_guard lock(failure_mutex);
                failures.push_back(error.what());
            }
        });
    }
    while (ready.load() != count) std::this_thread::yield();
    go = true;
    while (completed.load() == 0) std::this_thread::yield();

    const auto authority = identity("authority_member", "authority_task", "authority_mount");
    json authority_receipt;
    while (true) {
        const auto expected = core.version();
        try {
            authority_receipt = core.mount(expected, authority);
            break;
        } catch (const Refusal& refusal) {
            if (refusal.code() != "STALE_PRECONDITION") throw;
        }
    }
    for (auto& thread : threads) thread.join();
    check(failures.empty(), "priority chatter append failed");
    check(completed.load() == count, "not all priority chatter completed");
    check(authority_receipt.at("seq").get<int>() < count + 1,
        "authority writer starved until every chatter writer drained");
}

} // namespace

int main() {
    try {
        exact_enum_record_and_bound_pairs();
        std::cout << "PASS exact_six_record_types_and_bounded_post_pairs\n";
        revoked_mount_and_stale_epoch_refuse_at_head();
        std::cout << "PASS cross_surface_revocation_and_stale_epoch_refusals\n";
        groups_are_tags_and_empty_group_vanishes();
        std::cout << "PASS groups_are_member_tags_without_registry\n";
        opaque_queue_delivers_once_at_proven_boundary();
        std::cout << "PASS opaque_queue_safe_boundary_exactly_once_not_seen\n";
        epoch_change_cancels_queued_old_epoch_delivery();
        std::cout << "PASS adapter_epoch_cancel_drops_stale_queued_delivery\n";
        nonreader_is_skipped_without_wedging_and_lag_stays_visible();
        std::cout << "PASS nonreader_skip_and_visible_lag_without_wedge\n";
        post_and_read_rate_caps_have_positive_and_negative_arms();
        std::cout << "PASS member_post_and_read_rate_caps_paired\n";
        restart_preserves_room_sequence_and_watermark_without_duplicates();
        std::cout << "PASS restart_room_sequence_watermark_no_duplicates\n";
        room_records_never_enter_typed_gate_evidence();
        std::cout << "PASS structural_room_cannot_reach_typed_gate_evidence\n";
        commutative_chatter_blind_appends_concurrently();
        std::cout << "PASS concurrent_commutative_chatter_and_conditional_post_tokens\n";
        authority_writer_advances_before_queued_chatter_drains();
        std::cout << "PASS authority_priority_prevents_chatter_starvation\n";
        std::cout << "phase_b_workroom: PASS\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "phase_b_workroom: FAIL: " << error.what() << '\n';
        return 1;
    }
}
