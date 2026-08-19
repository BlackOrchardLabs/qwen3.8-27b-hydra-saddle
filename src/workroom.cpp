#include "dispatch_organ/core.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace dispatch_organ {
namespace {

const std::set<std::string> kRoomEventTypes{
    "ACK", "ANSWER", "BLOCKED", "CHALLENGE", "OBSERVATION", "QUESTION",
};

const std::set<std::string> kForbiddenRoomEventTypes{
    "ARM", "FINISH", "GATE", "SPEND", "VERDICT",
};

void validate_room_token(const std::string& value, std::string_view field) {
    if (value.empty() || value.size() > 128) {
        throw Refusal("MALFORMED_ROOM_RECORD", std::string(field) + " must contain 1..128 characters");
    }
    const bool valid = std::all_of(value.begin(), value.end(), [](unsigned char ch) {
        return std::isalnum(ch) != 0 || ch == '_' || ch == '-' || ch == '.' || ch == ':';
    });
    if (!valid) {
        throw Refusal("MALFORMED_ROOM_RECORD", std::string(field) + " contains a forbidden character");
    }
}

std::vector<std::string> validate_tags(
    std::vector<std::string> tags,
    int maximum,
    std::string_view context) {
    if (static_cast<int>(tags.size()) > maximum) {
        throw Refusal("GROUP_TAG_CAP_EXCEEDED", std::string(context) + " exceeds max_group_tags_per_member");
    }
    std::sort(tags.begin(), tags.end());
    if (std::adjacent_find(tags.begin(), tags.end()) != tags.end()) {
        throw Refusal("MALFORMED_GROUP_TAGS", std::string(context) + " contains duplicate tags");
    }
    for (const auto& tag : tags) validate_room_token(tag, "group_tag");
    return tags;
}

bool intersects(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    if (left.empty()) return true;
    return std::any_of(left.begin(), left.end(), [&](const auto& item) {
        return std::binary_search(right.begin(), right.end(), item);
    });
}

json run_adapter_conformance_fixture(const std::string& adapter_id) {
    if (adapter_id != "opaque_local_qwen" && adapter_id != "lying_immediate") {
        throw Refusal("UNKNOWN_ADAPTER", "adapter has no Phase B conformance fixture");
    }
    const bool lies_about_opaque_delivery = adapter_id == "lying_immediate";
    int queued = 1;
    auto deliver = [&](bool safe_boundary) {
        if (queued == 0) return 0;
        if (!safe_boundary && !lies_about_opaque_delivery) return 0;
        const auto delivered = queued;
        queued = 0;
        return delivered;
    };
    const int during_opaque = deliver(false);
    const int at_first_safe_boundary = deliver(true);
    const int at_second_safe_boundary = deliver(true);
    queued = 1;
    queued = 0; // epoch/cancel invalidates queued content before delivery
    const int after_epoch_cancel = deliver(true);
    const bool passed = during_opaque == 0 && at_first_safe_boundary == 1 &&
        at_second_safe_boundary == 0 && after_epoch_cancel == 0;
    return {
        {"passed", passed},
        {"during_opaque", during_opaque},
        {"at_first_safe_boundary", at_first_safe_boundary},
        {"at_second_safe_boundary", at_second_safe_boundary},
        {"after_epoch_cancel", after_epoch_cancel},
    };
}

} // namespace

WorkroomPolicy parse_workroom_policy(const json& value) {
    require_exact_keys(value, {
        "max_chatter_rounds", "max_delivery_batch", "max_group_tags_per_member",
        "max_payload_bytes", "max_posts_per_member", "max_reads_per_member",
        "max_unread_per_member",
    }, {}, "workroom policy");
    WorkroomPolicy policy;
    try {
        policy.max_payload_bytes = value.at("max_payload_bytes").get<int>();
        policy.max_posts_per_member = value.at("max_posts_per_member").get<int>();
        policy.max_reads_per_member = value.at("max_reads_per_member").get<int>();
        policy.max_chatter_rounds = value.at("max_chatter_rounds").get<int>();
        policy.max_unread_per_member = value.at("max_unread_per_member").get<int>();
        policy.max_group_tags_per_member = value.at("max_group_tags_per_member").get<int>();
        policy.max_delivery_batch = value.at("max_delivery_batch").get<int>();
    } catch (const std::exception& error) {
        throw Refusal("MALFORMED_WORKROOM_POLICY", error.what());
    }
    if (policy.max_payload_bytes < 1 || policy.max_posts_per_member < 1 ||
        policy.max_reads_per_member < 1 || policy.max_chatter_rounds < 1 ||
        policy.max_unread_per_member < 1 || policy.max_group_tags_per_member < 1 ||
        policy.max_delivery_batch < 1) {
        throw Refusal("MALFORMED_WORKROOM_POLICY", "all Workroom bounds must be positive");
    }
    return policy;
}

