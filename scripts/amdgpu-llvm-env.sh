# Sourced by shader/code-object regression scripts. The machine-code fixture
# was verified with LLVM 21.1.8; changing the compiler requires revalidating it.
if [[ -n "${AMDGPU_LLVM_BIN:-}" ]]; then
  llvm_bin="$AMDGPU_LLVM_BIN"
elif [[ -x /opt/homebrew/Cellar/llvm/21.1.8/bin/llvm-mc ]]; then
  llvm_bin=/opt/homebrew/Cellar/llvm/21.1.8/bin
elif [[ -x /opt/homebrew/opt/llvm@21/bin/llvm-mc ]]; then
  llvm_bin=/opt/homebrew/opt/llvm@21/bin
else
  llvm_bin=/opt/homebrew/opt/llvm/bin
fi

# Homebrew keeps older kegs but updates /opt symlinks on dependency upgrades.
# Find versioned Z3 dylibs by their original basename without changing global
# links or loading a different ABI under the old library's name.
amdgpu_llvm_fallback="${DYLD_FALLBACK_LIBRARY_PATH:-/usr/local/lib:/usr/lib}"
for amdgpu_z3_lib in /opt/homebrew/Cellar/z3/*/lib; do
  [[ -d "$amdgpu_z3_lib" ]] || continue
  amdgpu_llvm_fallback="$amdgpu_llvm_fallback:$amdgpu_z3_lib"
done
export DYLD_FALLBACK_LIBRARY_PATH="$amdgpu_llvm_fallback"
