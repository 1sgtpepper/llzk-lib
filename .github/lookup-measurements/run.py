#!/usr/bin/env python3
"""Compare invocation-local lookup variants with paired CLI timings and differential output."""
import hashlib
import json
import os
from pathlib import Path
import random
import re
import resource
import shutil
import statistics
import subprocess
import sys
import time

DRIVER = Path(__file__).resolve().parent
SOURCE = Path(sys.argv[1]).resolve()
OUT = Path(sys.argv[2]).resolve()
OUT.mkdir(parents=True, exist_ok=True)
VARIANTS = ['baseline', 'tables', 'filtered', 'combined']
BASE_HEAD = 'af667b15804e80b08454279ca2769eb18f2b7a5f'
MANIFEST = json.loads((DRIVER / 'variants.json').read_text())


def sha(path):
    """Identify exact source, fixture, and executable bytes."""
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def command(args, log, cwd=SOURCE):
    """Record and execute a bounded build command, preserving failure output."""
    print('RUN', ' '.join(map(str, args)), flush=True)
    with Path(log).open('w') as stream:
        stream.write('command=' + json.dumps(list(map(str, args))) + '\n')
        stream.flush()
        subprocess.run(args, cwd=cwd, stdout=stream, stderr=subprocess.STDOUT, check=True)


def functions(ty):
    """Use the existing struct compute/constrain contract for generated workloads."""
    return f'''      function.def @compute() -> {ty} {{
        %self = struct.new : {ty}
        function.return %self : {ty}
      }}
      function.def @constrain(%self: {ty}) {{ function.return }}
'''


def template(k, restriction='felt'):
    """Generate a target with distinct, ordered parameter identities."""
    restrictions = {'felt': ' : !felt.type<"bn128">', 'index': ' : index', 'any': ''}
    params = ''.join(f'    poly.param @P{i}' + (f' : !poly.tvar<@P{i}>' if restriction == 'type' else restrictions[restriction]) + '\n' for i in range(k))
    ty = '!struct.type<@T::@S<[' + ', '.join(f'@P{i}' for i in range(k)) + ']>>'
    return '  poly.template @T {\n' + params + '    struct.def @S {\n' + functions(ty) + '    }\n  }\n'


def workload(k, members, globals_count=0, included=False, local_count=0, restriction='felt', literal=False):
    """Vary one cost dimension while keeping the verified argument contract valid."""
    global_lines = ''.join(f'  global.def const @G{i} : !felt.type<"bn128"> = {i+1}\n' for i in range(globals_count))
    prefix = '  include.from "library.llzk" as @Lib\n' if included else global_lines
    if local_count:
        references = [f'@L{local_count-k+i}' for i in range(k)]
    elif literal:
        references = ['index' if restriction == 'type' else f'{i+1} : index' for i in range(k)]
    else:
        references = [(f'@Lib::@G{i}' if included else f'@G{i}') for i in range(k)]
    ty = '!struct.type<@T::@S<[' + ', '.join(references) + ']>>'
    use = ''.join(f'    struct.member @m{i} : {ty}\n' for i in range(members))
    use_ty = '!struct.type<@Caller::@Use<[' + ', '.join(f'@L{i}' for i in range(local_count)) + ']>>' if local_count else '!struct.type<@Use>'
    use = '  struct.def @Use {\n' + use + functions(use_ty) + '  }\n'
    if local_count:
        bindings = ''.join(f'    poly.param @L{i} : !felt.type<"bn128">\n' for i in range(local_count))
        use = '  poly.template @Caller {\n' + bindings + use + '  }\n'
    return 'module attributes {llzk.lang} {\n' + prefix + template(k, restriction) + use + '}\n', 'module attributes {llzk.lang} {\n' + global_lines + '}\n'