void Core::enable_workroom(WorkroomPolicy policy) {
    workroom_policy_ = policy;
}

const WorkroomPolicy& Core::require_workroom() const {
    if (!workroom_policy_) {
        throw Refusal("WORKROOM_DISABLED", "Phase B Workroom policy is not mounted");
    }
    return *workroom_policy_;
}

void Core::require_live_room_mount(const Envelope& envelope) const {
    require_mount(envelope);
    const auto live = mount_live_.find(envelope.mount_id);
    if (live == mount_live_.end() || !live->second) {
        throw Refusal("MOUNT_REVOKED", "Workroom operation carries a revoked mount");
    }
    const auto epoch = current_job_epoch_.find(envelope.job_id);
    if (epoch == current_job_epoch_.end() || epoch->second != envelope.job_epoch) {
        throw Refusal("STALE_EPOCH", "Workroom operation carries a stale job_epoch");
    }
}

json Core::revoke_mount(std::uint64_t expected_version, const Envelope& envelope) {
    (void)require_workroom();
    require_mount(envelope);
    if (!mount_live_.at(envelope.mount_id)) {
        throw Refusal("MOUNT_REVOKED", "mount is already revoked");
    }
    return append_event({
        {"event_type", "MOUNT_REVOKED"},
        {"record_class", "AUTHORITY"},
        {"envelope", envelope_json(envelope)},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", envelope.mount_id}, {"mount_version", mount_version_.at(envelope.mount_id)}, {"live", true}},
        })},
        {"write_set", json::array({"mount:" + envelope.mount_id})},
    });
}

json Core::bump_job_epoch(std::uint64_t expected_version, const std::string& job_id) {
    (void)require_workroom();
    validate_room_token(job_id, "job_id");
    const auto found = current_job_epoch_.find(job_id);
    if (found == current_job_epoch_.end()) throw Refusal("UNKNOWN_JOB", "job has no committed epoch");
    return append_event({
        {"event_type", "JOB_EPOCH_BUMP"},
        {"record_class", "AUTHORITY"},
        {"job_id", job_id},
        {"prior_epoch", found->second},
        {"job_epoch", found->second + 1},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"job_id", job_id}, {"job_epoch", found->second}},
        })},
        {"write_set", json::array({"job_epoch:" + job_id})},
    });
}

json Core::prove_adapter(std::uint64_t expected_version, const std::string& adapter_id) {
    (void)require_workroom();
    const auto fixture = run_adapter_conformance_fixture(adapter_id);
    if (!fixture.at("passed").get<bool>()) {
        throw Refusal(
            "ADAPTER_CONFORMANCE_FAILED",
            "fixture delivered during an opaque call and is not trusted for mid-job delivery");
    }
    if (proven_adapters_.contains(adapter_id)) {
        throw Refusal("CONFLICT", "adapter proof already exists");
    }
    return append_event({
        {"event_type", "ADAPTER_PROVEN"},
        {"record_class", "AUTHORITY"},
        {"adapter_id", adapter_id},
        {"declared_delivery", "next_proven_safe_boundary"},
        {"declared_cancel", "queue_until_boundary_or_epoch_change"},
        {"fixture", "opaque_call_no_delivery_then_between_turns_exactly_once"},
        {"fixture_measurements", fixture},
        {"fixture_outcome", "PASS"},
        {"read_set", json::array({{{"state_version", expected_version}}})},
        {"write_set", json::array({"adapter_proof:" + adapter_id})},
    });
}

json Core::set_member_execution(
    std::uint64_t expected_version,
    const Envelope& envelope,
    const std::string& execution_state) {
    (void)require_workroom();
    require_live_room_mount(envelope);
    if (execution_state != "OPAQUE_CALL" && execution_state != "SAFE_BOUNDARY") {
        throw Refusal("MALFORMED_EXECUTION_STATE", "execution state must be OPAQUE_CALL or SAFE_BOUNDARY");
    }
    return append_event({
        {"event_type", "MEMBER_EXECUTION"},
        {"record_class", "AUTHORITY"},
        {"envelope", envelope_json(envelope)},
        {"execution_state", execution_state},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", envelope.mount_id}, {"mount_version", mount_version_.at(envelope.mount_id)}, {"live", true}},
            {{"job_id", envelope.job_id}, {"job_epoch", envelope.job_epoch}},
        })},
        {"write_set", json::array({"execution:" + envelope.mount_id})},
    });
}

