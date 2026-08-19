#include "dispatch_organ/core.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace dispatch_organ {
namespace {

const std::set<std::string> kOutcomes{
    "confirmed", "contradicted", "qualified", "unknown",
};
const std::set<std::string> kReviewOutcomes{
    "confirmed", "contradicted", "qualified",
};
const std::set<std::string> kReviewBases{
    "independent_verification", "mechanical_outcome", "explicit_adjudication",
};
const std::set<std::string> kFeedbackKinds{
    "USED", "CONTRADICTED", "IRRELEVANT", "UNOBSERVED",
};
const std::set<std::string> kMemoryModes{
    "normal", "clean_room", "no_memory_replay",
};
const std::set<std::string> kOmissionReasons{
    "context budget", "clean-room/naïve baseline",
    "stale/low maturity", "conflicting higher-priority", "no-memory replay",
};

void require_nonempty_text(
    const std::string& value,
    std::size_t max_bytes,
    std::string_view field) {
    if (value.empty() || value.size() > max_bytes) {
        throw Refusal(
            "MALFORMED_LEARNING",
            std::string(field) + " must contain 1.." + std::to_string(max_bytes) + " bytes");
    }
}

std::vector<std::string> strict_named_array(
    const json& value,
    std::string_view field,
    int maximum,
    bool require_nonempty) {
    if (!value.is_array()) {
        throw Refusal("MALFORMED_LEARNING", std::string(field) + " must be an array");
    }
    if (require_nonempty && value.empty()) {
        throw Refusal("MISSING_RETRIEVAL_KEYS", std::string(field) + " must name at least one future retrieval key");
    }
    if (value.size() > static_cast<std::size_t>(maximum)) {
        throw Refusal("LEDGER_CAP_EXCEEDED", std::string(field) + " exceeds its configured cap");
    }
    std::set<std::string> unique;
    for (const auto& item : value) {
        if (!item.is_string()) {
            throw Refusal("MALFORMED_LEARNING", std::string(field) + " entries must be strings");
        }
        const auto text = item.get<std::string>();
        require_nonempty_text(text, 512, field);
        if (!unique.insert(text).second) {
            throw Refusal("MALFORMED_LEARNING", std::string(field) + " contains a duplicate");
        }
    }
    return {unique.begin(), unique.end()};
}

bool is_subset(const std::vector<std::string>& required, const std::set<std::string>& supplied) {
    return std::all_of(required.begin(), required.end(), [&](const std::string& key) {
        return supplied.contains(key);
    });
}

std::string normalized_reason(std::string value) {
    if (value == "clean-room/naive baseline") return "clean-room/naïve baseline";
    return value;
}

} // namespace

LedgerPolicy parse_ledger_policy(const json& value) {
    require_exact_keys(value, {
        "contradiction_quarantine_threshold", "max_claim_bytes", "max_evidence_refs",
        "max_injection_bytes", "max_injection_tokens", "max_learnings",
        "max_limits_bytes", "max_relevance_keys", "min_verified_confidence",
    }, {}, "ledger policy");
    LedgerPolicy policy;
    try {
        policy.max_learnings = value.at("max_learnings").get<int>();
        policy.max_relevance_keys = value.at("max_relevance_keys").get<int>();
        policy.max_evidence_refs = value.at("max_evidence_refs").get<int>();
        policy.max_claim_bytes = value.at("max_claim_bytes").get<int>();
        policy.max_limits_bytes = value.at("max_limits_bytes").get<int>();
        policy.max_injection_bytes = value.at("max_injection_bytes").get<int>();
        policy.max_injection_tokens = value.at("max_injection_tokens").get<int>();
        policy.contradiction_quarantine_threshold =
            value.at("contradiction_quarantine_threshold").get<int>();
        policy.min_verified_confidence = value.at("min_verified_confidence").get<double>();
    } catch (const std::exception& error) {
        throw Refusal("MALFORMED_LEDGER_POLICY", error.what());
    }
    if (policy.max_learnings < 1 || policy.max_relevance_keys < 1 ||
        policy.max_evidence_refs < 1 || policy.max_claim_bytes < 1 ||
        policy.max_limits_bytes < 1 || policy.max_injection_bytes < 1 ||
        policy.max_injection_tokens < 1 || policy.contradiction_quarantine_threshold < 1 ||
        !std::isfinite(policy.min_verified_confidence) ||
        policy.min_verified_confidence < 0.0 || policy.min_verified_confidence > 1.0) {
        throw Refusal("MALFORMED_LEDGER_POLICY", "all caps must be positive and confidence must be in [0,1]");
    }
    return policy;
}

