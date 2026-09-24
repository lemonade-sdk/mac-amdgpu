#!/usr/bin/env python3
"""Opt-in local MLX accuracy reference; Apple Metal timings are not R9700 results.

Invoke with LM Studio's installed app-mlx-generate-mac26-arm64@33/bin/python.
Without --run this script imports no MLX and does not read or load model weights.
"""

import argparse
from collections import Counter
import heapq
import math
import hashlib
import importlib.metadata
import json
import os
from pathlib import Path
import struct
import sys
import time


PROMPT = "The capital of France is"
PROMPT_IDS = [760, 6511, 314, 9338, 369]
MODEL = Path.home() / ".lmstudio/models/lmstudio-community/Qwen3.8-27B-MLX-6bit"
OUTPUT = Path(__file__).resolve().parents[1] / "build/tests/qwen-mlx-reference/result.json"


def bounded_tokens(value):
    count = int(value)
    if not 1 <= count <= 64:
        raise argparse.ArgumentTypeError("max tokens must be between 1 and 64")
    return count


def sha256(path):
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(8 * 1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def local_file(directory, name):
    path = (directory / name).resolve(strict=True)
    if path.parent != directory or not path.is_file():
        raise ValueError(f"expected a file inside the local model directory: {name}")
    return path


def checkpoint_metadata(directory):
    config_path = local_file(directory, "config.json")
    config = json.loads(config_path.read_text())
    quant = config.get("quantization", {})
    if (config.get("model_type") != "qwen3_5" or quant.get("bits") != 6
            or quant.get("group_size") != 64 or quant.get("mode", "affine") != "affine"):
        raise ValueError("reference requires the local qwen3_5 affine Q6/group64 checkpoint")
    if config.get("model_file"):
        raise ValueError("custom model code is not supported by this reference")
    index_path = local_file(directory, "model.safetensors.index.json")
    index = json.loads(index_path.read_text())
    weight_map = index["weight_map"]
    shard_names = sorted(set(weight_map.values()))
    if not shard_names or set(shard_names) != {p.name for p in directory.glob("model*.safetensors")}:
        raise ValueError("indexed weight shards do not match the files the MLX loader would read")
    shards = []
    tensor_count = 0
    for name in shard_names:
        path = local_file(directory, name)
        print(f"Hashing local checkpoint shard {name}", file=sys.stderr, flush=True)
        with path.open("rb") as stream:
            size_bytes = struct.unpack("<Q", stream.read(8))[0]
            if not 2 <= size_bytes <= min(64 * 1024 * 1024, path.stat().st_size - 8):
                raise ValueError(f"invalid safetensors header size: {name}")
            header_bytes = stream.read(size_bytes)
        header = json.loads(header_bytes)
        tensors = {key: value for key, value in header.items() if key != "__metadata__"}
        expected = {key for key, shard in weight_map.items() if shard == name}
        if set(tensors) != expected:
            raise ValueError(f"safetensors/index tensor names disagree: {name}")
        tensor_count += len(tensors)
        shards.append({"name": name, "bytes": path.stat().st_size,
                       "sha256": sha256(path), "tensor_count": len(tensors),
                       "header_sha256": hashlib.sha256(header_bytes).hexdigest(),
                       "dtypes": sorted({value["dtype"] for value in tensors.values()})})
    files = {}
    for name in ("config.json", "tokenizer.json", "tokenizer_config.json",
                 "generation_config.json", "model.safetensors.index.json"):
        path = local_file(directory, name)
        files[name] = {"bytes": path.stat().st_size, "sha256": sha256(path)}
    return {"path": str(directory), "config": config, "files": files,
            "shards": shards, "tensor_count": tensor_count,
            "hash_scope": "SHA-256 of complete files, including complete weight shards"}


def configure_precision(model, mx, flatten, precision):
    """Change floating parameters only; never dequantize or repack Q6 weights."""
    before = dict(flatten(model.parameters()))
    integer_parameters = {
        name: value for name, value in before.items()
        if not mx.issubdtype(value.dtype, mx.floating)
    }
    packed = {name: value for name, value in integer_parameters.items()
              if value.dtype == mx.uint32 and name.endswith("weight")}
    if not packed:
        raise ValueError("loaded reference has no packed uint32 weight parameters")
    quantized_modules = {}
    for name, module in model.named_modules():
        if hasattr(module, "bits") and hasattr(module, "group_size"):
            quantized_modules[name] = {
                "class": f"{type(module).__module__}.{type(module).__name__}",
                "bits": module.bits, "group_size": module.group_size,
                "mode": getattr(module, "mode", "affine"),
            }
    if not quantized_modules or any(
            q["bits"] != 6 or q["group_size"] != 64 or q["mode"] != "affine"
            for q in quantized_modules.values()):
        raise ValueError("loaded quantized modules are not uniformly affine Q6/group64")
    if precision == "float32":
        model.set_dtype(mx.float32, predicate=lambda dtype: mx.issubdtype(dtype, mx.floating))
    elif precision != "native":
        raise ValueError(f"unknown reference precision: {precision}")
    after = dict(flatten(model.parameters()))
    if before.keys() != after.keys():
        raise ValueError("precision conversion changed parameter names")
    for name, original in before.items():
        if tuple(original.shape) != tuple(after[name].shape):
            raise ValueError(f"precision conversion changed parameter shape: {name}")
    for name, original in integer_parameters.items():
        if after[name] is not original or after[name].dtype != original.dtype:
            raise ValueError(f"precision conversion changed packed/integer parameter: {name}")
    if precision == "float32" and any(
            mx.issubdtype(value.dtype, mx.floating) and value.dtype != mx.float32
            for value in after.values()):
        raise ValueError("floating parameter remained non-float32 after conversion")
    current_modules = dict(model.named_modules())
    for name, original in quantized_modules.items():
        module = current_modules[name]
        if (module.bits != original["bits"] or module.group_size != original["group_size"]
                or getattr(module, "mode", "affine") != original["mode"]):
            raise ValueError(f"precision conversion changed quantization metadata: {name}")
    return {
        "mode": precision,
        "parameter_dtype_counts_before": dict(Counter(str(v.dtype) for v in before.values())),
        "parameter_dtype_counts_after": dict(Counter(str(v.dtype) for v in after.values())),
        "integer_parameters": len(integer_parameters),
        "packed_uint32_weight_tensors": len(packed),
        "integer_object_identity_preserved": True,
        "quantized_modules": len(quantized_modules),
        "quantization_metadata_preserved": True,
        "quantization": {"bits": 6, "group_size": 64, "mode": "affine"},
        "method": "Module.set_dtype(float32, floating-only predicate)" if precision == "float32" else "unchanged",
        "scope": ("All floating parameters, including quantization scales/biases, are float32; "
                  "packed integer weights are the identical objects. This does not require identical "
                  "kernel fusion, accumulation order or internal arithmetic to LSE. Raw output dtype "
                  "is checked on every step; no claim is made that every internal instruction uses float32.")
                  if precision == "float32" else "Native checkpoint parameters and intermediate precision.",
    }


def ranked_logits(values, count=10):
    """Stable tie reporting: descending logit, then ascending token ID."""
    if not values or any(not math.isfinite(value) for value in values):
        raise ValueError("empty or non-finite logits")
    top = heapq.nsmallest(count, enumerate(values), key=lambda pair: (-pair[1], pair[0]))
    maximum = top[0][1]
    tied = [index for index, value in enumerate(values) if value == maximum]
    return {
        "top10_ids": [index for index, _ in top],
        "top10_logits": [value for _, value in top],
        "top1_top2_margin": maximum - top[1][1] if len(top) > 1 else 0.0,
        "maximum_tie_count": len(tied), "maximum_tie_ids": tied[:64],
        "maximum_tie_ids_truncated": len(tied) > 64,
        "topk_tie_order": "ascending token ID within identical logits",
    }


def run_reference(args, result):
    # Set before importing any MLX/Hugging Face package. The model path must exist
    # locally; no repository identifier or trusted remote model code is accepted.
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["PYTHONDONTWRITEBYTECODE"] = "1"
    sys.dont_write_bytecode = True
    directory = args.model.expanduser().resolve(strict=True)
    if not directory.is_dir():
        raise ValueError("model must be an existing local directory")
    result["runtime_versions"] = {
        name: importlib.metadata.version(name)
        for name in ("mlx", "mlx-metal", "mlx-lm", "transformers", "tokenizers")
    }
    # Tokenization is CPU-only and rejects oversized contexts before model load.
    from tokenizers import Tokenizer
    ids = Tokenizer.from_file(str(local_file(directory, "tokenizer.json"))).encode(
        args.prompt, add_special_tokens=False).ids
    if not ids or len(ids) + args.max_tokens > 128:
        raise ValueError("prompt plus requested output must fit within 128 tokens")
    if args.prompt == PROMPT and ids != PROMPT_IDS:
        raise ValueError(f"default prompt token IDs changed: {ids}")
    result["prompt_ids"] = ids
    started = time.perf_counter()
    result["checkpoint"] = checkpoint_metadata(directory)
    result["checkpoint_hash_seconds"] = time.perf_counter() - started

    import mlx.core as mx
    from mlx_lm import load
    from mlx_lm.models.cache import make_prompt_cache
    from mlx.utils import tree_flatten

    mx.set_default_device(mx.gpu)
    result["device"] = str(mx.default_device())
    started = time.perf_counter()
    model, tokenizer = load(str(directory), tokenizer_config={"local_files_only": True},
                            trust_remote_code=False)
    if type(model).__module__ != "mlx_lm.models.qwen3_5":
        raise ValueError(f"unexpected text-model implementation: {type(model).__module__}")
    if tokenizer.encode(args.prompt, add_special_tokens=False) != ids:
        raise ValueError("MLX tokenizer differs from the checkpoint tokenizer.json")
    # This known text implementation sanitizes away vision and MTP weights.
    # No draft model, speculative decoding, sampling processor or KV quantizer.
    model.eval()
    result["precision_audit"] = configure_precision(model, mx, tree_flatten, args.precision)
    # Cast before constructing recurrent/KV caches: their initial dtype must
    # follow this reference's model activations rather than an earlier run.
    cache = make_prompt_cache(model)
    result["model_load_seconds"] = time.perf_counter() - started
    result["model_class"] = f"{type(model).__module__}.{type(model).__name__}"
    module_path = Path(sys.modules[type(model).__module__].__file__)
    result["model_implementation"] = {"path": str(module_path), "sha256": sha256(module_path)}
    tokens = mx.array([ids])
    generated = []
    result["steps"] = []
    eos = set(tokenizer.eos_token_ids)
    result["eos_token_ids"] = sorted(eos)
    result["finish_reason"] = "length"
    for step in range(args.max_tokens):
        started = time.perf_counter()
        raw_logits = model(tokens, cache=cache)[0, -1, :]
        if args.precision == "float32" and raw_logits.dtype != mx.float32:
            raise ValueError(f"float32 reference produced {raw_logits.dtype} logits at step {step}")
        logits = raw_logits.astype(mx.float32)
        mx.eval(logits)
        forward_seconds = time.perf_counter() - started
        if not bool(mx.all(mx.isfinite(logits)).item()):
            raise ValueError(f"non-finite logits at generated step {step}")
        chosen = int(mx.argmax(logits).item())
        ranking = ranked_logits(logits.tolist())
        if chosen not in ranking["maximum_tie_ids"] and not ranking["maximum_tie_ids_truncated"]:
            raise ValueError("MLX argmax did not select a maximum logit")
        generated.append(chosen)
        result["steps"].append({"index": step, "token_id": chosen,
            "context_tokens": len(ids) + step, "raw_logits_dtype": str(raw_logits.dtype),
            **ranking, "forward_seconds": forward_seconds})
        print(f"Reference token {step + 1}/{args.max_tokens}: id={chosen}",
              file=sys.stderr, flush=True)
        if chosen in eos:
            result["finish_reason"] = "stop"
            break
        tokens = mx.array([[chosen]])
    mx.synchronize()
    result["generated_ids"] = generated
    result["text"] = tokenizer.decode(generated)
    result["generated_tokens"] = len(generated)
    result["decode_steps"] = max(0, len(generated) - 1)
    result["all_requested_tokens_completed"] = len(generated) == args.max_tokens
    result["success"] = True


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run", action="store_true", help="explicitly load and run the local model on Apple Metal")
    parser.add_argument("--model", type=Path, default=MODEL)
    parser.add_argument("--prompt", default=PROMPT, help="raw prompt; no chat template or added special tokens")
    parser.add_argument("--max-tokens", type=bounded_tokens, default=33)
    parser.add_argument("--output", type=Path, default=OUTPUT)
    parser.add_argument("--precision", choices=("native", "float32"), default="native",
                        help="float32 casts floating parameters/scales/biases, preserving packed Q6 integer weights")
    args = parser.parse_args()
    if not args.run:
        parser.print_help()
        print("No model loaded; --run is required. No MLX import or GPU calls performed.")
        return 2
    result = {"success": False, "purpose": "independent same-checkpoint accuracy reference",
        "platform": "Apple Metal MLX; not R9700 performance",
        "timing_scope": "host wall time per forward evaluation; excludes top-k extraction; diagnostic only, not matched LSE throughput",
        "prompt": args.prompt, "requested_generated_tokens": args.max_tokens,
        "context_limit": 128, "sampling": "greedy raw-logit argmax", "chat_template": False,
        "mtp": False, "kv_quantization": False,
        "precision": args.precision,
        "precision_description": ("MLX native checkpoint/intermediate precision; float32 logit reporting does not change model precision"
                                  if args.precision == "native" else
                                  "Float32 floating parameters/scales/biases and checked float32 logits; unchanged packed Q6 weights. Internal fusion/accumulation may differ from LSE."),
        "python": sys.version, "python_executable": sys.executable}
    try:
        run_reference(args, result)
    except Exception as error:
        result["error"] = f"{type(error).__name__}: {error}"
        print(result["error"], file=sys.stderr)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    serialized = json.dumps(result, indent=2, allow_nan=False) + "\n"
    args.output.write_text(serialized)
    print(serialized, end="")
    return 0 if result["success"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
