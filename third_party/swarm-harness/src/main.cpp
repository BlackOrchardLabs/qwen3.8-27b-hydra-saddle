#include "swarm_harness/core.hpp"

#include <curl/curl.h>

#include <filesystem>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

namespace {

void usage() {
    std::cerr
        << "usage: swarm-harness <scripted|director|mcp> "
        << "--config CONFIG.json --job JOB.json --output DECLARED_OUTPUT_DIR\n";
}

std::map<std::string, std::string> parse_options(int argc, char** argv, int start) {
    std::map<std::string, std::string> options;
    for (int index = start; index < argc; index += 2) {
        if (index + 1 >= argc) {
            throw std::runtime_error("option is missing a value: " + std::string(argv[index]));
        }
        const std::string key = argv[index];
        if (key != "--config" && key != "--job" && key != "--output") {
            throw std::runtime_error("unknown option: " + key);
        }
        options[key] = argv[index + 1];
    }
    for (const auto* required : {"--config", "--job", "--output"}) {
        if (!options.contains(required)) {
            throw std::runtime_error("missing required option: " + std::string(required));
        }
    }
    return options;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        usage();
        return 1;
    }
    try {
        const std::string mode = argv[1];
        if (mode != "scripted" && mode != "director" && mode != "mcp") {
            usage();
            throw std::runtime_error("unknown mode: " + mode);
        }
        const auto options = parse_options(argc, argv, 2);
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("curl_global_init failed");
        }
        int result = 0;
        {
            auto config = swarm::load_config(options.at("--config"));
            auto job = swarm::load_job(options.at("--job"));
            swarm::Harness harness(std::move(config), std::move(job), options.at("--output"));
            if (mode == "scripted") result = swarm::run_scripted(harness);
            if (mode == "director") result = swarm::run_director(harness);
            if (mode == "mcp") result = swarm::run_mcp(harness);
        }
        curl_global_cleanup();
        return result;
    } catch (const std::exception& error) {
        std::cerr << "swarm-harness: " << error.what() << '\n';
        curl_global_cleanup();
        return 1;
    }
}

