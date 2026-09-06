"""Collect configured style diagnostics for a fixed revision without applying fixes."""

import concurrent.futures
import json
from pathlib import Path
import subprocess
import sys


def main():
    """Run the selected files and preserve commands, diagnostics, and source identity."""
    root, build, output, manifest, label = sys.argv[1:]
    root, build, output = map(Path, (root, build, output))
    output.mkdir(parents=True, exist_ok=True)
    target = next(item for item in json.loads(Path(manifest).read_text()) if item['label'] == label)
    head = subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=root, text=True).strip()
    assert head == target['head'], (head, target['head'])
    (output / 'target.json').write_text(json.dumps(target, indent=2) + '\n')
    for tool in ['clang-tidy', 'clang-format']:
        result = subprocess.run([tool, '--version'], text=True, capture_output=True)
        (output / (tool + '-version.txt')).write_text(result.stdout + result.stderr)
        result.check_returncode()
    config = subprocess.run(['clang-tidy', '--dump-config'], cwd=root, text=True, capture_output=True)
    (output / 'effective-clang-tidy-config.yaml').write_text(config.stdout)
    config.check_returncode()
    (output / 'compile_commands.json').write_bytes((build / 'compile_commands.json').read_bytes())

    def check_file(path):
        """Keep each invocation's diagnostics separate, including missing baseline files."""
        destination = output / path
        destination.mkdir(parents=True, exist_ok=True)
        if not (root / path).is_file():
            record = {'path': path, 'status': 'absent-at-revision'}
        else:
            command = ['clang-tidy', '-p', str(build), '--export-fixes=' + str(destination / 'diagnostics.yaml'), str(root / path)]
            (destination / 'command.json').write_text(json.dumps(command, indent=2) + '\n')
            try:
                result = subprocess.run(command, cwd=root, text=True, capture_output=True, timeout=600)
                (destination / 'stdout.txt').write_text(result.stdout)
                (destination / 'stderr.txt').write_text(result.stderr)
                record = {'path': path, 'status': 'completed', 'exit_code': result.returncode}
            except subprocess.TimeoutExpired as error:
                (destination / 'stdout.txt').write_bytes(error.stdout or b'')
                (destination / 'stderr.txt').write_bytes(error.stderr or b'')
                record = {'path': path, 'status': 'timeout'}
        (destination / 'result.json').write_text(json.dumps(record, indent=2) + '\n')
        print(json.dumps(record), flush=True)
        return record

    with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(check_file, target['files']))
    format_command = ['git-clang-format', '--binary', 'clang-format', '--diff', target['base'], target['head']]
    result = subprocess.run(format_command, cwd=root, text=True, capture_output=True)
    (output / 'format-command.json').write_text(json.dumps(format_command, indent=2) + '\n')
    (output / 'format-output.txt').write_text(result.stdout + result.stderr)
    summary = {'target': target, 'files': results, 'format_exit_code': result.returncode}
    (output / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    status = subprocess.check_output(['git', 'status', '--porcelain', '--untracked-files=no'], cwd=root, text=True)
    (output / 'tracked-status.txt').write_text(status)
    assert not status, status


if __name__ == '__main__':
    main()