json Core::set_group_tags(
    std::uint64_t expected_version,
    const Envelope& envelope,
    std::vector<std::string> group_tags) {
    const auto& policy = require_workroom();
    require_live_room_mount(envelope);
    group_tags = validate_tags(std::move(group_tags), policy.max_group_tags_per_member, "group_tags");
    return append_event({
        {"event_type", "GROUP_TAGS_SET"},
        {"record_class", "ROSTER"},
        {"envelope", envelope_json(envelope)},
        {"group_tags", group_tags},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", envelope.mount_id}, {"mount_version", mount_version_.at(envelope.mount_id)}, {"live", true}},
            {{"job_id", envelope.job_id}, {"job_epoch", envelope.job_epoch}},
        })},
        {"write_set", json::array({"member_tags:" + envelope.member_id})},
    });
}

json Core::workroom_post(
    std::uint64_t expected_version,
    const Envelope& envelope,
    RoomPost post) {
    const auto& policy = require_workroom();
    require_live_room_mount(envelope);
    if (kForbiddenRoomEventTypes.contains(post.event_type)) {
        throw Refusal("ROOM_EVENT_FORBIDDEN", "authority-mutating event type is absent from the Workroom enum");
    }
    if (!kRoomEventTypes.contains(post.event_type)) {
        throw Refusal("ROOM_EVENT_TYPE", "event_type is outside the exact six-value Workroom enum");
    }
    if (post.payload.empty() || static_cast<int>(post.payload.size()) > policy.max_payload_bytes) {
        throw Refusal("ROOM_PAYLOAD_CAP_EXCEEDED", "bounded_payload is empty or exceeds max_payload_bytes");
    }
    if (post.chatter_round < 1 || post.chatter_round > policy.max_chatter_rounds) {
        throw Refusal("ROOM_ROUND_CAP_EXCEEDED", "chatter_round exceeds max_chatter_rounds");
    }
    if (member_posts_[envelope.member_id] >= policy.max_posts_per_member) {
        throw Refusal("ROOM_POST_CAP_EXCEEDED", "member reached max_posts_per_member");
    }
    post.group_tags = validate_tags(
        std::move(post.group_tags), policy.max_group_tags_per_member, "post group_tags");
    const auto member_tags = member_group_tags_.contains(envelope.member_id)
        ? member_group_tags_.at(envelope.member_id) : std::vector<std::string>{};
    for (const auto& tag : post.group_tags) {
        if (!std::binary_search(member_tags.begin(), member_tags.end(), tag)) {
            throw Refusal("GROUP_MEMBERSHIP_REQUIRED", "sender is not tagged into target group: " + tag);
        }
    }
    if (post.reply_to) {
        const auto reply = std::find_if(room_records_.begin(), room_records_.end(), [&](const auto& record) {
            return record.room_seq == *post.reply_to;
        });
        if (reply == room_records_.end() || reply->thread_id != envelope.thread_id || reply->job_id != envelope.job_id) {
            throw Refusal("UNKNOWN_REPLY", "reply_to does not name a Workroom record in this thread/job");
        }
    }

    std::set<std::string> skipped_members;
    for (const auto& [mount_id, mounted] : mounts_) {
        if (!mount_live_.at(mount_id) || mounted.member_id == envelope.member_id ||
            mounted.thread_id != envelope.thread_id || mounted.job_id != envelope.job_id) {
            continue;
        }
        const auto receiver_tags = member_group_tags_.contains(mounted.member_id)
            ? member_group_tags_.at(mounted.member_id) : std::vector<std::string>{};
        if (!intersects(post.group_tags, receiver_tags)) continue;
        const auto watermark = member_watermark_[mounted.member_id];
        int unread = 0;
        for (const auto& record : room_records_) {
            if (record.room_seq > watermark && record.thread_id == envelope.thread_id &&
                record.job_id == envelope.job_id && record.job_epoch == mounted.job_epoch &&
                record.member_id != mounted.member_id &&
                intersects(record.group_tags, receiver_tags)) {
                ++unread;
            }
        }
        if (unread + 1 >= policy.max_unread_per_member) skipped_members.insert(mounted.member_id);
    }

    const auto payload_hash = sha256_text(post.payload);
    return append_event({
        {"event_type", "ROOM_POST"},
        {"record_class", "RECORD"},
        {"record", {
            {"room_seq", nullptr},
            {"thread_id", envelope.thread_id},
            {"job_id", envelope.job_id},
            {"task_id", envelope.task_id},
            {"run_id", envelope.run_id},
            {"member_id", envelope.member_id},
            {"mount_id", envelope.mount_id},
            {"job_epoch", envelope.job_epoch},
            {"timestamp", nullptr},
            {"event_type", post.event_type},
            {"reply_to", post.reply_to ? json(*post.reply_to) : json(nullptr)},
            {"bounded_payload", post.payload},
            {"payload_hash", payload_hash},
            {"group_tags", post.group_tags},
            {"chatter_round", post.chatter_round},
            {"commutative", false},
        }},
        {"skipped_members", skipped_members},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", envelope.mount_id}, {"mount_version", mount_version_.at(envelope.mount_id)}, {"live", true}},
            {{"job_id", envelope.job_id}, {"job_epoch", envelope.job_epoch}},
            {{"member_id", envelope.member_id}, {"post_count", member_posts_[envelope.member_id]}},
        })},
        {"write_set", json::array({"workroom_record", "member_posts:" + envelope.member_id})},
    }, true);
}