def fixtures():
    """Build explicit controls, scale experiments, and untouched repository workloads."""
    cases = []
    directory = OUT / 'workloads'
    directory.mkdir(exist_ok=True)
    specs = [
        ('native-small', dict(k=1, members=1, globals_count=1)),
        ('native-medium', dict(k=8, members=32, globals_count=256)),
        ('native-large', dict(k=16, members=64, globals_count=2048)),
        ('include-small', dict(k=1, members=1, globals_count=1, included=True)),
        ('include-medium', dict(k=4, members=8, globals_count=128, included=True)),
        ('include-large', dict(k=8, members=16, globals_count=512, included=True)),
        ('local-small', dict(k=1, members=1, local_count=1)),
        ('local-medium', dict(k=8, members=64, local_count=64)),
        ('local-large', dict(k=16, members=64, local_count=256)),
        ('unrestricted-native', dict(k=8, members=32, globals_count=256, restriction='any')),
        ('unrestricted-include', dict(k=4, members=8, globals_count=128, restriction='any', included=True)),
        ('literal-small', dict(k=1, members=1, restriction='index', literal=True)),
        ('literal-large', dict(k=32, members=128, restriction='index', literal=True)),
        ('type-small', dict(k=1, members=1, restriction='type', literal=True)),
        ('type-large', dict(k=32, members=128, restriction='type', literal=True)),
    ]
    for name, spec in specs:
        folder = directory / name
        folder.mkdir(exist_ok=True)
        text, library = workload(**spec)
        path = folder / 'input.llzk'
        path.write_text(text)
        (folder / 'library.llzk').write_text(library)
        cases.append({'name': name, 'kind': 'synthetic', 'dimensions': spec, 'args': ['-I', str(folder), str(path)], 'expected_exit': 0, 'source_sha256': sha(path), 'include_sha256': sha(folder / 'library.llzk')})
    for rel, flags in [
        ('test/Dialect/Struct/struct_params_symbolic_restrictions_pass.llzk', ['-verify-diagnostics']),
        ('test/Dialect/Function/call_fieldless_felt_symbol_pass.llzk', ['-verify-diagnostics']),
        ('test/Dialect/Verif/include_fieldless_felt_symbol_pass.llzk', ['-verify-diagnostics']),
        ('test/Transforms/Flattening/mastermind_with_main.llzk', ['-llzk-flatten']),
        ('test/Transforms/Flattening/mastermind_included_with_main.llzk', ['-llzk-inline-includes', '-llzk-flatten']),
        ('test/Transforms/Flattening/zir_example_9.llzk', ['-llzk-flatten']),
        ('test/Transforms/Flattening/instantiate_column_field_globals_pass.llzk', ['-split-input-file', '-llzk-const-global-propagation', '-llzk-flatten']),
    ]:
        path = SOURCE / rel
        cases.append({'name': path.stem, 'kind': 'repository', 'args': ['-I', str(SOURCE / 'test'), *flags, str(path)], 'expected_exit': 0, 'source_sha256': sha(path)})
    for wrong in (False, True):
        # Successful restricted argument cannot hide either remaining generic failure.
        # A restriction failure must precede and suppress both generic diagnostics.
        params = template(3).replace('poly.param @P1 : !felt.type<"bn128">', 'poly.param @P1').replace('poly.param @P2 : !felt.type<"bn128">', 'poly.param @P2 : !poly.tvar<@P2>')
        arg = '@Wrong' if wrong else '@Good'
        path = directory / ('restriction-first.llzk' if wrong else 'mixed-failures.llzk')
        path.write_text('module attributes {llzk.lang} {\n  global.def const @Good : !felt.type<"bn128"> = 1\n  global.def const @Wrong : !felt.type<"goldilocks"> = 2\n' + params + '  struct.def @Use {\n    struct.member @bad : !struct.type<@T::@S<[' + arg + ', @MissingValue, !array.type<@MissingDimension x !felt.type>]>>\n' + functions('!struct.type<@Use>') + '  }\n}\n')
        cases.append({'name': path.stem, 'kind': 'diagnostic', 'args': [str(path)], 'expected_exit': 1, 'source_sha256': sha(path)})
    (OUT / 'workloads.json').write_text(json.dumps(cases, indent=2) + '\n')
    return cases


