#!/usr/bin/env node
// Independent LOC-04 inspection/decoder validation. Requires FFmpeg/libx264,
// ffprobe and Node or Bun. This tests emitted artifacts, not network delivery.
// Usage: node scripts/test-loc04.mjs <publisher> [--keep]
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';

const [publisherArg, option] = process.argv.slice(2);
if (!publisherArg || (option && option !== '--keep') || process.argv.length > 4) {
    console.error('Usage: node scripts/test-loc04.mjs <publisher> [--keep]');
    process.exit(2);
}
const publisher = resolve(publisherArg);
const work = await mkdtemp(join(tmpdir(), 'moqxr-loc04-'));
function run(command, args) {
    const result = spawnSync(command, args, { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
    if (result.error || result.status !== 0)
        throw new Error(`${command} ${args.join(' ')} failed: ${result.error ?? result.stderr ?? result.signal}`);
    return result.stdout;
}
function hexDump(text) {
    return Buffer.from((text ?? '').split('\n').flatMap(line => {
        const match = /^\s*[0-9a-f]+:\s(.+?)(?:  |$)/i.exec(line);
        return match ? [match[1].replaceAll(' ', '')] : [];
    }).join(''), 'hex');
}
function properties(hex) {
    assert.match(hex, /^(?:[0-9a-f]{2})*$/i);
    const bytes = Buffer.from(hex, 'hex');
    let offset = 0;
    const vi64 = () => {
        assert.ok(offset < bytes.length, 'truncated vi64');
        const first = bytes[offset++];
        let extra = 0;
        for (let mask = 128; mask && (first & mask); mask >>= 1) ++extra;
        assert.ok(offset + extra <= bytes.length, 'truncated vi64 continuation');
        let value = BigInt(extra === 8 ? 0 : first & (127 >> extra));
        for (let i = 0; i < extra; ++i) value = (value << 8n) | BigInt(bytes[offset++]);
        return value;
    };
    let id = 0n;
    const result = [];
    while (offset < bytes.length) {
        id += vi64();
        assert.ok(!result.length || id > BigInt(result.at(-1).id), 'properties must be ordered and unique');
        if (id & 1n) {
            const size = Number(vi64());
            assert.ok(size <= 65535 && offset + size <= bytes.length, 'invalid property length');
            result.push({ id: String(id), hex: bytes.subarray(offset, offset + size).toString('hex') });
            offset += size;
        } else result.push({ id: String(id), value: String(vi64()) });
    }
    return result;
}
function u32(value) { const b = Buffer.alloc(4); b.writeUInt32BE(Number(value)); return b; }
function i32(value) { const b = Buffer.alloc(4); b.writeInt32BE(Number(value)); return b; }
function u64(value) { const b = Buffer.alloc(8); b.writeBigUInt64BE(BigInt(value)); return b; }
function box(type, ...parts) {
    const payload = Buffer.concat(parts);
    return Buffer.concat([u32(payload.length + 8), Buffer.from(type), payload]);
}
function fragment(id, sequence, packet, pts, payload) {
    // LOC carries PTS but no DTS/duration: use independently checked source
    // DTS/duration for this decoder harness, and LOC PTS for the signed CTO.
    const tfhd = box('tfhd', u32(0x020000), u32(id));
    const tfdt = box('tfdt', u32(0x01000000), u64(packet.dts));
    const flags = packet.flags.includes('K') ? 0x02000000 : 0x01010000;
    const trun = offset => box('trun', u32(0x01000f01), u32(1), u32(offset),
        u32(packet.duration), u32(payload.length), u32(flags), i32(pts - BigInt(packet.dts)));
    const moof = offset => box('moof', box('mfhd', u32(0), u32(sequence)), box('traf', tfhd, tfdt, trun(offset)));
    return Buffer.concat([moof(moof(0).length + 8), box('mdat', payload)]);
}
function frameHashes(file, selector) {
    return run('ffmpeg', ['-v', 'error', '-xerror', '-i', file, '-map', selector, '-f', 'framemd5', '-'])
        .split('\n').filter(line => line && !line.startsWith('#')).map(line => line.split(',').slice(4).join(',').trim());
}
try {
    const source = join(work, 'source.mp4');
    run('ffmpeg', ['-v', 'error', '-y', '-f', 'lavfi', '-i', 'testsrc2=size=320x180:rate=25',
        '-f', 'lavfi', '-i', 'sine=frequency=997:sample_rate=48000', '-t', '3',
        '-c:v', 'libx264', '-threads', '1', '-preset', 'fast', '-g', '25', '-keyint_min', '25',
        '-sc_threshold', '0', '-bf', '2', '-pix_fmt', 'yuv420p', '-c:a', 'aac', '-b:a', '96k',
        '-movflags', '+frag_keyframe+empty_moov+default_base_moof+separate_moof', source]);
    const probe = JSON.parse(run('ffprobe', ['-v', 'error', '-show_streams', '-show_packets', '-show_data', '-of', 'json', source]));
    assert.ok(probe.packets.some(p => p.pts !== p.dts), 'fixture must exercise reordered B-frame PTS');
    const loc = join(work, 'loc');
    const cmaf = join(work, 'cmaf');
    run(publisher, ['--input', source, '--draft', '18', '--packaging', 'loc', '--emit-dir', loc]);
    run(publisher, ['--input', source, '--draft', '18', '--packaging', 'cmaf', '--emit-dir', cmaf]);
    const catalog = JSON.parse(await readFile(join(loc, 'catalog.json'), 'utf8'));
    const files = await readdir(loc);
    assert.ok(!files.some(file => /_(?:media|probe|init)\.mp4$/.test(file)), 'LOC artifacts must not masquerade as MP4');
    const objects = files.flatMap(file => {
        const match = /^(.*)_g(\d+)_o(\d+)_media\.loc$/.exec(file);
        return match ? [{ file, track: match[1], group: BigInt(match[2]), object: BigInt(match[3]) }] : [];
    });
    assert.equal(new Set(objects.map(o => o.track)).size, 2, 'expected audio and video media');
    let total = 0;
    for (const stream of probe.streams) {
        const entry = catalog.tracks.find(t => t.role === stream.codec_type);
        assert.ok(entry, `missing ${stream.codec_type} catalog track`);
        assert.equal(entry.packaging, 'loc');
        assert.equal(entry.locmafVersion, undefined);
        const extradata = hexDump(stream.extradata);
        assert.ok(extradata.length, 'fixture codec extradata missing');
        const initEntry = catalog.initDataList.find(i => i.id === entry.initRef);
        assert.deepEqual(Buffer.from(initEntry.data, 'base64'), extradata, 'LOC catalog must carry codec extradata');
        const samples = probe.packets.filter(p => p.stream_index === stream.index);
        const ordered = objects.filter(o => o.track === entry.name).sort((a, b) =>
            a.group !== b.group ? (a.group < b.group ? -1 : 1) : a.object < b.object ? -1 : a.object > b.object ? 1 : 0);
        assert.equal(ordered.length, samples.length, `${entry.name}: one LOC object per source sample`);
        const [numerator, denominator] = stream.time_base.split('/').map(BigInt);
        assert.equal(numerator, 1n);
        const reconstructed = [await readFile(join(cmaf, `${entry.name}_init.mp4`))];
        let previousGroup = -1n;
        for (let i = 0; i < ordered.length; ++i) {
            const object = ordered[i];
            const sample = samples[i];
            const payload = await readFile(join(loc, object.file));
            assert.deepEqual(payload, hexDump(sample.data), `${object.file}: source bytes`);
            const sidecar = JSON.parse(await readFile(join(loc, object.file.replace('_media.loc', '_properties.json')), 'utf8'));
            const decoded = properties(sidecar.encoded);
            assert.deepEqual(decoded, sidecar.properties, `${object.file}: independent property parsing`);
            const values = new Map(decoded.map(p => [p.id, p.value === undefined ? Buffer.from(p.hex, 'hex') : BigInt(p.value)]));
            assert.equal(values.get('8'), denominator, 'LOC timescale');
            assert.equal(values.get('16'), BigInt(sample.pts), 'LOC presentation timestamp');
            assert.equal(BigInt(sidecar.decodeTimeUs), BigInt(sample.dts) * 1000000n / denominator, 'inspection DTS');
            assert.equal(BigInt(sidecar.durationUs), BigInt(sample.duration) * 1000000n / denominator, 'inspection duration');
            const video = stream.codec_type === 'video';
            const config = values.get(video ? '13' : '15');
            if (config || (video ? sample.flags.includes('K') : object.group !== previousGroup))
                assert.deepEqual(config, extradata, 'random-access codec configuration');
            if (video) {
                const marking = values.get('9');
                assert.ok(Buffer.isBuffer(marking) && marking.length === 1, 'single-layer frame marking bytes');
                assert.equal(Boolean(marking[0] & 0x20), sample.flags.includes('K'), 'independence bit');
                assert.equal(marking[0] & 0xc0, 0xc0, 'complete-frame start/end bits');
                if (object.group !== previousGroup) assert.ok(sample.flags.includes('K'), 'video group starts at random access');
            }
            if (object.group !== previousGroup) assert.equal(object.object, 0n, 'group begins at object zero');
            else assert.equal(object.object, ordered[i - 1].object + 1n, 'contiguous object IDs');
            previousGroup = object.group;
            reconstructed.push(fragment(Number.parseInt(stream.id), i + 1, sample, values.get('16'), payload));
            ++total;
        }
        const rebuilt = join(work, `${entry.name}-reconstructed.mp4`);
        await writeFile(rebuilt, Buffer.concat(reconstructed));
        assert.deepEqual(frameHashes(rebuilt, '0:0'), frameHashes(source, `0:${stream.index}`), `${entry.name}: decoded frames`);
    }
    console.log(`LOC-04 PASS: ${total} samples; independent VI64 properties, exact payload/PTS/config, B-frame ordering, and FFmpeg decoded frames.`);
    console.log('Inspection-artifact validation only; no network receiver interoperability claim.');
} finally {
    if (option === '--keep') console.log(`Artifacts: ${work}`);
    else await rm(work, { recursive: true, force: true });
}