json Core::workroom_commutative_observation(
    const std::string& thread_id,
    const std::string& job_id,
    const std::string& task_id,
    const std::string& run_id,
    std::uint64_t job_epoch,
    const std::string& payload) {
    const auto& policy = require_workroom();
    validate_room_token(thread_id, "thread_id");
    validate_room_token(job_id, "job_id");
    validate_room_token(task_id, "task_id");
    validate_room_token(run_id, "run_id");
    if (job_epoch < 1) {
        throw Refusal("MALFORMED_ROOM_RECORD", "commutative job_epoch must be positive");
    }
    if (payload.empty() || static_cast<int>(payload.size()) > policy.max_payload_bytes) {
        throw Refusal("ROOM_PAYLOAD_CAP_EXCEEDED", "commutative payload is empty or over cap");
    }
    return append_event({
        {"event_type", "ROOM_POST"},
        {"record_class", "RECORD"},
        {"record", {
            {"room_seq", nullptr},
            {"thread_id", thread_id},
            {"job_id", job_id},
            {"task_id", task_id},
            {"run_id", run_id},
            {"member_id", "dispatch_system"},
            {"mount_id", "commutative_info"},
            {"job_epoch", job_epoch},
            {"timestamp", nullptr},
            {"event_type", "OBSERVATION"},
            {"reply_to", nullptr},
            {"bounded_payload", payload},
            {"payload_hash", sha256_text(payload)},
            {"group_tags", json::array()},
            {"chatter_round", 0},
            {"commutative", true},
        }},
        {"skipped_members", json::array()},
        {"read_set", json::array()},
        {"write_set", json::array({"commutative:workroom_record"})},
    }, true);
}

