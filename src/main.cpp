#include "dispatch_organ/core.hpp"
#include "dispatch_organ/harness_client.hpp"

#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace {

using dispatch_organ::Core;
using dispatch_organ::Envelope;
using dispatch_organ::HarnessClient;
using dispatch_organ::HarnessLaunch;
using dispatch_organ::Refusal;
using dispatch_organ::json;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("cannot read file: " + path.string());
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

std::map<std::string, std::string> parse_options(int argc, char** argv) {
    const std::set<std::string> allowed{
        "--harness-bin", "--harness-config", "--harness-job", "--harness-output",
        "--journal", "--ledger-policy", "--policy", "--workroom-policy",
    };
    std::map<std::string, std::string> options;
    for (int index = 1; index < argc; index += 2) {
        if (index + 1 >= argc) throw std::runtime_error("option is missing a value");
        const std::string key = argv[index];
        if (!allowed.contains(key)) throw std::runtime_error("unknown option: " + key);
        if (!options.emplace(key, argv[index + 1]).second) {
            throw std::runtime_error("duplicate option: " + key);
        }
    }
    for (const auto* required : {"--journal", "--policy"}) {
        if (!options.contains(required)) throw std::runtime_error("missing required option: " + std::string(required));
    }
    const int harness_count = static_cast<int>(options.contains("--harness-bin")) +
        static_cast<int>(options.contains("--harness-config")) +
        static_cast<int>(options.contains("--harness-job")) +
        static_cast<int>(options.contains("--harness-output"));
    if (harness_count != 0 && harness_count != 4) {
        throw std::runtime_error("all four harness mount options are required together");
    }
    if (options.contains("--ledger-policy") && !options.contains("--workroom-policy")) {
        throw std::runtime_error("--ledger-policy requires --workroom-policy");
    }
    return options;
}

std::uint64_t expected_version(const json& request) {
    try {
        return request.at("expected_version").get<std::uint64_t>();
    } catch (const std::exception& error) {
        throw Refusal("MALFORMED_REQUEST", std::string("expected_version: ") + error.what());
    }
}

Envelope request_envelope(const json& request) {
    return dispatch_organ::parse_envelope(request.at("envelope"));
}

std::vector<std::string> request_string_array(const json& value, std::string_view field) {
    if (!value.is_array()) throw Refusal("MALFORMED_REQUEST", std::string(field) + " must be an array");
    std::vector<std::string> output;
    for (const auto& item : value) {
        if (!item.is_string()) throw Refusal("MALFORMED_REQUEST", std::string(field) + " entries must be strings");
        output.push_back(item.get<std::string>());
    }
    return output;
}

json event_response(const json& event) {
    return {
        {"ok", true},
        {"outcome", "COMMITTED"},
        {"state_version", event.at("state_version")},
        {"receipt", event},
    };
}

std::string harness_role(const std::string& role) {
    if (role == "builder") return "Builder";
    if (role == "scout") return "Scout";
    if (role == "adversary") return "Adversary";
    if (role == "verifier") return "Verifier/Integrator";
    throw Refusal("UNSUPPORTED_ROLE", "role has no swarm-harness mapping");
}

void deterministic_fault(std::string_view point) {
    const char* configured = std::getenv("DISPATCH_ORGAN_FAULT_POINT");
    if (configured != nullptr && point == configured) {
        _exit(86);
    }
}

