#!/usr/bin/env bun
// Opt-in interoperability test; requires Bun, ffmpeg/libx264, ffprobe, and a
// moq-playa checkout containing packages/locmaf/src/track-decoder.ts.
// Usage: bun scripts/test-locmaf-playa.mjs <publisher> <moq-playa> [--keep]
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [publisherArg, playaArg, option] = process.argv.slice(2);
if (!publisherArg || !playaArg || (option && option !== '--keep') || process.argv.length > 5) {
    console.error('Usage: bun scripts/test-locmaf-playa.mjs <publisher> <moq-playa-checkout> [--keep]');
    process.exit(2);
}
if (!globalThis.Bun) throw new Error('Run this script with Bun to load the Playa TypeScript sources directly.');
const publisher = resolve(publisherArg);
const { LocmafTrackDecoder } = await import(pathToFileURL(join(resolve(playaArg), 'packages/locmaf/src/track-decoder.ts')).href);
const work = await mkdtemp(join(tmpdir(), 'moqxr-locmaf-playa-'));

function run(command, args) {
    const result = spawnSync(command, args, { encoding: 'utf8', maxBuffer: 32 * 1024 * 1024 });
    if (result.error || result.status !== 0) {
        throw new Error(`${command} ${args.join(' ')} failed: ${result.error ?? result.stderr ?? result.signal}`);
    }
    return result.stdout;
}

function mdat(bytes) {
    const payloads = [];
    for (let offset = 0; offset < bytes.length;) {
        assert.ok(offset + 8 <= bytes.length, 'truncated BMFF header');
        const size = bytes.readUInt32BE(offset);
        assert.ok(size >= 8 && offset + size <= bytes.length, 'invalid BMFF size');
        if (bytes.toString('ascii', offset + 4, offset + 8) === 'mdat') payloads.push(bytes.subarray(offset + 8, offset + size));
        offset += size;
    }
    assert.equal(payloads.length, 1, 'expected one mdat per CMAF chunk');
    return payloads[0];
}

function packets(file) {
    return JSON.parse(run('ffprobe', ['-v', 'error', '-show_packets', '-show_data_hash', 'sha256',
        '-show_entries', 'packet=pts,dts,duration,size,flags,data_hash', '-of', 'json', file])).packets;
}

function decodedFrames(file) {
    return run('ffmpeg', ['-v', 'error', '-xerror', '-i', file, '-map', '0:0', '-f', 'framemd5', '-'])
        .split('\n').filter(line => line && !line.startsWith('#'));
}