def build():
    """Rebuild each variant incrementally in identical release settings; run full suites."""
    if subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=SOURCE, text=True).strip() != BASE_HEAD:
        raise RuntimeError('baseline head mismatch')
    source_file = SOURCE / 'lib/Util/SymbolHelper.cpp'
    baseline = source_file.read_bytes()
    command(['cmake', '-S', str(SOURCE), '-B', 'build/lookup-release', '-G', 'Ninja', '-DCMAKE_BUILD_TYPE=Release', '-DLLZK_VERSION_OVERRIDE=3.0.0', '-DLLZK_BUILD_DEVTOOLS=ON', '-DBUILD_SHARED_LIBS=OFF'], OUT / 'configure.log')
    cache = SOURCE / 'build/lookup-release/CMakeCache.txt'
    if not re.search(r'GTest_DIR:PATH=/nix/store/[^\n]+', cache.read_text()):
        raise RuntimeError('unit tests must be configured; GTest missing')
    binary_dir = OUT / 'binaries'
    binary_dir.mkdir(exist_ok=True)
    for name in VARIANTS:
        source_file.write_bytes(baseline)
        if name != 'baseline':
            command(['git', 'apply', str(DRIVER / 'patches' / (name + '.patch'))], OUT / (name + '-patch.log'))
        if sha(source_file) != MANIFEST[name]['source_sha256']:
            raise RuntimeError('variant source hash mismatch')
        command(['clang-format', '--dry-run', '--Werror', str(source_file)], OUT / (name + '-format.log'))
        command(['cmake', '--build', 'build/lookup-release', '--parallel', '2', '--target', 'check'], OUT / (name + '-check.log'))
        folder = OUT / name
        folder.mkdir(exist_ok=True)
        shutil.copy2(SOURCE / 'build/lookup-release/test/report.xml', folder / 'lit-report.xml')
        shutil.copy2(SOURCE / 'build/lookup-release/Testing/Temporary/LastTest.log', folder / 'unit-report.log')
        shutil.copy2(source_file, folder / 'SymbolHelper.cpp')
        shutil.copy2(SOURCE / 'build/lookup-release/compile_commands.json', folder / 'compile_commands.json')
        exe = binary_dir / name
        shutil.copy2(SOURCE / 'build/lookup-release/bin/llzk-opt', exe)
        needed = subprocess.check_output(['ldd', str(exe)], text=True)
        (folder / 'ldd.txt').write_text(needed)
        if str(SOURCE / 'build') in needed:
            raise RuntimeError('snapshot binary has mutable build-tree shared-library dependency')
        (folder / 'identity.json').write_text(json.dumps({'baseline_head': BASE_HEAD, 'variant': name, 'source_sha256': sha(source_file), 'binary_sha256': sha(exe), 'version': subprocess.check_output([str(exe), '--version'], text=True)}, indent=2) + '\n')
        print('BUILD PASSED', name, flush=True)
    source_file.write_bytes(baseline)
    shutil.copy2(cache, OUT / 'CMakeCache.txt')


def differential(cases):
    """Compare exact bytes, exit status, mixed error aggregation, and failure precedence."""
    rows = []
    for case in cases:
        values = []
        for name in VARIANTS:
            result = subprocess.run([str(OUT / 'binaries' / name), *case['args']], capture_output=True, timeout=60)
            folder = OUT / name / 'differential'
            folder.mkdir(exist_ok=True)
            (folder / (case['name'] + '.stdout')).write_bytes(result.stdout)
            (folder / (case['name'] + '.stderr')).write_bytes(result.stderr)
            values.append((result.returncode, result.stdout, result.stderr))
        if values[0][0] != case['expected_exit'] or any(v != values[0] for v in values[1:]):
            raise RuntimeError('differential failure: ' + case['name'])
        stderr = values[0][2].decode()
        if case['name'] == 'mixed-failures':
            if stderr.count('error:') != 2 or not (stderr.index('references unknown symbol "@MissingValue"') < stderr.index('references unknown symbol "@MissingDimension"')):
                raise RuntimeError('mixed failures must aggregate in original order')
        if case['name'] == 'restriction-first':
            if stderr.count('error:') != 1 or 'is not compatible with parameter' not in stderr or 'references unknown symbol' in stderr:
                raise RuntimeError('restriction failure must precede remaining validation')
        rows.append({'name': case['name'], 'exit': values[0][0], 'stdout_sha256': hashlib.sha256(values[0][1]).hexdigest(), 'stderr_sha256': hashlib.sha256(values[0][2]).hexdigest(), 'all_variants_identical': True})
    (OUT / 'differential.json').write_text(json.dumps(rows, indent=2) + '\n')


