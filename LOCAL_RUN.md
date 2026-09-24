# Local Terminal run sheet

Open with `open -a TextEdit LOCAL_RUN.md` or `less LOCAL_RUN.md` (`q` exits). Dependency pins and signing requirements: [full setup guide](docs/LSE_QUICKSTART.md). Projects: [mac_amdgpu](https://github.com/lemonade-sdk/mac-amdgpu), [Lemon Seed Engine](https://github.com/Geramy/LSE).

**Current boundary:** HRX compute, LSE Q6 and convolution pass on the GPU. Qwen weights load; full execution stops at a missing Loom `repeat` template. Full GPU token generation, HTTP chat and TPS/PP/s remain unverified.

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

Default model: `~/.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit`; override with `--model PATH`. The runner uses HRX/Loom, strict GPU execution, KV128, no MTP, one greedy token and statistics. Run one GPU workload at a time.

## HTTP chat candidate — pending full-model qualification

Server build/option parsing pass; model generation and shutdown need hardware validation. After one-token qualification succeeds, the intended server command is:

```sh
export DYLD_LIBRARY_PATH="$PWD/build/hsa"
export LSE_REQUIRE_DEVICE_KERNELS=1
MODEL="$HOME/.lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit"
build/lse-macos-adapter/lse-server --model "$MODEL" \
  --pool hrx:0 --dialect loom --no-mtp --kv-len 128 \
  --host 127.0.0.1 --port 8080 --served-name local-qwen --max-tokens 16
```

Wait for model loading and the listening message. In another Terminal:

```sh
curl -sS http://127.0.0.1:8080/health
curl -sS http://127.0.0.1:8080/v1/models
curl -sS http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"local-qwen","messages":[{"role":"user","content":"Say hello."}],"temperature":0,"max_tokens":8,"stream":false}' \
  | python3 -m json.tool
```

`/health` checks HTTP availability. Responses expose **PP/s** in `timings.prompt_per_second` and **decode TPS** in `timings.predicted_per_second`, plus counts/durations. CLI `--stats` reports prefill time and decode tokens/s. Initial JIT/model loading affect latency. Ctrl-C requests shutdown; wait for workloads to exit before replacing the driver.
