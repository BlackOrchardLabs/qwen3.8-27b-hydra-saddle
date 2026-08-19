# Hydra Saddle for Qwen 3.8

Your local Qwen forgets everything. Give it a memory it can trust.

Run Qwen 3.8-27B on your own machine and every agent you spin up is an amnesiac, forgetting the second the session closes. Yesterday's answer? Gone. It will re-solve the same problem and re-read the same files while burning more tokens chasing rabbits down holes.

Hydra Saddle remembers what worked for your local Qwen. A fresh worker inherits that knowledge, while wrong, irrelevant, bloated work gets blocked at the door instead of poisoning the next run.

No fine-tuning. No bigger model. No giant context window. Same Qwen weights, it just stops starting over.

A cold Qwen worker scores 0/12 on a task it's never cracked. Hand it the 435 bytes an earlier worker verified, and it's 12/12. Experience that outlives the agent that earned it.

**Cold: 0/12. Inherited: 12/12. Without inheritance: cannot. With inheritance: can.**

![Hydra Saddle: a six-headed purple hydra wearing a leather saddle harness with a Qwen badge, next to the pitch: durable verified memory that outlives the agent, wrong work gets blocked, same Qwen weights, proven 0/12 to 12/12 with 435 bytes. Verify, protect, inherit, evolve.](assets/hydra-saddle-card.png)

Powered by the Dispatch Organ.

## Install and run

You need Linux, git, a C++20 compiler, CMake 3.20+, Python 3, and a Qwen model you can serve locally. A GPU makes it faster.

**1. Get Hydra Saddle**
```bash
git clone https://github.com/BlackOrchardLabs/qwen3.8-27b-hydra-saddle.git
cd qwen3.8-27b-hydra-saddle
```

**2. Install the build tools**

Debian/Ubuntu:
```bash
sudo apt-get install build-essential cmake python3 libcurl4-openssl-dev
```

**3. Build**
```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```
We verified this from a bare checkout: no sibling repo, no manual dependency hunt.

**4. Serve a Qwen model**

Hydra Saddle talks to Qwen through a local OpenAI-compatible endpoint. The simplest route is `llama-server`:
```bash
llama-server \
  -m /path/to/your-qwen.gguf \
  -ngl 99 \
  -c 8192 \
  --host 127.0.0.1 \
  --port 8778
```
Wait for the server to report that it is listening. `-ngl 99` puts as much of the model as possible on the GPU. Lower it if needed.

We verified Hydra Saddle end-to-end with Qwen2.5-Coder-32B on stock llama.cpp.

To reproduce the published Hydra Saddle numbers, use the exact Qwen3.8-27B reference model named in [REPRODUCE.md](REPRODUCE.md). That model needs the llama.cpp build and GPU support described there. You do **not** need it just to try Hydra Saddle.

**5. Run the first demo**

In another terminal, from the repo:
```bash
./scripts/demo_frontier.py \
  --endpoint http://127.0.0.1:8778/v1/chat/completions \
  --model /path/to/your-qwen.gguf
```
Hydra Saddle runs against your model and writes fresh evidence under `receipts/`. If you are not using the exact reference model, it will still run; the published reference comparison simply does not apply.

The other demos are:
```bash
./scripts/demo_rediscovery.py --endpoint http://127.0.0.1:8778/v1/chat/completions --model /path/to/your-qwen.gguf
./scripts/demo_poison.py --endpoint http://127.0.0.1:8778/v1/chat/completions --model /path/to/your-qwen.gguf
```

## Three demos

Each command builds Hydra Saddle and its vendored harness, runs two fixtures,
and writes fresh evidence under `receipts/runs/`. Existing evidence is never
overwritten.

### 1. Rediscovery: first-pass + approximately 86% leaner

A 435-byte discovered residue lets a fresh stateless worker succeed first-pass
on a task it cannot solve cold. The matched control can still succeed after
rereading the full reference archive. This is a first-pass and context-efficiency
result, not an impossibility claim.

### 2. Capability frontier: without inheritance cannot, with inheritance can

The matched cold controls receive the identical prompt minus only 435 bytes of
verified residue and remain `0/12`; inherited workers are `12/12`. The residue
carries a discovered **result**, not a reusable skill or reasoning method. Runs
use temperature zero; the reproduction strength comes from two distinct
fixtures, not independent sampling across repetitions.

### 3. Poison: more with good, not-less with bad, self-healing when bypassed

The poison experiment uses a calibrated `0.50` baseline with headroom in both
directions. Good experience measures `1.00`. Irrelevant, false, and oversized
experience is refused, not promoted, or bounded and remains `0.50`; deliberately
bypassed false experience measures `0.25`, then contradiction feedback
quarantines it and the next dispatch recovers to `0.50`.

## Architecture

Hydra Saddle combines a stateless worker harness with the Dispatch Organ, a C++
authority core. The core records one hash-chained journal across dispatch,
bounded workroom records, verified experience, injection decisions, feedback,
and recovery. Models produce work; operator-owned gates and capabilities decide
what can change durable state.

The accepted core behavior is covered by deterministic process-boundary tests:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j2
ctest --test-dir build --output-on-failure
```

## Receipts

Do not believe the summary; run the demos. Every run emits exact prompts,
responses, model identity, matched-pair measurements, protection decisions,
hash-chained journals, and a `summary.json` containing receipt hashes. See
`receipts/README.md` for the layout.

No GitHub Actions workflows, webhooks, telemetry, or network dependency fetches
are included.

## License

Hydra Saddle is licensed under the Apache License 2.0. See `LICENSE` and
`NOTICE`. The vendored nlohmann/json dependency remains under its MIT license;
see `third_party/nlohmann/LICENSE.MIT`. The vendored swarm-harness source is
Apache-2.0; see `third_party/swarm-harness/LICENSE`, `NOTICE`, and
`PROVENANCE.md`.

Black Orchard Labs
