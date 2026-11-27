#!/usr/bin/env python3
"""Local C/C++ naming cleanup for MeshCoreWatchUltra. Python 3.8+; no dependencies."""
import argparse
import difflib
import re
import sys
import tempfile
from collections import defaultdict
from pathlib import Path

ROOTS = ('main', 'hardware', 'meshcore', 'services', 'watch')
EXT = {'.c', '.cc', '.cpp', '.cxx', '.h', '.hh', '.hpp', '.hxx', '.ipp', '.inl'}
EXCLUDED = {'third', 'third_party', 'third-party', 'thirdparty', 'managed_components', 'generated', 'designer', 'build', '.git', '.vscode', 'vendor', 'external'}
IDENT = re.compile(r'[A-Za-z_][A-Za-z_0-9]*\Z')
# Protect comments, literals (including raw strings), header names and numbers.
TOKEN = re.compile(r'(?m:^[ \t]*#[ \t]*(?:include|include_next|import)\b[^\n]*(?:\\\n[^\n]*)*)|//(?:[^\n\\]|\\[^\n]|\\\n)*|/\*[\s\S]*?\*/|(?:u8|u|U|L)?R"(?P<delimiter>[^\s()\\]{0,16})\([\s\S]*?\)(?P=delimiter)"|(?:u8|u|U|L)?"(?:\\[\s\S]|[^"\\])*"|(?:u8|u|U|L)?\'(?:\\[\s\S]|[^\'\\])*\'|(?:\d|\.\d)[\w.\']*|[A-Za-z_][A-Za-z_0-9]*|[^\s]')
PREFIX = re.compile(r'^(?:[smg]_[A-Za-z][A-Za-z_0-9]*|k[A-Z][A-Za-z_0-9]*)$')
PREFERRED = {'s_charging': 'charging_active', 's_task': 'task_handle', 's_timeout_ms': 'inactivity_timeout_ms', 's_charging_display': 'charging_display_callback'}
KEYWORDS = set('alignas alignof and and_eq asm auto bitand bitor bool break case catch char char8_t char16_t char32_t class compl concept const consteval constexpr constinit const_cast continue co_await co_return co_yield decltype default delete do double dynamic_cast else enum explicit export extern false float for friend goto if inline int long mutable namespace new noexcept not not_eq nullptr operator or or_eq private protected public register reinterpret_cast requires return short signed sizeof static static_assert static_cast struct switch template this thread_local throw true try typedef typeid typename union unsigned using virtual void volatile wchar_t while xor xor_eq'.split())

def snake(name):
    name = re.sub(r'([A-Z]+)([A-Z][a-z])', r'\1_\2', name)
    name = re.sub(r'([a-z0-9])([A-Z])', r'\1_\2', name)
    return name.lower()

def tokens(src):
    return list(TOKEN.finditer(src))

def ids(ts):
    return {t.group() for t in ts if IDENT.fullmatch(t.group())}

def own_files(root):
    for folder in ROOTS:
        base = root / folder
        if not base.is_dir() or base.is_symlink():
            continue
        for p in sorted(base.rglob('*')):
            rel = p.relative_to(root)
            if any(part.lower() in EXCLUDED or part.lower().startswith('build_') for part in rel.parts):
                continue
            if p.suffix.lower() in EXT and p.is_file() and not any((root.joinpath(*rel.parts[:i])).is_symlink() for i in range(1, len(rel.parts)+1)):
                yield p

def protected_names(root):
    # Read only checked-in vendor/generated source to avoid renaming shared names.
    # Never traverse managed_components or build trees.
    result = set()
    for base in [root/'third', root/'third_party', root/'third-party', root/'thirdparty', root/'watch/WatchDisplay/generated']:
        if not base.is_dir() or base.is_symlink():
            continue
        for p in base.rglob('*'):
            if p.is_file() and not p.is_symlink() and p.suffix.lower() in EXT:
                result.update(ids(tokens(p.read_bytes().decode('utf-8', errors='replace'))))
    return result