json Core::workroom_read(
    std::uint64_t expected_version,
    const Envelope& envelope,
    const std::string& adapter_id,
    const std::string& safe_boundary,
    int limit) {
    const auto& policy = require_workroom();
    require_live_room_mount(envelope);
    if (!proven_adapters_.contains(adapter_id)) {
        throw Refusal("ADAPTER_UNPROVEN", "adapter must pass its conformance fixture before delivery");
    }
    const auto execution = member_execution_.contains(envelope.mount_id)
        ? member_execution_.at(envelope.mount_id) : "OPAQUE_CALL";
    if (execution != "SAFE_BOUNDARY") {
        throw Refusal("NO_SAFE_BOUNDARY", "messages remain queued while the receiver is in an opaque call");
    }
    if (safe_boundary != "BETWEEN_TURNS" && safe_boundary != "BEFORE_TOOL" &&
        safe_boundary != "AFTER_TOOL") {
        throw Refusal("UNPROVEN_BOUNDARY", "safe_boundary is not in the adapter's proven set");
    }
    if (limit < 1 || limit > policy.max_delivery_batch) {
        throw Refusal("ROOM_READ_BATCH_CAP_EXCEEDED", "read limit exceeds max_delivery_batch");
    }
    if (member_reads_[envelope.member_id] >= policy.max_reads_per_member) {
        throw Refusal("ROOM_READ_CAP_EXCEEDED", "member reached max_reads_per_member");
    }

    const auto tags = member_group_tags_.contains(envelope.member_id)
        ? member_group_tags_.at(envelope.member_id) : std::vector<std::string>{};
    const auto from_watermark = member_watermark_[envelope.member_id];
    auto to_watermark = from_watermark;
    json delivered = json::array();
    for (const auto& record : room_records_) {
        if (record.room_seq <= from_watermark || record.thread_id != envelope.thread_id ||
            record.job_id != envelope.job_id) {
            continue;
        }
        to_watermark = record.room_seq;
        if (record.job_epoch != envelope.job_epoch || record.member_id == envelope.member_id ||
            !intersects(record.group_tags, tags)) continue;
        delivered.push_back({
            {"room_seq", record.room_seq},
            {"thread_id", record.thread_id},
            {"job_id", record.job_id},
            {"task_id", record.task_id},
            {"run_id", record.run_id},
            {"member_id", record.member_id},
            {"mount_id", record.mount_id},
            {"job_epoch", record.job_epoch},
            {"timestamp", record.timestamp},
            {"event_type", record.event_type},
            {"reply_to", record.reply_to ? json(*record.reply_to) : json(nullptr)},
            {"bounded_payload", record.bounded_payload},
            {"payload_hash", record.payload_hash},
            {"group_tags", record.group_tags},
        });
        if (static_cast<int>(delivered.size()) == limit) break;
    }

    return append_event({
        {"event_type", "ROOM_READ"},
        {"record_class", "ROSTER"},
        {"envelope", envelope_json(envelope)},
        {"adapter_id", adapter_id},
        {"safe_boundary", safe_boundary},
        {"watermark_from", from_watermark},
        {"watermark_to", to_watermark},
        {"delivered", delivered},
        {"durably_observed", true},
        {"seen", false},
        {"understood", false},
        {"read_count", member_reads_[envelope.member_id] + 1},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", envelope.mount_id}, {"mount_version", mount_version_.at(envelope.mount_id)}, {"live", true}},
            {{"job_id", envelope.job_id}, {"job_epoch", envelope.job_epoch}},
            {{"member_id", envelope.member_id}, {"watermark", from_watermark}},
        })},
        {"write_set", json::array({"watermark:" + envelope.member_id, "member_reads:" + envelope.member_id})},
    }, true);
}

json Core::workroom_status() const {
    const auto& policy = require_workroom();
    const auto current_room_seq = room_records_.empty() ? 0 : room_records_.back().room_seq;
    json members = json::array();
    std::map<std::string, int> groups;
    std::map<std::string, std::string> representative_mount;
    for (const auto& [mount_id, mounted] : mounts_) {
        const auto found = representative_mount.find(mounted.member_id);
        if (found == representative_mount.end() ||
            mount_version_.at(mount_id) > mount_version_.at(found->second)) {
            representative_mount[mounted.member_id] = mount_id;
        }
    }
    for (const auto& [member_id, mount_id] : representative_mount) {
        const auto& mounted = mounts_.at(mount_id);
        const auto tags = member_group_tags_.contains(mounted.member_id)
            ? member_group_tags_.at(mounted.member_id) : std::vector<std::string>{};
        const bool live = mount_live_.contains(mount_id) && mount_live_.at(mount_id) &&
            mounted.job_epoch == current_job_epoch_.at(mounted.job_id);
        if (live) {
            for (const auto& tag : tags) ++groups[tag];
        }
        const auto watermark = member_watermark_.contains(mounted.member_id)
            ? member_watermark_.at(mounted.member_id) : 0;
        int unread = 0;
        for (const auto& record : room_records_) {
            if (record.room_seq > watermark && record.thread_id == mounted.thread_id &&
                record.job_id == mounted.job_id &&
                record.job_epoch == current_job_epoch_.at(mounted.job_id) &&
                record.member_id != mounted.member_id &&
                intersects(record.group_tags, tags)) {
                ++unread;
            }
        }
        members.push_back({
            {"member_id", member_id},
            {"mount_id", mount_id},
            {"live", live},
            {"job_epoch", current_job_epoch_.at(mounted.job_id)},
            {"watermark", watermark},
            {"unread_count", unread},
            {"lag_visible", unread > 0},
            {"skipped_at_cap", member_skipped_.contains(mounted.member_id) && member_skipped_.at(mounted.member_id)},
            {"posts", member_posts_.contains(mounted.member_id) ? member_posts_.at(mounted.member_id) : 0},
            {"reads", member_reads_.contains(mounted.member_id) ? member_reads_.at(mounted.member_id) : 0},
            {"group_tags", tags},
            {"execution_state", member_execution_.contains(mount_id) ? member_execution_.at(mount_id) : "OPAQUE_CALL"},
        });
    }
    json derived_groups = json::object();
    for (const auto& [tag, count] : groups) {
        if (count > 0) derived_groups[tag] = count;
    }
    return {
        {"ok", true},
        {"state_version", version()},
        {"room_seq", current_room_seq},
        {"record_count", room_records_.size()},
        {"members", members},
        {"derived_groups", derived_groups},
        {"proven_adapters", proven_adapters_},
        {"bounds", {
            {"max_payload_bytes", policy.max_payload_bytes},
            {"max_posts_per_member", policy.max_posts_per_member},
            {"max_reads_per_member", policy.max_reads_per_member},
            {"max_chatter_rounds", policy.max_chatter_rounds},
            {"max_unread_per_member", policy.max_unread_per_member},
            {"max_group_tags_per_member", policy.max_group_tags_per_member},
            {"max_delivery_batch", policy.max_delivery_batch},
        }},
    };
}