const LedgerPolicy& Core::require_ledger() const {
    if (!ledger_policy_) {
        throw Refusal("LEDGER_DISABLED", "Experience Ledger policy is not mounted");
    }
    return *ledger_policy_;
}

void Core::enable_ledger(LedgerPolicy policy) {
    if (!workroom_enabled()) {
        throw Refusal("PHASE_DEPENDENCY", "Phase C requires the accepted Phase B Workroom policy");
    }
    if (ledger_policy_) throw Refusal("CONFLICT", "Experience Ledger policy is already mounted");
    ledger_policy_ = policy;
    if (learnings_.size() > static_cast<std::size_t>(policy.max_learnings)) {
        throw Refusal("LEDGER_CAP_EXCEEDED", "replayed learning count exceeds mounted policy");
    }
}

json Core::ledger_create_candidate(
    std::uint64_t expected_version,
    const Envelope& author,
    const json& candidate) {
    const auto& policy = require_ledger();
    require_live_room_mount(author);
    require_exact_keys(candidate, {
        "claim", "confidence", "evidence_refs", "injection_budget", "learning_id",
        "limits", "outcome", "question_key", "relevance_keys", "review_after", "role_scope",
    }, {"origin_room_seq"}, "learning candidate");
    if (learnings_.size() >= static_cast<std::size_t>(policy.max_learnings)) {
        throw Refusal("LEDGER_CAP_EXCEEDED", "maximum learning count reached");
    }

    json learning;
    try {
        learning = {
            {"learning_id", candidate.at("learning_id").get<std::string>()},
            {"question_key", candidate.at("question_key").get<std::string>()},
            {"role_scope", candidate.at("role_scope").get<std::string>()},
            {"relevance_keys", strict_named_array(
                candidate.at("relevance_keys"), "relevance_keys", policy.max_relevance_keys, true)},
            {"claim", candidate.at("claim").get<std::string>()},
            {"limits", candidate.at("limits").get<std::string>()},
            {"evidence_refs", strict_named_array(
                candidate.at("evidence_refs"), "evidence_refs", policy.max_evidence_refs, true)},
            {"outcome", candidate.at("outcome").get<std::string>()},
            {"maturity", "candidate"},
            {"confidence", candidate.at("confidence").get<double>()},
            {"created_seq", 0},
            {"review_after", candidate.at("review_after").get<std::uint64_t>()},
            {"injection_budget", candidate.at("injection_budget").get<int>()},
            {"origin", {
                {"thread_id", author.thread_id}, {"job_id", author.job_id},
                {"task_id", author.task_id}, {"run_id", author.run_id},
                {"member_id", author.member_id}, {"mount_id", author.mount_id},
                {"job_epoch", author.job_epoch},
            }},
        };
    } catch (const Refusal&) {
        throw;
    } catch (const std::exception& error) {
        throw Refusal("MALFORMED_LEARNING", error.what());
    }
    require_nonempty_text(learning.at("learning_id").get_ref<const std::string&>(), 128, "learning_id");
    require_nonempty_text(learning.at("question_key").get_ref<const std::string&>(), 256, "question_key");
    require_nonempty_text(learning.at("role_scope").get_ref<const std::string&>(), 64, "role_scope");
    require_nonempty_text(learning.at("claim").get_ref<const std::string&>(),
        static_cast<std::size_t>(policy.max_claim_bytes), "claim");
    require_nonempty_text(learning.at("limits").get_ref<const std::string&>(),
        static_cast<std::size_t>(policy.max_limits_bytes), "limits");
    const auto outcome = learning.at("outcome").get<std::string>();
    if (!kOutcomes.contains(outcome)) throw Refusal("MALFORMED_LEARNING", "unknown learning outcome");
    const auto confidence = learning.at("confidence").get<double>();
    if (!std::isfinite(confidence) || confidence < 0.0 || confidence > 1.0) {
        throw Refusal("MALFORMED_LEARNING", "confidence must be finite and in [0,1]");
    }
    const int injection_budget = learning.at("injection_budget").get<int>();
    if (injection_budget < 1 || injection_budget > policy.max_injection_bytes) {
        throw Refusal("MALFORMED_LEARNING", "injection_budget must fit the mounted byte cap");
    }
    const auto id = learning.at("learning_id").get<std::string>();
    if (learnings_.contains(id)) throw Refusal("CONFLICT", "learning_id already exists");

    json room_precondition;
    if (candidate.contains("origin_room_seq")) {
        if (!candidate.at("origin_room_seq").is_number_unsigned()) {
            throw Refusal("MALFORMED_LEARNING", "origin_room_seq must be unsigned");
        }
        const auto room_seq = candidate.at("origin_room_seq").get<std::uint64_t>();
        const auto found = std::find_if(room_records_.begin(), room_records_.end(), [&](const StoredRoomRecord& record) {
            return record.room_seq == room_seq;
        });
        if (found == room_records_.end()) throw Refusal("UNKNOWN_ROOM_ORIGIN", "origin room record does not exist");
        if (found->thread_id != author.thread_id || found->job_id != author.job_id ||
            found->run_id != author.run_id || found->member_id != author.member_id ||
            found->mount_id != author.mount_id || found->job_epoch != author.job_epoch) {
            throw Refusal("ROOM_ORIGIN_MISMATCH", "room origin does not belong to the candidate author lattice");
        }
        learning["origin_room_seq"] = room_seq;
        room_precondition = {{"origin_room_seq", room_seq}};
    }

    json reads = json::array({
        {{"state_version", expected_version}},
        {{"mount_id", author.mount_id}, {"live", true}},
        {{"job_id", author.job_id}, {"job_epoch", author.job_epoch}},
        {{"learning_id_free", id}},
    });
    if (!room_precondition.is_null()) reads.push_back(room_precondition);
    return append_event({
        {"event_type", "LEARNING_CANDIDATE"},
        {"record_class", "LEDGER"},
        {"envelope", envelope_json(author)},
        {"learning", learning},
        {"read_set", reads},
        {"write_set", json::array({"ledger:learning:" + id})},
    });
}