def plan(root):
    sources = {}
    existing = set(KEYWORDS)
    declarations = set()
    occurrences = defaultdict(set)
    protected = protected_names(root)
    for path in own_files(root):
        src = path.read_bytes().decode('utf-8')
        ts = tokens(src)
        sources[path] = (src, ts)
        existing.update(ids(ts))
        for i, t in enumerate(ts):
            name = t.group()
            if not PREFIX.fullmatch(name):
                continue
            occurrences[name].add(path)
            prev = ts[i-1].group() if i else ''
            nxt = ts[i+1].group() if i+1 < len(ts) else ''
            # Conservative declaration heuristic: type/ pointer / reference,
            # followed by an initializer or declaration terminator. Not a C++ AST.
            if (IDENT.fullmatch(prev) and prev not in {'return','case','throw','delete','sizeof','co_return'} or prev in {'*','&','>','&&'}) and nxt in {'=', '{', ';', '[', ',', ')'}:
                declarations.add(name)
            # Avoid preprocessor stringification and token pasting.
            if prev == '#' or nxt == '#':
                protected.add(name)
        # Avoid macro names and any prefixed tokens in macro definitions.
        for directive in re.finditer(r'(?m)^[ \t]*#[ \t]*(?:define|undef)\b[^\n]*(?:\\\n[^\n]*)*', src):
            protected.update(n for n in ids(tokens(directive.group())) if PREFIX.fullmatch(n))
    mapping = {}
    skipped = {}
    reserved = existing | protected
    for old in sorted(occurrences):
        if old in protected:
            skipped[old] = 'shared with excluded source or preprocessor'
            continue
        if old not in declarations:
            skipped[old] = 'no recognizable owned variable declaration'
            continue
        target = PREFERRED.get(old, snake(old[2:] if old[1]=='_' else old[1:]))
        if target in reserved or not IDENT.fullmatch(target):
            skipped[old] = 'target already exists: ' + target
            continue
        mapping[old] = target
        reserved.add(target)
    changes = {}
    for path, (src, ts) in sources.items():
        parts = []
        pos = 0
        for t in ts:
            if t.group() in mapping:
                parts.extend((src[pos:t.start()], mapping[t.group()]))
                pos = t.end()
        if parts:
            parts.append(src[pos:])
            changes[path] = (src, ''.join(parts))
    return mapping, skipped, changes

def apply(root, changes):
    # Back up all originals before writing any file; restore on write failure.
    backup = Path(tempfile.mkdtemp(prefix='naming-backup-', dir=root))
    for path, (old, _) in changes.items():
        dest = backup / path.relative_to(root)
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(old.encode('utf-8'))
    written = []
    try:
        for path, (old, new) in changes.items():
            if path.read_bytes() != old.encode('utf-8'):
                raise ValueError(f'File changed during patch: {path}')
            written.append(path)
            path.write_bytes(new.encode('utf-8'))
    except BaseException:
        for path in written:
            path.write_bytes((backup / path.relative_to(root)).read_bytes())
        raise
    print('Backup:', backup)

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=Path.cwd())
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument('--check', action='store_true', help='Preview changes (default)')
    mode.add_argument('--apply', action='store_true', help='Apply reviewed changes')
    args = parser.parse_args()
    root = args.root.resolve()
    if not (root/'CMakeLists.txt').is_file() or not (root/'main').is_dir() or not (root/'hardware').is_dir():
        raise ValueError('Run from the MeshCoreWatchUltra repository root or specify --root.')
    mapping, skipped, changes = plan(root)
    for old, new in mapping.items():
        print(f'{old} -> {new}')
    for old, why in skipped.items():
        print(f'SKIPPED {old}: {why}')
    print(f'{len(mapping)} identifier spellings, {len(changes)} files, {len(skipped)} skipped.')
    if not changes:
        return
    if args.apply:
        apply(root, changes)
        print('Applied. Review git diff and run idf.py build.')
    else:
        for path, (old, new) in changes.items():
            rel = str(path.relative_to(root))
            sys.stdout.write(''.join(difflib.unified_diff(old.splitlines(True), new.splitlines(True), fromfile=rel, tofile=rel+' (renamed)')))
        print('\nPreview only. Use --apply to write files.')

if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError) as exc:
        print('Patch aborted:', exc, file=sys.stderr)
        sys.exit(1)