json Core::gate_evidence_view() const {
    json evidence = json::array();
    for (const auto& event : events_) {
        if (event.at("event_type") != "RESULT") continue;
        evidence.push_back({
            {"seq", event.at("seq")},
            {"event_type", event.at("event_type")},
            {"receipt", event.at("receipt")},
        });
    }
    return {
        {"ok", true},
        {"type_filter", json::array({"RESULT"})},
        {"range_fold", false},
        {"evidence", evidence},
    };
}

bool Core::apply_workroom_event(const json& event) {
    const auto type = event.at("event_type").get<std::string>();
    if (type == "MOUNT_REVOKED") {
        const auto envelope = parse_envelope(event.at("envelope"));
        mount_live_[envelope.mount_id] = false;
        mount_version_[envelope.mount_id] = event.at("state_version").get<std::uint64_t>();
    } else if (type == "JOB_EPOCH_BUMP") {
        current_job_epoch_[event.at("job_id").get<std::string>()] = event.at("job_epoch").get<std::uint64_t>();
    } else if (type == "ADAPTER_PROVEN") {
        proven_adapters_.insert(event.at("adapter_id").get<std::string>());
    } else if (type == "MEMBER_EXECUTION") {
        const auto envelope = parse_envelope(event.at("envelope"));
        member_execution_[envelope.mount_id] = event.at("execution_state").get<std::string>();
    } else if (type == "GROUP_TAGS_SET") {
        const auto envelope = parse_envelope(event.at("envelope"));
        member_group_tags_[envelope.member_id] = event.at("group_tags").get<std::vector<std::string>>();
    } else if (type == "ROOM_POST") {
        const auto& record = event.at("record");
        StoredRoomRecord stored;
        stored.room_seq = record.at("room_seq").get<std::uint64_t>();
        stored.thread_id = record.at("thread_id").get<std::string>();
        stored.job_id = record.at("job_id").get<std::string>();
        stored.task_id = record.at("task_id").get<std::string>();
        stored.run_id = record.at("run_id").get<std::string>();
        stored.member_id = record.at("member_id").get<std::string>();
        stored.mount_id = record.at("mount_id").get<std::string>();
        stored.job_epoch = record.at("job_epoch").get<std::uint64_t>();
        stored.timestamp = record.at("timestamp").get<std::string>();
        stored.event_type = record.at("event_type").get<std::string>();
        if (!record.at("reply_to").is_null()) stored.reply_to = record.at("reply_to").get<std::uint64_t>();
        stored.bounded_payload = record.at("bounded_payload").get<std::string>();
        stored.payload_hash = record.at("payload_hash").get<std::string>();
        stored.group_tags = record.at("group_tags").get<std::vector<std::string>>();
        stored.chatter_round = record.at("chatter_round").get<int>();
        stored.commutative = record.at("commutative").get<bool>();
        room_records_.push_back(std::move(stored));
        if (!room_records_.back().commutative) ++member_posts_[room_records_.back().member_id];
        for (const auto& member : event.at("skipped_members")) {
            member_skipped_[member.get<std::string>()] = true;
        }
    } else if (type == "ROOM_READ") {
        const auto envelope = parse_envelope(event.at("envelope"));
        member_watermark_[envelope.member_id] = event.at("watermark_to").get<std::uint64_t>();
        ++member_reads_[envelope.member_id];
        member_skipped_[envelope.member_id] = false;
    } else {
        return false;
    }
    return true;
}

} // namespace dispatch_organ