json Core::ledger_review(
    std::uint64_t expected_version,
    const Envelope& reviewer,
    const std::string& learning_id,
    const std::string& outcome,
    const std::string& basis,
    const std::string& conditions,
    std::vector<std::string> evidence_refs) {
    const auto& policy = require_ledger();
    require_live_room_mount(reviewer);
    if (reviewer.epistemic_role != "verifier") {
        throw Refusal("VERIFIER_REQUIRED", "learning promotion requires a verifier mount");
    }
    const auto found = learnings_.find(learning_id);
    if (found == learnings_.end()) throw Refusal("UNKNOWN_LEARNING", "learning_id does not exist");
    const auto& origin = found->second.object.at("origin");
    if (origin.at("member_id") == reviewer.member_id && origin.at("run_id") == reviewer.run_id) {
        throw Refusal("SELF_PROMOTION", "an author cannot verify or promote its own candidate in the same run");
    }
    if (!kReviewOutcomes.contains(outcome)) throw Refusal("MALFORMED_REVIEW", "review outcome cannot promote");
    if (!kReviewBases.contains(basis)) throw Refusal("MALFORMED_REVIEW", "promotion basis is not declared");
    if (basis == "independent_verification" && origin.at("member_id") == reviewer.member_id) {
        throw Refusal("INDEPENDENCE_REQUIRED", "independent verification requires a different member");
    }
    require_nonempty_text(conditions, static_cast<std::size_t>(policy.max_limits_bytes), "conditions");
    json evidence = strict_named_array(
        evidence_refs, "evidence_refs", policy.max_evidence_refs, true);
    const std::string resulting_maturity = outcome == "contradicted" ? "quarantined" : "verified";
    return append_event({
        {"event_type", "LEARNING_REVIEW"},
        {"record_class", "LEDGER"},
        {"envelope", envelope_json(reviewer)},
        {"learning_id", learning_id},
        {"review", {
            {"basis", basis}, {"conditions", conditions}, {"evidence_refs", evidence},
            {"outcome", outcome}, {"resulting_maturity", resulting_maturity},
        }},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", reviewer.mount_id}, {"live", true}},
            {{"job_id", reviewer.job_id}, {"job_epoch", reviewer.job_epoch}},
            {{"learning_id", learning_id}, {"created_seq", found->second.object.at("created_seq")}},
        })},
        {"write_set", json::array({"ledger:status:" + learning_id})},
    });
}

