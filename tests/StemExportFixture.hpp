// SPDX-License-Identifier: AGPL-3.0-or-later
#pragma once

#include "library/PreparedTrackStore.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>
#include <stdexcept>

namespace conflux::test {
inline void writeFixtureBytes(const QString& path, const QByteArray& bytes)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(bytes) != bytes.size() || !file.flush())
        throw std::runtime_error("Cannot write generated stem exporter fixture");
}

// Real local subprocess, with bounded 4096-frame raw conversion blocks. This
// exercises store/process/controller behavior; it performs no model inference.
inline library::StemEngine makeStemEngineFixture(const QString& directory, const QByteArray& mode = "good")
{
    library::StemEngine result;
    result.pythonExecutable = QFileInfo(QStandardPaths::findExecutable("python3")).canonicalFilePath();
    result.exporterPath = QDir(directory).filePath("exporter.py");
    result.workerPath = QDir(directory).filePath("worker.py");
    result.runtimePath = QDir(directory).filePath("runtime.bin");
    result.modelPath = QDir(directory).filePath("model.bin");
    result.maxSeconds = 5;
    result.requestTimeout = 1;
    writeFixtureBytes(result.workerPath, "worker identity fixture\n");
    writeFixtureBytes(result.runtimePath, "runtime identity fixture\n");
    writeFixtureBytes(result.modelPath, "model identity fixture\n");
    QByteArray script = "MODE = '" + mode + "'\n";
    script += R"PY(import argparse, hashlib, json, math, os, pathlib, signal, struct, sys, time
P = pathlib.Path(__file__)
NAMES = ['drums', 'bass', 'other', 'vocals']
def identity(path):
    digest = hashlib.sha256()
    size = 0
    with pathlib.Path(path).open('rb') as source:
        while block := source.read(65536):
            digest.update(block); size += len(block)
    return {'sha256': digest.hexdigest(), 'bytes': size}
def describe():
    return {'schema': 1, 'helper_sha256': identity(P)['sha256'],
            'worker_sha256': identity(P.with_name('worker.py'))['sha256'], 'numpy_version': 'fixture-1',
            'input': {'format': 'f32le', 'sample_rate': 44100, 'channels': 2},
            'window_frames': 343980, 'hop_frames': 257985, 'overlap_frames': 85995,
            'normalization': 'track-mono-sample-variance-f32-v1',
            'overlap_add': 'fixed-grid-triangle-f32-v1'}
if sys.argv[1:] == ['--describe']:
    P.with_suffix('.prefix').write_text(sys.prefix)
    print(json.dumps(describe())); sys.exit(0)
a = argparse.ArgumentParser()
for key in ['input', 'destination', 'runtime', 'model', 'backend']:
    a.add_argument('--' + key, required=True)
for key in ['threads', 'max-input-bytes', 'max-output-bytes', 'max-job-bytes', 'max-diagnostic-bytes']:
    a.add_argument('--' + key, type=int, required=True)
for key in ['max-seconds', 'request-timeout']:
    a.add_argument('--' + key, type=float, required=True)
v = a.parse_args()
with P.with_suffix('.calls').open('ab') as calls: calls.write(b'x')
P.with_suffix('.started').write_text(str(os.getpid()))
if MODE == 'flood':
    sys.stdout.buffer.write(b'x' * 131072); sys.stdout.buffer.flush(); time.sleep(60)
if MODE == 'block':
    time.sleep(60)
input_bytes = pathlib.Path(v.input).stat().st_size
frames = input_bytes // 8
destination = pathlib.Path(v.destination)
stage = destination.with_name(destination.name + '.stage')
stage.mkdir(mode=0o700)
stems = []
for index, name in enumerate(NAMES):
    leaf = stage / (name + '.f32')
    digest = hashlib.sha256()
    size = 0
    with pathlib.Path(v.input).open('rb') as source, leaf.open('xb') as target:
        while block := source.read(4096 * 8):
            values = struct.unpack('<' + 'f' * (len(block) // 4), block)
            samples = [sample * (index + 1) / 4 for sample in values]
            if MODE == 'nonfinite' and index == 2 and source.tell() == input_bytes:
                samples[-1] = float('nan')
            data = struct.pack('<' + 'f' * len(samples), *samples)
            target.write(data); digest.update(data); size += len(data)
    if MODE == 'truncate' and index == 2:
        with leaf.open('r+b') as target: target.truncate(size - 4)
        value = identity(leaf); size = value['bytes']; digest_hex = value['sha256']
    else:
        digest_hex = digest.hexdigest()
    stems.append({'name': name, 'file': name + '.f32', 'frames': frames,
                  'bytes': size, 'sha256': digest_hex})
    if MODE == 'partial' and index == 1: sys.exit(3)
d = describe()
manifest = {'schema': 1, 'describe': d,
    'source': dict(identity(v.input), format='f32le', sample_rate=44100, channels=2, frames=frames),
    'model': identity(v.model),
    'runtime': dict(identity(v.runtime), ready={'sample_rate': 44100, 'channels': 2,
        'segment_frames': 343980, 'stems': NAMES, 'backend': v.backend, 'threads': v.threads,
        'revision': 'a' * 40, 'weight_storage': 'native', 'fence_wait_mode': 'blocking'}),
    'separation': {'worker_sha256': d['worker_sha256'], 'numpy_version': d['numpy_version'],
        'mean': 0.0, 'std': 1.0, 'window_frames': 343980, 'hop_frames': 257985,
        'overlap_frames': 85995, 'normalization': d['normalization'], 'overlap_add': d['overlap_add']},
    'stems': stems,
    'limits': {'max_input_bytes': v.max_input_bytes, 'max_output_bytes': v.max_output_bytes,
        'max_job_bytes': v.max_job_bytes, 'max_seconds': v.max_seconds,
        'request_timeout': v.request_timeout, 'max_diagnostic_bytes': v.max_diagnostic_bytes},
    'reservation_bytes': 5 * input_bytes + 13759200 + v.max_diagnostic_bytes + 16384}
if MODE == 'wrong-order': manifest['stems'].reverse()
if MODE == 'wrong-model': manifest['model']['sha256'] = '0' * 64
if MODE == 'wrong-describe': manifest['describe'] = dict(d, numpy_version='changed')
if MODE == 'changed-source':
    with P.with_name('source.wav').open('r+b') as original:
        original.seek(44); original.write(struct.pack('<f', 0.625))
(stage / 'manifest.json').write_text(json.dumps(manifest))
stage.rename(destination)
)PY";
    writeFixtureBytes(result.exporterPath, script);
    return result;
}
} // namespace conflux::test
