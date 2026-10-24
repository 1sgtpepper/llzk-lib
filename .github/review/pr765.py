"""Review-only probes; the candidate's implementation and tests are unchanged."""

import hashlib
import json
import os
from pathlib import Path
import re
import shlex
import shutil
import subprocess
import sys


root = Path.cwd()
out = root / "review-outputs"
out.mkdir(exist_ok=True)
tool = Path(sys.argv[1]).resolve()
os.environ["PATH"] = str(tool.parent) + os.pathsep + os.environ["PATH"]
records = []


def run(name, command, required=True):
    """Retain full streams, the command, and its actual exit status."""
    result = subprocess.run(["bash", "-o", "pipefail", "-c", command], capture_output=True)
    (out / (name + ".stdout")).write_bytes(result.stdout)
    (out / (name + ".stderr")).write_bytes(result.stderr)
    records.append({"name": name, "command": command, "exit_code": result.returncode})
    (out / "commands.json").write_text(json.dumps(records, indent=2) + "\n")
    print(name, "exit", result.returncode, flush=True)
    if required and result.returncode:
        sys.stderr.buffer.write(result.stderr)
        raise SystemExit(result.returncode)
    return result.stdout.decode()


identity = {
    "source_sha": "294f722b0fe73677552308893f769aaee0a23a4e",
    "harness_sha": subprocess.check_output(["git", "rev-parse", "HEAD"], text=True).strip(),
    "tool": str(tool),
    "tool_sha256": hashlib.sha256(tool.read_bytes()).hexdigest(),
    "FileCheck": shutil.which("FileCheck"),
}
(out / "identity.json").write_text(json.dumps(identity, indent=2) + "\n")
run("tool-version", shlex.quote(str(tool)) + " --version")
run("filecheck-version", "FileCheck --version")

tests = [
    "Dialect/Function/call_fieldless_felt_symbol_pass.llzk",
    "Dialect/Function/call_with_template_params_fail.llzk",
    "Dialect/LLZK_and_Builtin/mainAttrVerif_fail.llzk",
    "Dialect/POD/pod_type_pass.llzk",
    "Dialect/Struct/struct_param_included_global_fail.llzk",
    "Dialect/Struct/struct_param_type_variable_fail.llzk",
    "Dialect/Struct/struct_params_fail.llzk",
    "Dialect/Struct/struct_params_restrictions_pass.llzk",
    "Dialect/Struct/struct_params_symbolic_restrictions_pass.llzk",
    "Dialect/Verif/contracts_fail.llzk",
    "Dialect/Verif/include_fieldless_felt_symbol_pass.llzk",
    "Dialect/Verif/include_symbolic_template_argument_fail.llzk",
    "Transforms/TemplateMonomorphization/fieldless_felt_arguments.llzk",
    "Transforms/TemplateMonomorphization/nested_root_type_argument.llzk",
    "Transforms/TemplateMonomorphization/type_argument_forwarding.llzk",
    "Transforms/TemplateMonomorphization/typed_arguments_fail.llzk",
    "Transforms/TypeVarInference/infer_tvars_fail.llzk",
    "Transforms/TemplateMonomorphization/typed_arguments_pass.llzk",
]
for relative in tests:
    source = root / "test" / relative
    commands = re.findall(r"^//\s*RUN:\s*(.*)$", source.read_text(), re.M)
    for index, command in enumerate(commands, 1):
        name = source.stem + "-run" + str(index)
        command = command.replace("%input_dir", shlex.quote(str(root / "test")))
        command = command.replace("%s", shlex.quote(str(source)))
        if "| FileCheck" in command:
            command = command.replace(
                "| FileCheck", "| tee " + shlex.quote(str(out / (name + ".raw.llzk"))) + " | FileCheck"
            )
        run(name, command)

mono = root / "test/Transforms/TemplateMonomorphization"
for stem, expected_clones in [("nested_root_type_argument", 2), ("type_argument_forwarding", 4)]:
    source = mono / (stem + ".llzk")
    flags = " -split-input-file" if stem == "type_argument_forwarding" else ""
    original = run(stem + "-no-pass-control", "llzk-opt" + flags + " --verify-diagnostics " + shlex.quote(str(source)))
    transformed = (out / (stem + "-run2.stdout")).read_text()
    original_clones = len(re.findall(r"^  struct.def @[^\n]*__spec_", original, re.M))
    actual_clones = len(re.findall(r"^  struct.def @[^\n]*__spec_", transformed, re.M))
    assert original_clones == 0, original_clones
    assert actual_clones == expected_clones, (stem, actual_clones, expected_clones)
    print(stem, "both commands succeed; original clones", original_clones, "transformed clones", actual_clones, flush=True)
    raw = out / (stem + "-run2.stdout")
    round_trip = run(stem + "-round-trip", "llzk-opt" + flags + " " + shlex.quote(str(raw)))
    assert round_trip == transformed, stem + " parse-print output changed"

    # Generate each split separately, so separators cannot confuse source-to-check mapping.
    chunks = re.split(r"^// -----\s*$", transformed, flags=re.M)
    for index, chunk in enumerate(chunks, 1):
        if not chunk.strip():
            continue
        chunk_file = out / (stem + "-chunk" + str(index) + ".llzk")
        chunk_file.write_text(chunk.strip() + "\n")
        run(stem + "-generated" + str(index), "python3 scripts/generate-test-checks.py " + shlex.quote(str(chunk_file)))

print("All review probes completed successfully.", flush=True)