json Core::ledger_reconcile(
    std::uint64_t expected_version,
    const Envelope& verifier,
    std::vector<std::string> learning_ids,
    const std::string& conditions,
    std::vector<std::string> evidence_refs) {
    const auto& policy = require_ledger();
    require_live_room_mount(verifier);
    if (verifier.epistemic_role != "verifier") {
        throw Refusal("VERIFIER_REQUIRED", "contradiction reconciliation requires a verifier mount");
    }
    std::sort(learning_ids.begin(), learning_ids.end());
    learning_ids.erase(std::unique(learning_ids.begin(), learning_ids.end()), learning_ids.end());
    if (learning_ids.size() < 2) {
        throw Refusal("MALFORMED_RECONCILIATION", "reconciliation must link at least two distinct claims");
    }
    for (const auto& id : learning_ids) {
        if (!learnings_.contains(id)) throw Refusal("UNKNOWN_LEARNING", "reconciliation learning_id does not exist");
    }
    require_nonempty_text(conditions, static_cast<std::size_t>(policy.max_limits_bytes), "conditions");
    json evidence = strict_named_array(
        evidence_refs, "evidence_refs", policy.max_evidence_refs, true);
    const auto reconciliation_id =
        "reconcile:" + verifier.run_id + ":" + std::to_string(expected_version);
    return append_event({
        {"event_type", "LEARNING_RECONCILIATION"},
        {"record_class", "LEDGER"},
        {"envelope", envelope_json(verifier)},
        {"reconciliation", {
            {"reconciliation_id", reconciliation_id}, {"learning_ids", learning_ids},
            {"conditions", conditions}, {"evidence_refs", evidence},
            {"preserves_originals", true},
        }},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", verifier.mount_id}, {"live", true}},
            {{"job_id", verifier.job_id}, {"job_epoch", verifier.job_epoch}},
            {{"learning_ids", learning_ids}},
        })},
        {"write_set", json::array({"ledger:reconciliation:" + reconciliation_id})},
    });
}