json handle_request(Core& core, HarnessClient* harness, const json& request) {
    if (!request.is_object() || !request.contains("op") || !request.at("op").is_string()) {
        throw Refusal("MALFORMED_REQUEST", "request requires string op");
    }
    const auto op = request.at("op").get<std::string>();
    const std::set<std::string> phase_b_operations{
        "bump_job_epoch", "gate_evidence_view", "prove_adapter", "revoke_mount",
        "set_group_tags", "set_member_execution", "workroom_commutative_observation",
        "workroom_post", "workroom_read", "workroom_status",
    };
    if (phase_b_operations.contains(op) && !core.workroom_enabled()) {
        throw Refusal("UNKNOWN_OPERATION", "operation is outside the Phase A surface: " + op);
    }
    const std::set<std::string> phase_c_operations{
        "ledger_create_candidate", "ledger_feedback", "ledger_injection",
        "ledger_protected_namespace_write", "ledger_reconcile", "ledger_review", "ledger_status",
    };
    if (phase_c_operations.contains(op) && !core.ledger_enabled()) {
        throw Refusal("UNKNOWN_OPERATION", "operation is outside the Phase A+B surface: " + op);
    }
    if (op == "status") {
        dispatch_organ::require_exact_keys(request, {"op"}, {}, "status request");
        return core.status();
    }
    if (op == "mount") {
        dispatch_organ::require_exact_keys(request, {"envelope", "expected_version", "op"}, {}, "mount request");
        return event_response(core.mount(expected_version(request), request_envelope(request)));
    }
    if (op == "cast") {
        dispatch_organ::require_exact_keys(
            request, {"envelope", "expected_version", "op", "prompt", "round_index"}, {}, "cast request");
        if (!request.at("prompt").is_string() || !request.at("round_index").is_number_integer()) {
            throw Refusal("MALFORMED_REQUEST", "cast prompt/round_index have wrong types");
        }
        return event_response(core.cast(
            expected_version(request), request_envelope(request),
            request.at("prompt").get<std::string>(), request.at("round_index").get<int>()));
    }
    if (op == "tools") {
        dispatch_organ::require_exact_keys(request, {"envelope", "op"}, {}, "tools request");
        return core.tools(request_envelope(request));
    }
    if (op == "authorize_tool") {
        dispatch_organ::require_exact_keys(request, {"envelope", "op", "tool"}, {}, "authorize_tool request");
        if (!request.at("tool").is_string() || request.at("tool").get_ref<const std::string&>().empty()) {
            throw Refusal("MALFORMED_REQUEST", "tool must be a non-empty string");
        }
        return core.authorize_tool(request_envelope(request), request.at("tool").get<std::string>());
    }
    if (op == "finish_job") {
        dispatch_organ::require_exact_keys(request, {"envelope", "op"}, {}, "finish_job request");
        (void)core.tools(request_envelope(request));
        throw Refusal("FORBIDDEN_TOOL", "finish_job is absent from every Phase A BOT mount and cannot manufacture DONE");
    }
    if (op == "dispatch") {
        if (core.ledger_enabled()) {
            dispatch_organ::require_exact_keys(request, {
                "declared_omissions", "envelope", "expected_version", "memory_mode",
                "omit_learning_ids", "op", "question_key", "relevance_keys",
                "requested_max_bytes",
            }, {
                "artifact_contract_id", "artifact_name", "harness_seat",
                "input_artifact_ids", "input_result_task_ids",
            }, "Phase C dispatch request");
        } else {
            dispatch_organ::require_exact_keys(request, {"envelope", "expected_version", "op"}, {}, "dispatch request");
        }
        if (harness == nullptr) throw Refusal("ENGINE_UNMOUNTED", "swarm-harness MCP subprocess is not mounted");
        const auto envelope = request_envelope(request);
        std::uint64_t dispatch_version = expected_version(request);
        json injection_event;
        std::string dispatch_prompt = core.prompt_for(envelope);
        if (core.ledger_enabled()) {
            if (!request.at("question_key").is_string() || !request.at("memory_mode").is_string() ||
                !request.at("requested_max_bytes").is_number_integer()) {
                throw Refusal("MALFORMED_REQUEST", "Phase C dispatch selection field has wrong type");
            }
            injection_event = core.ledger_prepare_injection(
                dispatch_version, envelope,
                request.at("question_key").get<std::string>(),
                request_string_array(request.at("relevance_keys"), "relevance_keys"),
                request.at("memory_mode").get<std::string>(),
                request.at("requested_max_bytes").get<int>(),
                request_string_array(request.at("omit_learning_ids"), "omit_learning_ids"),
                request.at("declared_omissions"));
            dispatch_version = injection_event.at("state_version").get<std::uint64_t>();
            const auto packet = injection_event.at("receipt").at("exact_packet").get<std::string>();
            if (!packet.empty()) {
                dispatch_prompt =
                    "[EXPERIENCE_LEDGER_PACKET sha256=" +
                    injection_event.at("receipt").at("exact_packet_hash").get<std::string>() +
                    "]\n" + packet + "\n[/EXPERIENCE_LEDGER_PACKET]\n\n" + dispatch_prompt;
            }
        }
        const auto reservation = core.begin_dispatch(dispatch_version, envelope);
        deterministic_fault("after_dispatch_intent_before_call");
        try {
            json harness_arguments{
                {"job_id", envelope.job_id},
                {"task_id", envelope.task_id},
                {"role", harness_role(envelope.epistemic_role)},
                {"seat", request.value("harness_seat", "swarm")},
                {"prompt", dispatch_prompt},
                {"input_artifact_ids", request.contains("input_artifact_ids")
                    ? request.at("input_artifact_ids") : json::array()},
                {"input_result_task_ids", request.contains("input_result_task_ids")
                    ? request.at("input_result_task_ids") : json::array()},
            };
            if (!harness_arguments.at("seat").is_string() ||
                harness_arguments.at("seat").get_ref<const std::string&>().empty()) {
                throw Refusal("MALFORMED_REQUEST", "harness_seat must be a non-empty string");
            }
            (void)request_string_array(harness_arguments.at("input_artifact_ids"), "input_artifact_ids");
            (void)request_string_array(harness_arguments.at("input_result_task_ids"), "input_result_task_ids");
            if (request.contains("artifact_name") != request.contains("artifact_contract_id")) {
                throw Refusal("MALFORMED_REQUEST", "artifact_name and artifact_contract_id must be supplied together");
            }
            if (request.contains("artifact_name")) {
                if (!request.at("artifact_name").is_string() || !request.at("artifact_contract_id").is_string() ||
                    request.at("artifact_name").get_ref<const std::string&>().empty() ||
                    request.at("artifact_contract_id").get_ref<const std::string&>().empty()) {
                    throw Refusal("MALFORMED_REQUEST", "artifact fields must be non-empty strings");
                }
                harness_arguments["artifact_name"] = request.at("artifact_name");
                harness_arguments["artifact_contract_id"] = request.at("artifact_contract_id");
            }
            const auto harness_receipt = harness->call_tool("dispatch_task", harness_arguments);
            deterministic_fault("after_dispatch_call_before_ack");
            auto response = event_response(core.acknowledge_dispatch(
                reservation.state_version, envelope, reservation.call_id, harness_receipt));
            if (core.ledger_enabled()) response["injection_receipt"] = injection_event.at("receipt");
            return response;
        } catch (const std::exception& error) {
            const auto failure = core.fail_call(
                reservation.state_version, envelope, reservation.call_id, error.what());
            throw Refusal("EXTERNAL_CALL_FAILED", failure.dump());
        }
    }
    if (op == "harness_gate") {
        if (!core.ledger_enabled()) {
            throw Refusal("UNKNOWN_OPERATION", "operation is outside the Phase A+B surface: harness_gate");
        }
        dispatch_organ::require_exact_keys(request, {
            "envelope", "expected_version", "gate_name", "op", "target_task_id",
        }, {}, "harness_gate request");
        if (harness == nullptr) throw Refusal("ENGINE_UNMOUNTED", "swarm-harness MCP subprocess is not mounted");
        if (!request.at("target_task_id").is_string() || !request.at("gate_name").is_string()) {
            throw Refusal("MALFORMED_REQUEST", "gate target and name must be strings");
        }
        const auto envelope = request_envelope(request);
        const auto target_task_id = request.at("target_task_id").get<std::string>();
        const auto gate_name = request.at("gate_name").get<std::string>();
        const auto reservation = core.begin_gate(
            expected_version(request), envelope, target_task_id, gate_name);
        deterministic_fault("after_gate_intent_before_call");
        try {
            const auto harness_receipt = harness->call_tool("run_gate", {
                {"job_id", envelope.job_id},
                {"task_id", target_task_id},
                {"gate_name", gate_name},
            });
            deterministic_fault("after_gate_call_before_ack");
            return event_response(core.complete_gate(
                reservation.state_version, envelope, reservation.call_id, harness_receipt));
        } catch (const std::exception& error) {
            const auto failure = core.fail_gate(
                reservation.state_version, envelope, reservation.call_id, error.what());
            throw Refusal("EXTERNAL_CALL_FAILED", failure.dump());
        }
    }
    if (op == "result") {
        dispatch_organ::require_exact_keys(request, {"envelope", "expected_version", "op"}, {}, "result request");
        if (harness == nullptr) throw Refusal("ENGINE_UNMOUNTED", "swarm-harness MCP subprocess is not mounted");
        const auto envelope = request_envelope(request);
        const auto reservation = core.begin_result(expected_version(request), envelope);
        deterministic_fault("after_result_intent_before_call");
        try {
            const auto harness_result = harness->call_tool("get_result", {
                {"job_id", envelope.job_id},
                {"task_id", envelope.task_id},
                {"wait_ms", core.policy().call_timeout_ms},
            });
            deterministic_fault("after_result_call_before_ack");
            return event_response(core.complete_result(
                reservation.state_version, envelope, reservation.call_id, harness_result));
        } catch (const std::exception& error) {
            const auto failure = core.fail_call(
                reservation.state_version, envelope, reservation.call_id, error.what());
            throw Refusal("EXTERNAL_CALL_FAILED", failure.dump());
        }
    }
    if (op == "dispose_late") {
        dispatch_organ::require_exact_keys(
            request, {"disposition", "envelope", "expected_version", "op"}, {}, "dispose_late request");
        if (!request.at("disposition").is_string()) {
            throw Refusal("MALFORMED_REQUEST", "disposition must be a string");
        }
        return event_response(core.dispose_late(
            expected_version(request), request_envelope(request), request.at("disposition").get<std::string>()));
    }
    if (op == "revoke_mount") {
        dispatch_organ::require_exact_keys(request, {"envelope", "expected_version", "op"}, {}, "revoke_mount request");
        return event_response(core.revoke_mount(expected_version(request), request_envelope(request)));
    }
    if (op == "bump_job_epoch") {
        dispatch_organ::require_exact_keys(request, {"expected_version", "job_id", "op"}, {}, "bump_job_epoch request");
        if (!request.at("job_id").is_string()) throw Refusal("MALFORMED_REQUEST", "job_id must be a string");
        return event_response(core.bump_job_epoch(expected_version(request), request.at("job_id").get<std::string>()));
    }
    if (op == "prove_adapter") {
        dispatch_organ::require_exact_keys(request, {"adapter_id", "expected_version", "op"}, {}, "prove_adapter request");
        if (!request.at("adapter_id").is_string()) throw Refusal("MALFORMED_REQUEST", "adapter_id must be a string");
        return event_response(core.prove_adapter(expected_version(request), request.at("adapter_id").get<std::string>()));
    }
    if (op == "set_member_execution") {
        dispatch_organ::require_exact_keys(
            request, {"envelope", "execution_state", "expected_version", "op"}, {}, "set_member_execution request");
        if (!request.at("execution_state").is_string()) {
            throw Refusal("MALFORMED_REQUEST", "execution_state must be a string");
        }
        return event_response(core.set_member_execution(
            expected_version(request), request_envelope(request), request.at("execution_state").get<std::string>()));
    }
    if (op == "set_group_tags") {
        dispatch_organ::require_exact_keys(
            request, {"envelope", "expected_version", "group_tags", "op"}, {}, "set_group_tags request");
        return event_response(core.set_group_tags(
            expected_version(request), request_envelope(request),
            request_string_array(request.at("group_tags"), "group_tags")));
    }
    if (op == "workroom_post") {
        dispatch_organ::require_exact_keys(request, {
            "bounded_payload", "chatter_round", "envelope", "event_type", "expected_version",
            "group_tags", "op", "reply_to",
        }, {}, "workroom_post request");
        if (!request.at("event_type").is_string() || !request.at("bounded_payload").is_string() ||
            !request.at("chatter_round").is_number_integer()) {
            throw Refusal("MALFORMED_REQUEST", "Workroom post field has wrong type");
        }
        std::optional<std::uint64_t> reply_to;
        if (!request.at("reply_to").is_null()) {
            if (!request.at("reply_to").is_number_unsigned()) {
                throw Refusal("MALFORMED_REQUEST", "reply_to must be null or an unsigned room_seq");
            }
            reply_to = request.at("reply_to").get<std::uint64_t>();
        }
        const auto event = core.workroom_post(
            expected_version(request), request_envelope(request), {
                request.at("event_type").get<std::string>(),
                reply_to,
                request.at("bounded_payload").get<std::string>(),
                request_string_array(request.at("group_tags"), "group_tags"),
                request.at("chatter_round").get<int>(),
            });
        deterministic_fault("after_room_append_before_response");
        return event_response(event);
    }
    if (op == "workroom_commutative_observation") {
        dispatch_organ::require_exact_keys(request, {
            "bounded_payload", "job_epoch", "job_id", "op", "run_id", "task_id", "thread_id",
        }, {}, "workroom_commutative_observation request");
        for (const auto* key : {"bounded_payload", "job_id", "run_id", "task_id", "thread_id"}) {
            if (!request.at(key).is_string()) throw Refusal("MALFORMED_REQUEST", std::string(key) + " must be a string");
        }
        if (!request.at("job_epoch").is_number_unsigned()) {
            throw Refusal("MALFORMED_REQUEST", "job_epoch must be an unsigned integer");
        }
        return event_response(core.workroom_commutative_observation(
            request.at("thread_id").get<std::string>(), request.at("job_id").get<std::string>(),
            request.at("task_id").get<std::string>(), request.at("run_id").get<std::string>(),
            request.at("job_epoch").get<std::uint64_t>(),
            request.at("bounded_payload").get<std::string>()));
    }
    if (op == "workroom_read") {
        dispatch_organ::require_exact_keys(request, {
            "adapter_id", "envelope", "expected_version", "limit", "op", "safe_boundary",
        }, {}, "workroom_read request");
        if (!request.at("adapter_id").is_string() || !request.at("safe_boundary").is_string() ||
            !request.at("limit").is_number_integer()) {
            throw Refusal("MALFORMED_REQUEST", "Workroom read field has wrong type");
        }
        return event_response(core.workroom_read(
            expected_version(request), request_envelope(request),
            request.at("adapter_id").get<std::string>(), request.at("safe_boundary").get<std::string>(),
            request.at("limit").get<int>()));
    }
    if (op == "workroom_status") {
        dispatch_organ::require_exact_keys(request, {"op"}, {}, "workroom_status request");
        return core.workroom_status();
    }
    if (op == "gate_evidence_view") {
        dispatch_organ::require_exact_keys(request, {"op"}, {}, "gate_evidence_view request");
        return core.gate_evidence_view();
    }
    if (op == "ledger_create_candidate") {
        dispatch_organ::require_exact_keys(
            request, {"candidate", "envelope", "expected_version", "op"}, {},
            "ledger_create_candidate request");
        const auto event = core.ledger_create_candidate(
            expected_version(request), request_envelope(request), request.at("candidate"));
        deterministic_fault("after_ledger_append_before_response");
        return event_response(event);
    }
    if (op == "ledger_review") {
        dispatch_organ::require_exact_keys(request, {
            "basis", "conditions", "envelope", "evidence_refs", "expected_version",
            "learning_id", "op", "outcome",
        }, {}, "ledger_review request");
        for (const auto* key : {"basis", "conditions", "learning_id", "outcome"}) {
            if (!request.at(key).is_string()) {
                throw Refusal("MALFORMED_REQUEST", std::string(key) + " must be a string");
            }
        }
        return event_response(core.ledger_review(
            expected_version(request), request_envelope(request),
            request.at("learning_id").get<std::string>(), request.at("outcome").get<std::string>(),
            request.at("basis").get<std::string>(), request.at("conditions").get<std::string>(),
            request_string_array(request.at("evidence_refs"), "evidence_refs")));
    }
    if (op == "ledger_reconcile") {
        dispatch_organ::require_exact_keys(request, {
            "conditions", "envelope", "evidence_refs", "expected_version",
            "learning_ids", "op",
        }, {}, "ledger_reconcile request");
        if (!request.at("conditions").is_string()) {
            throw Refusal("MALFORMED_REQUEST", "conditions must be a string");
        }
        return event_response(core.ledger_reconcile(
            expected_version(request), request_envelope(request),
            request_string_array(request.at("learning_ids"), "learning_ids"),
            request.at("conditions").get<std::string>(),
            request_string_array(request.at("evidence_refs"), "evidence_refs")));
    }
    if (op == "ledger_injection") {
        dispatch_organ::require_exact_keys(request, {
            "declared_omissions", "envelope", "expected_version", "memory_mode",
            "omit_learning_ids", "op", "question_key", "relevance_keys",
            "requested_max_bytes",
        }, {}, "ledger_injection request");
        if (!request.at("question_key").is_string() || !request.at("memory_mode").is_string() ||
            !request.at("requested_max_bytes").is_number_integer()) {
            throw Refusal("MALFORMED_REQUEST", "ledger injection field has wrong type");
        }
        return event_response(core.ledger_prepare_injection(
            expected_version(request), request_envelope(request),
            request.at("question_key").get<std::string>(),
            request_string_array(request.at("relevance_keys"), "relevance_keys"),
            request.at("memory_mode").get<std::string>(),
            request.at("requested_max_bytes").get<int>(),
            request_string_array(request.at("omit_learning_ids"), "omit_learning_ids"),
            request.at("declared_omissions")));
    }
    if (op == "ledger_feedback") {
        dispatch_organ::require_exact_keys(request, {
            "envelope", "evidence_ref", "expected_version", "feedback", "learning_id", "op",
        }, {}, "ledger_feedback request");
        for (const auto* key : {"evidence_ref", "feedback", "learning_id"}) {
            if (!request.at(key).is_string()) {
                throw Refusal("MALFORMED_REQUEST", std::string(key) + " must be a string");
            }
        }
        const auto event = core.ledger_feedback(
            expected_version(request), request_envelope(request),
            request.at("learning_id").get<std::string>(), request.at("feedback").get<std::string>(),
            request.at("evidence_ref").get<std::string>());
        deterministic_fault("after_ledger_feedback_before_response");
        return event_response(event);
    }
    if (op == "ledger_status") {
        dispatch_organ::require_exact_keys(request, {"op"}, {}, "ledger_status request");
        return core.ledger_status();
    }
    if (op == "ledger_protected_namespace_write") {
        dispatch_organ::require_exact_keys(
            request, {"learning_id", "op", "protected_namespace"}, {}, "ledger_protected_namespace_write request");
        if (!request.at("learning_id").is_string() || !request.at("protected_namespace").is_string()) {
            throw Refusal("MALFORMED_REQUEST", "protected-namespace write fields must be strings");
        }
        core.refuse_protected_namespace_write(
            request.at("learning_id").get<std::string>(), request.at("protected_namespace").get<std::string>());
    }
    throw Refusal("UNKNOWN_OPERATION", "operation is outside the current authorized surface: " + op);
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse_options(argc, argv);
        const auto policy_value = dispatch_organ::parse_strict_json(read_file(options.at("--policy")));
        Core core(dispatch_organ::parse_policy(policy_value), options.at("--journal"));
        if (options.contains("--workroom-policy")) {
            core.enable_workroom(dispatch_organ::parse_workroom_policy(
                dispatch_organ::parse_strict_json(read_file(options.at("--workroom-policy")))));
        }
        if (options.contains("--ledger-policy")) {
            core.enable_ledger(dispatch_organ::parse_ledger_policy(
                dispatch_organ::parse_strict_json(read_file(options.at("--ledger-policy")))));
        }

        std::unique_ptr<HarnessClient> harness;
        if (options.contains("--harness-bin")) {
            harness = std::make_unique<HarnessClient>(HarnessLaunch{
                options.at("--harness-bin"),
                options.at("--harness-config"),
                options.at("--harness-job"),
                options.at("--harness-output"),
                core.policy().call_timeout_ms,
            });
        }

        std::string line;
        while (std::getline(std::cin, line)) {
            if (line.empty()) continue;
            try {
                const auto request = dispatch_organ::parse_strict_json(line);
                std::cout << handle_request(core, harness.get(), request).dump() << '\n' << std::flush;
            } catch (const Refusal& refusal) {
                std::cout << json{
                    {"ok", false},
                    {"outcome", "REFUSED"},
                    {"code", refusal.code()},
                    {"error", refusal.what()},
                    {"state_version", core.version()},
                }.dump() << '\n' << std::flush;
            } catch (const std::exception& error) {
                std::cout << json{
                    {"ok", false},
                    {"outcome", "REFUSED"},
                    {"code", "INTERNAL_ERROR"},
                    {"error", error.what()},
                    {"state_version", core.version()},
                }.dump() << '\n' << std::flush;
            }
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "dispatch-organ: " << error.what() << '\n';
        return 1;
    }
}
