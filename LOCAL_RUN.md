# Local Terminal run sheet

Open with `open -a TextEdit LOCAL_RUN.md` or `less LOCAL_RUN.md` (`q` exits). Dependency pins and signing requirements: [full setup guide](docs/LSE_QUICKSTART.md). Projects: [mac_amdgpu](https://github.com/lemonade-sdk/mac-amdgpu), [Lemon Seed Engine](https://github.com/Geramy/LSE).

**Current boundary:** Qwen 27B Q6 CLI generation and repeated HTTP completion/chat requests pass through LSE/Loom/HRX with GPU execution required and clean shutdown. All five prompt IDs and 33 generated IDs from the France fixture exactly match independent MLX on the same checkpoint. The optimized Q6 kernels reached median 10.12 decode tokens/s with flush64/poll64; automatic batching reached 10.13 with poll64 and 6.97 with the default poll1000. These are short runs with KV128 and MTP disabled; longer contexts and broad accuracy remain unqualified. [Exact conditions and evidence](docs/LSE_PERFORMANCE.md).

## Build locally

```sh
cd /Users/geramyloveless/Documents/Development/mac_amdgpu
cmake -S hsa -B build/hsa -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/hsa --parallel 4
bash scripts/build-hrx-macos.sh
bash scripts/build-lse-macos.sh
```

Requires sources/fixtures from the setup guide. The LSE script builds `lse` and `lse-server` and runs offline checks; it does not reinstall the driver.

For a driver change, stop GPU workloads and use **Stop GPU** before replacing the extension. Build and sign using your configured team/identity:

```sh
export XCODE_TEAM_ID=YOURTEAMID
export SIGN_IDENTITY='Apple Development: Your Name (IDENTITY)'
bash scripts/build.sh --release
BUILT=$(xcodebuild -project MacAMDGPU.xcodeproj -scheme MacAMDGPUHost \
  -configuration Release -showBuildSettings 2>/dev/null \
  | sed -n 's/^ *BUILT_PRODUCTS_DIR = //p' | head -1)
bash scripts/sign-development.sh "$BUILT/MacAMDGPUHost.app"
open -R "$BUILT/MacAMDGPUHost.app"
```

Replace `/Applications/MacAMDGPUHost.app` with that signed app in Finder, then launch it:

```sh
open /Applications/MacAMDGPUHost.app
```

Approve driver replacement, verify the installed version, and initialize the GPU. [Signing/approval requirements](README.md#apple-developer-portal-setup).

## Monitor and one-token qualification

In a separate Terminal:

```sh
cd /Users/geramyloveless/Documents/Development/mac_amdgpu
cmake -S amdgpu_mtop -B build/amdgpu_mtop -DCMAKE_BUILD_TYPE=Release
cmake --build build/amdgpu_mtop --parallel 4
build/amdgpu_mtop/amdgpu_mtop
```

Press **h** for fast/slow refresh, **n/p** to switch devices, **q** to quit. Sensor sampling remains slower than the screen refresh.

Back in the project Terminal, preflight the installed LM Studio checkpoint (no GPU submission), then explicitly request the bounded one-token attempt:

```sh
python3 scripts/run-lse-qwen-smoke.py
python3 scripts/run-lse-qwen-smoke.py --run --timeout-seconds 1200
```

Default model: `~/.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit`; override with `--model PATH`. The runner uses HRX/Loom, strict GPU execution, KV128, no MTP, greedy generation and statistics. One token is the default; add `--tokens 16` to exercise decoding. Run one GPU workload at a time.

## Experimental resident benchmark

Eligible macOS gfx1201 single-device decode automatically measures intervals
16/64/256/0; other work retains the 16-dispatch baseline. Blocked polling defaults
to **1000 µs**. The measured 64/64 settings below are explicit overrides,
using an isolated HSA build. They do not require reinstalling the driver.
Build LSE/HRX as above first, then configure the experiment and run its CPU-only
signal checks:

```sh
cmake -S hsa -B build/hsa-wait-perf -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build build/hsa-wait-perf --target hsa-runtime64 hsa-signal-test mac-hsa-info --parallel 4
ctest --test-dir build/hsa-wait-perf -R '^hsa-cpu-signals(-short-poll)?$' --output-on-failure
```

With other GPU workloads stopped, explicitly run one warmup and three measured
requests against the resident model:

```sh
env -u LSE_TIME_STEPS -u LSE_PROFILE_DISPATCH -u MAC_HSA_SIGNAL_BACKEND \
  LSE_TIME_SPANS=1 LSE_FLUSH_INTERVAL=64 MAC_HSA_BLOCKED_POLL_US=64 \
  python3 scripts/run-lse-server-smoke.py --run \
  --hsa-library-dir build/hsa-wait-perf \
  --benchmark-repeats 3 --generated-tokens 33 --kv-len 128 \
  --log-dir build/tests/qwen-resident-experiment-64-64
build/hsa-wait-perf/mac-hsa-info
```

The runner uses the raw five-token France prompt and records binary/runtime
hashes, settings, each response and median rates. Require all 33 generated tokens
(32 subsequent decode steps), identical output, normal server exit and driver
stage 0 afterward. Median 10.118556 decode tokens/s was measured with the
optimized Q6 kernels. Results depend on the recorded build and context.
To exercise automatic batching, omit `LSE_FLUSH_INTERVAL`; `LSE_AUTO_BATCH=0`
disables it. Explicit flush overrides always take precedence. None of these five-token prefill measurements is a PP512 benchmark.

To repeat the polling control, keep the same binaries and flush interval, change
`MAC_HSA_BLOCKED_POLL_US` to `1000`, and use a distinct log directory. To measure
the fixed baseline, use `LSE_FLUSH_INTERVAL=16` and
`MAC_HSA_BLOCKED_POLL_US=1000`. Command-scoped overrides above do not persist in
the shell or change the interactive server defaults below.

## HTTP chat

For interactive terminal chat, start the resident server from this project root:

```sh
DYLD_LIBRARY_PATH="$PWD/build/hsa-wait-perf" \
LSE_REQUIRE_DEVICE_KERNELS=1 LSE_FLUSH_INTERVAL=64 MAC_HSA_BLOCKED_POLL_US=64 \
build/lse-macos-adapter/lse-server \
  --model "$HOME/.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit" \
  --pool hrx:0 --dialect loom --no-mtp --kv-len 2048 --max-tokens 256 \
  --host 127.0.0.1 --port 8080 --served-name local-qwen
```

Once it is listening, in a second Terminal at this project root:

```sh
python3 tools/lse_chat.py
```

The client streams text and prints the server's PP/s and decode TPS after each
response. `/clear` resets conversation history and `/quit` exits the client.
The server keeps the model loaded across requests; stop it with Ctrl-C and wait
for shutdown when finished. The first response may need kernel compilation.
This larger-context interactive configuration is separate from the KV128
performance fixture. The client preserves conversation history, so use `/clear`
before accumulated turns exhaust the context. `LSE_API_KEY` supplies a bearer
token if the server was launched with `--api-key`.

From the project root, start the qualified short-context server:

```sh
export DYLD_LIBRARY_PATH="$PWD/build/hsa"
export LSE_REQUIRE_DEVICE_KERNELS=1
MODEL="$HOME/.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit"
build/lse-macos-adapter/lse-server --model "$MODEL" \
  --pool hrx:0 --dialect loom --no-mtp --kv-len 128 \
  --host 127.0.0.1 --port 8080 --served-name local-qwen --max-tokens 64 --shutdown-grace-seconds 30
```

Wait for model loading and the listening message. In another Terminal:

```sh
curl -sS http://127.0.0.1:8080/health
curl -sS http://127.0.0.1:8080/v1/models
curl -sS http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"local-qwen","messages":[{"role":"system","content":"Answer briefly."},{"role":"user","content":"What is the capital of Germany?"}],"temperature":0,"max_tokens":64,"stream":false}' \
  | python3 -m json.tool
```

`/health` checks HTTP availability. Responses expose **PP/s** in `timings.prompt_per_second` and **decode TPS** in `timings.decode_per_second`, plus counts/durations. Decode timing excludes the first token produced by prefill. CLI `--stats` reports prefill time and decode tokens/s. Keep the prompt plus generated tokens within the 128-token context; this checkpoint may emit a thinking section before its answer. Initial JIT/model loading affect latency. Ctrl-C requests shutdown; wait for workloads to exit before replacing the driver.

To rerun the five-request regression instead of starting an interactive server:

```sh
python3 scripts/run-lse-server-smoke.py --run
```

It checks repeated-prompt isolation, timing counts and normal server exit. Results are written to `build/tests/lse-server-smoke/`. Run it with other GPU workloads stopped.