json Core::ledger_prepare_injection(
    std::uint64_t expected_version,
    const Envelope& envelope,
    const std::string& question_key,
    std::vector<std::string> relevance_keys,
    const std::string& memory_mode,
    int requested_max_bytes,
    std::vector<std::string> omit_learning_ids,
    const json& declared_omissions) {
    const auto& policy = require_ledger();
    require_live_room_mount(envelope);
    const auto& attempt = require_attempt(envelope);
    if (attempt.state != "CAST") {
        throw Refusal("INVALID_ATTEMPT_STATE", "learning injection requires a freshly cast dispatch attempt");
    }
    require_nonempty_text(question_key, 256, "question_key");
    json query_keys_json = relevance_keys;
    relevance_keys = strict_named_array(
        query_keys_json, "relevance_keys", policy.max_relevance_keys, true);
    if (!kMemoryModes.contains(memory_mode)) throw Refusal("MALFORMED_INJECTION", "unknown memory_mode");
    if (requested_max_bytes < 1 || requested_max_bytes > policy.max_injection_bytes) {
        throw Refusal("INJECTION_CAP_EXCEEDED", "requested injection byte cap exceeds mounted policy");
    }

    std::sort(omit_learning_ids.begin(), omit_learning_ids.end());
    if (std::adjacent_find(omit_learning_ids.begin(), omit_learning_ids.end()) != omit_learning_ids.end()) {
        throw Refusal("MALFORMED_INJECTION", "omit_learning_ids contains a duplicate");
    }
    std::map<std::string, std::string> omission_reasons;
    if (!declared_omissions.is_array()) {
        throw Refusal("MALFORMED_INJECTION", "declared_omissions must be an array");
    }
    for (const auto& omission : declared_omissions) {
        require_exact_keys(omission, {"learning_id", "named_reason"}, {}, "declared omission");
        if (!omission.at("learning_id").is_string() || !omission.at("named_reason").is_string()) {
            throw Refusal("MALFORMED_INJECTION", "declared omission fields must be strings");
        }
        const auto id = omission.at("learning_id").get<std::string>();
        const auto reason = normalized_reason(omission.at("named_reason").get<std::string>());
        if (!kOmissionReasons.contains(reason)) {
            throw Refusal("UNNAMED_OMISSION_REASON", "omission reason is outside the sealed list");
        }
        if (!omission_reasons.emplace(id, reason).second) {
            throw Refusal("MALFORMED_INJECTION", "learning has duplicate omission receipts");
        }
    }
    for (const auto& [id, reason] : omission_reasons) {
        (void)reason;
        if (!std::binary_search(omit_learning_ids.begin(), omit_learning_ids.end(), id)) {
            throw Refusal("MALFORMED_INJECTION", "declared omission does not correspond to an omitted learning");
        }
    }

    const std::set<std::string> query_keys(relevance_keys.begin(), relevance_keys.end());
    struct Considered {
        std::string id;
        const StoredLearning* learning;
        bool eligible;
        std::string ineligible_reason;
    };
    std::vector<Considered> considered;
    for (const auto& [id, stored] : learnings_) {
        const auto& object = stored.object;
        if (object.at("question_key") != question_key) continue;
        const auto role_scope = object.at("role_scope").get<std::string>();
        if (role_scope != "*" && role_scope != envelope.epistemic_role) continue;
        if (!is_subset(object.at("relevance_keys").get<std::vector<std::string>>(), query_keys)) continue;
        bool eligible = true;
        std::string reason;
        const auto confidence = object.at("confidence").get<double>();
        const auto review_after = object.at("review_after").get<std::uint64_t>();
        if (stored.maturity != "verified" ||
            (stored.outcome != "confirmed" && stored.outcome != "qualified") ||
            confidence < policy.min_verified_confidence ||
            (review_after != 0 && version() >= review_after)) {
            eligible = false;
            reason = "stale/low maturity";
        }
        considered.push_back({id, &stored, eligible, reason});
    }
    std::sort(considered.begin(), considered.end(), [](const Considered& left, const Considered& right) {
        const auto left_confidence = left.learning->object.at("confidence").get<double>();
        const auto right_confidence = right.learning->object.at("confidence").get<double>();
        if (left_confidence != right_confidence) return left_confidence > right_confidence;
        const auto left_seq = left.learning->object.at("created_seq").get<std::uint64_t>();
        const auto right_seq = right.learning->object.at("created_seq").get<std::uint64_t>();
        return std::tie(left_seq, left.id) < std::tie(right_seq, right.id);
    });

    std::set<std::string> eligible_ids;
    for (const auto& item : considered) if (item.eligible) eligible_ids.insert(item.id);
    for (const auto& id : omit_learning_ids) {
        if (!eligible_ids.contains(id)) {
            throw Refusal("INVALID_OMISSION", "only an eligible learning may be deliberately omitted");
        }
        if (memory_mode == "normal" && !omission_reasons.contains(id)) {
            throw Refusal("SILENT_OMISSION", "eligible learning omission lacks a named receipted reason");
        }
    }

    json selected = json::array();
    json packet_items = json::array();
    json rejected = json::array();
    json considered_ids = json::array();
    for (const auto& item : considered) {
        considered_ids.push_back(item.id);
        if (!item.eligible) {
            rejected.push_back({{"learning_id", item.id}, {"named_reason", item.ineligible_reason}});
            continue;
        }
        if (memory_mode == "clean_room") {
            rejected.push_back({{"learning_id", item.id}, {"named_reason", "clean-room/naïve baseline"}});
            continue;
        }
        if (memory_mode == "no_memory_replay") {
            rejected.push_back({{"learning_id", item.id}, {"named_reason", "no-memory replay"}});
            continue;
        }
        if (std::binary_search(omit_learning_ids.begin(), omit_learning_ids.end(), item.id)) {
            rejected.push_back({{"learning_id", item.id}, {"named_reason", omission_reasons.at(item.id)}});
            continue;
        }

        const auto& learning = item.learning->object;
        json packet_item{
            {"learning_id", item.id}, {"question_key", learning.at("question_key")},
            {"relevance_keys", learning.at("relevance_keys")}, {"claim", learning.at("claim")},
            {"limits", learning.at("limits")}, {"confidence", learning.at("confidence")},
            {"evidence_refs", learning.at("evidence_refs")},
        };
        auto proposed_items = packet_items;
        proposed_items.push_back(packet_item);
        const auto proposed_packet = json{
            {"experience_ledger", proposed_items}, {"schema", "dispatch-organ.phase-c/1"},
        }.dump();
        // The core does not own a model tokenizer. One UTF-8 byte per budget
        // token is a deterministic upper bound for byte-fallback tokenizers;
        // live receipts additionally carry the backend's measured token use.
        const auto proposed_tokens = static_cast<int>(proposed_packet.size());
        const auto per_learning_budget = learning.at("injection_budget").get<int>();
        if (static_cast<int>(packet_item.dump().size()) > per_learning_budget ||
            static_cast<int>(proposed_packet.size()) > requested_max_bytes ||
            proposed_tokens > policy.max_injection_tokens) {
            rejected.push_back({{"learning_id", item.id}, {"named_reason", "context budget"}});
            continue;
        }
        packet_items = std::move(proposed_items);
        selected.push_back(item.id);
    }

    std::string exact_packet;
    if (!packet_items.empty()) {
        exact_packet = json{
            {"experience_ledger", packet_items}, {"schema", "dispatch-organ.phase-c/1"},
        }.dump();
    }
    if (selected.size() + rejected.size() != considered_ids.size()) {
        throw Refusal("SILENT_OMISSION", "selector produced an unreceipted omission");
    }
    const auto injected_tokens = static_cast<int>(exact_packet.size());
    const json receipt{
        {"considered", considered_ids}, {"selected", selected}, {"rejected", rejected},
        {"injected_bytes", exact_packet.size()}, {"injected_tokens", injected_tokens},
        {"token_accounting", "conservative_utf8_byte_upper_bound"},
        {"exact_packet_hash", sha256_text(exact_packet)}, {"exact_packet", exact_packet},
        {"question_key", question_key}, {"relevance_keys", relevance_keys},
        {"memory_mode", memory_mode},
        {"dispatch_identity", {
            {"thread_id", envelope.thread_id}, {"job_id", envelope.job_id},
            {"task_id", envelope.task_id}, {"run_id", envelope.run_id},
            {"member_id", envelope.member_id}, {"mount_id", envelope.mount_id},
            {"job_epoch", envelope.job_epoch},
        }},
    };
    return append_event({
        {"event_type", "LEARNING_INJECTION"},
        {"record_class", "LEDGER"},
        {"envelope", envelope_json(envelope)},
        {"receipt", receipt},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", envelope.mount_id}, {"live", true}},
            {{"job_id", envelope.job_id}, {"job_epoch", envelope.job_epoch}},
            {{"learning_versions", considered_ids}},
        })},
        {"write_set", json::array({"ledger:injection:" + envelope.run_id})},
    });
}