try {
    const source = join(work, 'source.mp4');
    run('ffmpeg', ['-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=30',
        '-f', 'lavfi', '-i', 'sine=frequency=997:sample_rate=48000', '-t', '3',
        '-c:v', 'libx264', '-threads', '1', '-preset', 'fast', '-g', '30', '-keyint_min', '30',
        '-sc_threshold', '0', '-bf', '2', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '96k',
        '-movflags', '+frag_keyframe+empty_moov+default_base_moof+separate_moof', source]);
    const cmaf = join(work, 'cmaf');
    const locmaf = join(work, 'locmaf');
    run(publisher, ['--input', source, '--emit-dir', cmaf]);
    run(publisher, ['--input', source, '--packaging', 'locmaf', '--emit-dir', locmaf]);
    const catalog = JSON.parse(await readFile(join(locmaf, 'catalog.json'), 'utf8'));
    const files = await readdir(locmaf);
    const mediaObjects = (names, extension) => names.flatMap(file => {
        const match = /^(.*)_g(\d+)_o(\d+)_media\.(locmafobj|mp4)$/.exec(file);
        if (match?.[4] !== extension) return [];
        return match ? [{ file, track: match[1], group: BigInt(match[2]), object: BigInt(match[3]) }] : [];
    });
    const objects = mediaObjects(files, 'locmafobj');
    const baselineObjects = mediaObjects(await readdir(cmaf), 'mp4');
    const orderedTrack = (list, track) => list.filter(object => object.track === track).sort((a, b) =>
        a.group !== b.group ? (a.group < b.group ? -1 : 1) : a.object === b.object ? 0 : a.object < b.object ? -1 : 1);
    assert.ok(objects.length > 0, 'publisher emitted no LOCMAF media objects');
    assert.ok(!files.some(file => /_(?:media|probe)\.mp4$/.test(file)), 'LOCMAF output contains misleading MP4 media/probe files');
    const tracks = [...new Set(objects.map(object => object.track))];
    assert.equal(tracks.length, 2, 'expected both audio and video tracks');
    let objectCount = 0;
    let sampleCount = 0;
    let cmafBytes = 0;
    let locmafBytes = 0;
    for (const track of tracks) {
        const entry = catalog.tracks.find(value => value.name === track);
        assert.equal(entry?.packaging, 'locmaf', `${track}: catalog packaging`);
        assert.equal(entry?.locmafVersion, '0.3', `${track}: catalog LOCMAF version`);
        const init = await readFile(join(locmaf, `${track}_init.mp4`));
        assert.deepEqual(init, await readFile(join(cmaf, `${track}_init.mp4`)), `${track}: unchanged init`);
        const decoder = new LocmafTrackDecoder(init);
        const baseline = [init];
        const reconstructed = [init];
        const baselinePayloads = [];
        const reconstructedPayloads = [];
        for (const object of orderedTrack(baselineObjects, track)) {
            const original = await readFile(join(cmaf, object.file));
            baseline.push(original);
            baselinePayloads.push(mdat(original));
            cmafBytes += original.length;
        }
        const trackObjects = orderedTrack(objects, track);
        for (const object of trackObjects) {
            const encoded = await readFile(join(locmaf, object.file));
            const decoded = decoder.push(object.group, object.object, encoded);
            assert.equal(decoded.kind, 'chunk', `${object.file}: ${decoded.error?.message ?? decoded.kind}`);
            assert.ok(decoded.sampleCount > 0, `${object.file}: empty samples`);
            reconstructedPayloads.push(Buffer.from(decoded.mdat));
            reconstructed.push(Buffer.from(decoded.bytes));
            ++objectCount;
            sampleCount += decoded.sampleCount;
            locmafBytes += encoded.length;
        }
        assert.deepEqual(Buffer.concat(reconstructedPayloads), Buffer.concat(baselinePayloads), `${track}: sample bytes`);
        const baselineFile = join(work, `${track}-cmaf.mp4`);
        const reconstructedFile = join(work, `${track}-reconstructed.mp4`);
        await writeFile(baselineFile, Buffer.concat(baseline));
        await writeFile(reconstructedFile, Buffer.concat(reconstructed));
        const originalPackets = packets(baselineFile);
        assert.ok(originalPackets.length > 0, `${track}: ffprobe found no packets`);
        assert.deepEqual(packets(reconstructedFile), originalPackets, `${track}: packet payload hashes/timestamps/durations/flags`);
        const frames = decodedFrames(baselineFile);
        assert.ok(frames.length > 0, `${track}: ffmpeg decoded no frames`);
        assert.deepEqual(decodedFrames(reconstructedFile), frames, `${track}: decoded frame hashes/timing`);
        console.log(`${track}: ${trackObjects.length} objects, ${originalPackets.length} packets, ${frames.length} decoded frames passed`);
    }
    console.log(`PASS: Playa reconstructed ${objectCount} LOCMAF objects (${sampleCount} samples); media bytes CMAF=${cmafBytes}, LOCMAF=${locmafBytes}`);
    if (option === '--keep') console.log(`Artifacts: ${work}`);
    else await rm(work, { recursive: true, force: true });
} catch (error) {
    console.error(`Artifacts retained for diagnosis: ${work}`);
    throw error;
}
