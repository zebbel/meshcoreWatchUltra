#!/usr/bin/env python3
"""Apply the single-entry-point MeshCore initialization refactor locally."""
import difflib
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path.cwd()
PATCH = ROOT / 'meshcore-init-refactor.patch'
MAIN = 'main/main.cpp'
SOURCE = 'meshcore/MeshCorePort/MeshCorePort.cpp'
HEADER = 'meshcore/MeshCorePort/include/MeshCorePort.h'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'Expected exactly one occurrence of {old[:100]!r}')
    return text.replace(old, new, 1)


def transform(path, text):
    if path == MAIN:
        start = '    // Initialize MeshCore if SD storage is available.\n'
        end = '    // Start MeshCore Bluetooth companion.'
        if text.count(start) != 1 or text.count(end) != 1:
            raise ValueError('Unexpected main.cpp initialization block')
        a, b = text.index(start), text.index(end)
        block = text[a:b]
        for required in ('hardware::sd::ready()', 'meshcore::prepare_storage()',
                         'hardware::lora::init()', 'meshcore::init()'):
            if block.count(required) != 1:
                raise ValueError(f'Unexpected main.cpp: {required}')
        # Match the reviewed block exactly rather than silently dropping local changes.
        expected = '''    // Initialize MeshCore if SD storage is available.
    bool meshcore_enabled = false;
    if(hardware::sd::ready()){
        // Load persistent MeshCore data before initializing LoRa.
        err = meshcore::prepare_storage();
        if(err == ESP_OK) err = hardware::lora::init();
        if(err == ESP_OK) err = meshcore::init();
        meshcore_enabled = (err == ESP_OK);
        if(!meshcore_enabled) ESP_LOGE(TAG, "MeshCore unavailable: %s", esp_err_to_name(err));
    } else {
        ESP_LOGW(TAG, "SD unavailable; MeshCore disabled");
    }

'''
        if block != expected:
            raise ValueError('main.cpp block differs from the reviewed source; refusing to overwrite')
        return text[:a] + '''    // Initialize MeshCore.
    err = meshcore::init();
    const bool meshcore_enabled = (err == ESP_OK);
    if(!meshcore_enabled) ESP_LOGW(TAG, "MeshCore unavailable: %s", esp_err_to_name(err));

''' + text[b:]
    if path == HEADER:
        return replace_once(text, '''    // Preload ALL MeshCore files in a single SD session before radio init.
    esp_err_t prepare_storage();
    // Called after prepare_storage() and hardware::lora::init(). No SD I/O.
''', '''    // Load persistent storage, initialize LoRa and start MeshCore.
''')
    if path == SOURCE:
        text = replace_once(text, '    esp_err_t prepare_storage(){', '    static esp_err_t prepare_storage(){')
        return replace_once(text, '''    esp_err_t init(){
        if(initialized) return ESP_OK;
        if(!prepared || node == nullptr || prepared_radio == nullptr || settings_store == nullptr)
            return ESP_ERR_INVALID_STATE;

        const hardware::lora::config_t config = settings_store->get().radio();
        esp_err_t err = hardware::lora::configure(config);''', '''    esp_err_t init(){
        if(initialized) return ESP_OK;
        if(!hardware::sd::ready()){
            ESP_LOGW(TAG, "SD unavailable; MeshCore disabled");
            return ESP_ERR_INVALID_STATE;
        }

        // Finish the SD session before initializing the radio.
        esp_err_t err = prepare_storage();
        if(err != ESP_OK) return err;

        if(!prepared || node == nullptr || prepared_radio == nullptr || settings_store == nullptr)
            return ESP_ERR_INVALID_STATE;

        err = hardware::lora::init();
        if(err != ESP_OK){
            ESP_LOGE(TAG, "LoRa init failed: %s", esp_err_to_name(err));
            return err;
        }

        const hardware::lora::config_t config = settings_store->get().radio();
        err = hardware::lora::configure(config);''')
    raise ValueError(path)


def git(*args):
    return subprocess.run(['git', *args], cwd=ROOT, check=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)


def main():
    if len(sys.argv) != 1:
        raise ValueError('Run without arguments from the repository root')
    if git('rev-parse', '--show-prefix').stdout.strip():
        raise ValueError('Run from the repository root')
    paths = (MAIN, SOURCE, HEADER)
    if git('status', '--porcelain', '--', *paths).stdout.strip():
        raise ValueError('Affected files contain uncommitted changes; preserve them before applying')
    if PATCH.exists():
        raise ValueError(f'{PATCH.name} already exists; preserve or remove it before running again')
    patches = []
    for path in paths:
        raw = (ROOT / path).read_bytes()
        original = raw.decode('utf-8')
        normalized = original.replace('\r\n', '\n')
        updated = transform(path, normalized)
        # Git patch lines must match the index (including its line endings).
        index = git('show', ':' + path).stdout.decode('utf-8')
        if index.replace('\r\n', '\n') != normalized:
            raise ValueError(f'Working tree and index differ: {path}')
        if '\r\n' in index:
            updated = updated.replace('\n', '\r\n')
        diff = ''.join(difflib.unified_diff(index.splitlines(keepends=True), updated.splitlines(keepends=True), fromfile='a/'+path, tofile='b/'+path))
        if not diff:
            raise ValueError(f'No changes for {path}')
        # Handle a final line without a newline in a unified diff.
        if not index.endswith('\n') or not updated.endswith('\n'):
            diff = ''.join(line if line.endswith('\n') else line+'\n\\ No newline at end of file\n' for line in difflib.unified_diff(index.splitlines(keepends=True), updated.splitlines(keepends=True), fromfile='a/'+path, tofile='b/'+path))
        patches.append(diff)
    # Search tracked first-party files for other callers before removing the API.
    listing = git('ls-files', '-z', 'main', 'meshcore', 'hardware', 'services', 'watch').stdout.decode('utf-8').split('\0')
    for path in listing:
        if path in paths or not path.endswith(('.cpp','.c','.h','.hpp','.cc','.cxx')) or '/generated/' in path:
            continue
        if re.search(r'\bprepare_storage\s*\(', (ROOT/path).read_text(encoding='utf-8', errors='replace')):
            raise ValueError(f'Additional prepare_storage reference: {path}')
    PATCH.write_bytes(''.join(patches).encode('utf-8'))
    git('apply', '--check', str(PATCH))
    git('apply', str(PATCH))
    print('Applied MeshCore initialization refactor to three files.')
    print('Patch retained:', PATCH.name)
    print('Review: git diff; build: idf.py build')
    print('Rollback (before further edits): git apply -R '+PATCH.name)


if __name__ == '__main__':
    try:
        main()
    except (ValueError, OSError, subprocess.CalledProcessError) as exc:
        print('Error:', exc, file=sys.stderr)
        if isinstance(exc, subprocess.CalledProcessError):
            print(exc.stderr.decode(errors='replace'), file=sys.stderr)
        sys.exit(1)