json Core::ledger_feedback(
    std::uint64_t expected_version,
    const Envelope& observer,
    const std::string& learning_id,
    const std::string& feedback,
    const std::string& evidence_ref) {
    const auto& policy = require_ledger();
    require_live_room_mount(observer);
    const auto found = learnings_.find(learning_id);
    if (found == learnings_.end()) throw Refusal("UNKNOWN_LEARNING", "learning_id does not exist");
    if (!kFeedbackKinds.contains(feedback)) throw Refusal("MALFORMED_FEEDBACK", "unknown feedback value");
    require_nonempty_text(evidence_ref, 512, "evidence_ref");
    const int contradicted_count = found->second.contradicted_count + (feedback == "CONTRADICTED" ? 1 : 0);
    const std::string resulting_maturity =
        contradicted_count >= policy.contradiction_quarantine_threshold
            ? "quarantined" : found->second.maturity;
    return append_event({
        {"event_type", "LEARNING_FEEDBACK"},
        {"record_class", "LEDGER"},
        {"envelope", envelope_json(observer)},
        {"learning_id", learning_id},
        {"feedback", feedback},
        {"evidence_ref", evidence_ref},
        {"resulting_maturity", resulting_maturity},
        {"contradicted_count", contradicted_count},
        {"read_set", json::array({
            {{"state_version", expected_version}},
            {{"mount_id", observer.mount_id}, {"live", true}},
            {{"job_id", observer.job_id}, {"job_epoch", observer.job_epoch}},
            {{"learning_id", learning_id}, {"maturity", found->second.maturity}},
        })},
        {"write_set", json::array({"ledger:feedback:" + learning_id})},
    });
}