def open_counts(cases):
    """Count successful include-file openat calls outside timing; preserve complete traces."""
    rows = []
    for case in cases:
        if case['name'] not in ('include-small', 'include-medium', 'unrestricted-include'):
            continue
        for name in VARIANTS:
            trace = OUT / name / (case['name'] + '.strace')
            subprocess.run(['strace', '-qq', '-f', '-e', 'trace=openat', '-o', str(trace), str(OUT / 'binaries' / name), *case['args'], '-o', '/dev/null'], check=True, timeout=60, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            lines = [line for line in trace.read_text().splitlines() if 'library.llzk"' in line and not re.search(r'= -1\b', line)]
            rows.append({'case': case['name'], 'variant': name, 'successful_include_opens': len(lines), 'matching_lines': lines})
    (OUT / 'open-counts.json').write_text(json.dumps(rows, indent=2) + '\n')
    by_case = {(r['case'], r['variant']): r['successful_include_opens'] for r in rows}
    for case in ('include-small', 'include-medium'):
        base = by_case[case, 'baseline']
        if not base > by_case[case, 'filtered'] == by_case[case, 'combined'] or base != by_case[case, 'tables']:
            raise RuntimeError('restricted include work reduction not observed')
    if len({by_case['unrestricted-include', n] for n in VARIANTS}) != 1:
        raise RuntimeError('unrestricted include lookup count changed')


def timed(name, case, repeats):
    """Measure monotonic elapsed time and child CPU time for an uninstrumented CLI batch."""
    before = resource.getrusage(resource.RUSAGE_CHILDREN)
    start = time.perf_counter_ns()
    for _ in range(repeats):
        subprocess.run([str(OUT / 'binaries' / name), *case['args'], '-o', '/dev/null'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=60)
    elapsed = (time.perf_counter_ns() - start) / 1e9 / repeats
    after = resource.getrusage(resource.RUSAGE_CHILDREN)
    cpu = ((after.ru_utime + after.ru_stime) - (before.ru_utime + before.ru_stime)) / repeats
    return {'wall_seconds': elapsed, 'cpu_seconds': cpu}


def benchmark(cases):
    """Alternate all four variants on one CPU with fixed-seed randomized paired rounds."""
    cpus = sorted(os.sched_getaffinity(0))
    os.sched_setaffinity(0, {cpus[0]})
    metadata = {'available_cpus': cpus, 'pinned_cpu': cpus[0], 'uname': list(os.uname()), 'clock': str(time.get_clock_info('perf_counter')), 'rounds': 21, 'warmups': 3, 'seed': 765, 'baseline_head': BASE_HEAD, 'driver_head': os.environ.get('GITHUB_SHA'), 'run_url': 'https://github.com/' + os.environ.get('GITHUB_REPOSITORY', '') + '/actions/runs/' + os.environ.get('GITHUB_RUN_ID', '')}
    (OUT / 'machine.json').write_text(json.dumps(metadata, indent=2) + '\n')
    (OUT / 'cpuinfo.txt').write_text(Path('/proc/cpuinfo').read_text())
    generator = random.Random(765)
    rows = []
    for case in cases:
        if case['kind'] == 'diagnostic':
            continue
        for name in VARIANTS:
            timed(name, case, 3)
        probe = timed('baseline', case, 1)['wall_seconds']
        repeats = max(1, min(12, int(0.05 / probe)))
        samples = {name: [] for name in VARIANTS}
        for round_index in range(21):
            order = VARIANTS.copy()
            generator.shuffle(order)
            for name in order:
                sample = timed(name, case, repeats)
                sample.update({'round': round_index, 'order': order.index(name)})
                samples[name].append(sample)
        peaks = {}
        for name in VARIANTS:
            log = OUT / name / (case['name'] + '.memory.txt')
            subprocess.run(['/usr/bin/time', '-f', '%M', '-o', str(log), str(OUT / 'binaries' / name), *case['args'], '-o', '/dev/null'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=60)
            peaks[name] = int(log.read_text().strip())
        row = {'case': case['name'], 'kind': case['kind'], 'batch_repeats': repeats, 'samples': samples, 'peak_rss_kib': peaks}
        rows.append(row)
        (OUT / 'timings.json').write_text(json.dumps(rows, indent=2) + '\n')
        print('MEASURED', case['name'], {n: round(statistics.median(s['wall_seconds'] for s in samples[n]) * 1000, 3) for n in VARIANTS}, flush=True)


if __name__ == '__main__':
    if os.environ.get('REUSE_RUN_ID'):
        for name in VARIANTS:
            identity = json.loads((OUT / name / 'identity.json').read_text())
            if identity['baseline_head'] != BASE_HEAD or identity['source_sha256'] != MANIFEST[name]['source_sha256'] or identity['binary_sha256'] != sha(OUT / 'binaries' / name):
                raise RuntimeError('reused build identity mismatch')
            (OUT / 'binaries' / name).chmod(0o755)
        (OUT / 'reused-build-run.txt').write_text(os.environ['REUSE_RUN_ID'] + '\n')
    else:
        build()
    cases = fixtures()
    differential(cases)
    open_counts(cases)
    benchmark(cases)
    print('ALL MEASUREMENT AND SEMANTIC GATES PASSED', flush=True)