json Core::ledger_status() const {
    const auto& policy = require_ledger();
    json learnings = json::array();
    for (const auto& [id, stored] : learnings_) {
        auto item = stored.object;
        item["current_outcome"] = stored.outcome;
        item["current_maturity"] = stored.maturity;
        item["feedback_counts"] = {
            {"USED", stored.used_count}, {"CONTRADICTED", stored.contradicted_count},
            {"IRRELEVANT", stored.irrelevant_count}, {"UNOBSERVED", stored.unobserved_count},
        };
        item["zero_use"] = stored.used_count == 0;
        item["stale_for_review"] = item.at("review_after").get<std::uint64_t>() != 0 &&
            version() >= item.at("review_after").get<std::uint64_t>();
        item["last_feedback_seq"] = stored.last_feedback_seq;
        learnings.push_back(std::move(item));
        (void)id;
    }
    return {
        {"ok", true}, {"state_version", version()}, {"learnings", learnings},
        {"reconciliations", ledger_reconciliations_},
        {"injection_receipts", injection_receipts_},
        {"review_surface", {
            {"zero_use_count", std::count_if(learnings_.begin(), learnings_.end(),
                [](const auto& pair) { return pair.second.used_count == 0; })},
            {"stale_count", std::count_if(learnings_.begin(), learnings_.end(), [&](const auto& pair) {
                const auto after = pair.second.object.at("review_after").template get<std::uint64_t>();
                return after != 0 && version() >= after;
            })},
        }},
        {"bounds", {
            {"max_learnings", policy.max_learnings}, {"max_relevance_keys", policy.max_relevance_keys},
            {"max_evidence_refs", policy.max_evidence_refs}, {"max_claim_bytes", policy.max_claim_bytes},
            {"max_limits_bytes", policy.max_limits_bytes}, {"max_injection_bytes", policy.max_injection_bytes},
            {"max_injection_tokens", policy.max_injection_tokens},
            {"contradiction_quarantine_threshold", policy.contradiction_quarantine_threshold},
            {"min_verified_confidence", policy.min_verified_confidence},
        }},
    };
}

[[noreturn]] void Core::refuse_protected_namespace_write(
    const std::string& learning_id,
    const std::string& protected_namespace) const {
    (void)require_ledger();
    (void)learning_id;
    (void)protected_namespace;
    throw Refusal(
        "PROTECTED_NAMESPACE_FORBIDDEN",
        "machine Experience entries cannot write into protected namespaces or be represented as operator-owned memory");
}

bool Core::apply_ledger_event(const json& event) {
    const auto type = event.at("event_type").get<std::string>();
    if (type == "LEARNING_CANDIDATE") {
        const auto learning = event.at("learning");
        const auto id = learning.at("learning_id").get<std::string>();
        StoredLearning stored;
        stored.object = learning;
        stored.outcome = learning.at("outcome").get<std::string>();
        stored.maturity = learning.at("maturity").get<std::string>();
        if (!learnings_.emplace(id, std::move(stored)).second) {
            throw std::runtime_error("duplicate learning_id in Dispatch Journal");
        }
    } else if (type == "LEARNING_REVIEW") {
        auto& stored = learnings_.at(event.at("learning_id").get<std::string>());
        stored.outcome = event.at("review").at("outcome").get<std::string>();
        stored.maturity = event.at("review").at("resulting_maturity").get<std::string>();
    } else if (type == "LEARNING_RECONCILIATION") {
        auto reconciliation = event.at("reconciliation");
        reconciliation["created_seq"] = event.at("seq");
        ledger_reconciliations_.push_back(std::move(reconciliation));
    } else if (type == "LEARNING_INJECTION") {
        auto receipt = event.at("receipt");
        receipt["journal_seq"] = event.at("seq");
        injection_receipts_.push_back(std::move(receipt));
    } else if (type == "LEARNING_FEEDBACK") {
        auto& stored = learnings_.at(event.at("learning_id").get<std::string>());
        const auto feedback = event.at("feedback").get<std::string>();
        if (feedback == "USED") ++stored.used_count;
        else if (feedback == "CONTRADICTED") ++stored.contradicted_count;
        else if (feedback == "IRRELEVANT") ++stored.irrelevant_count;
        else if (feedback == "UNOBSERVED") ++stored.unobserved_count;
        stored.maturity = event.at("resulting_maturity").get<std::string>();
        stored.last_feedback_seq = event.at("seq").get<std::uint64_t>();
    } else {
        return false;
    }
    return true;
}

} // namespace dispatch_organ
