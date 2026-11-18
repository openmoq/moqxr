#!/usr/bin/env node
// Independent MSF-02 catalog validation. Runs the publisher over the repository fixtures and checks
// the emitted catalog.json and timeline payloads against docs/draft-ietf-moq-msf-02.txt without using
// any of the publisher's own code. A second pass checks that --msf-version 01 keeps the MSF-01 shapes.
// Usage: bun scripts/test-msf02.mjs <publisher> [--keep]
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdtemp, readFile, readdir, rm } from 'node:fs/promises';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { fileURLToPath } from 'node:url';

const [publisherArg, option] = process.argv.slice(2);
if (!publisherArg || (option && option !== '--keep') || process.argv.length > 4) {
    console.error('Usage: bun scripts/test-msf02.mjs <publisher> [--keep]');
    process.exit(2);
}
const publisher = resolve(publisherArg);
const fixtures = fileURLToPath(new URL('../tests/fixtures/', import.meta.url));
const work = await mkdtemp(join(tmpdir(), 'moqxr-msf02-'));

// fixture -> packagings the publisher supports for it
const CASES = [
    ['loc-bframes-fragmented', ['cmaf', 'locmaf', 'loc']],
    ['locmaf-publisher', ['cmaf', 'locmaf', 'loc']],
    ['irregular-first-sample', ['cmaf', 'locmaf', 'loc']],
    ['loc-hevc-fragmented', ['cmaf', 'loc']],
    ['loc-av1-fragmented', ['cmaf', 'loc']],
    ['loc-vp9-fragmented', ['loc']],
    ['loc-opus', ['cmaf', 'loc']],
];
const ROOT_KEYS_02 = new Set(['version', 'generatedAt', 'isComplete', 'contentProtections', 'tracks', 'publishTracks', 'initDataList']);
const MSF02_TRACK_FIELDS = ['packagingMetadata', 'accessibleWindowTime', 'keyframeSpacing', 'connectionURI', 'parentName', 'parentNamespace'];

function run(args) {
    const result = spawnSync(publisher, args, { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
    if (result.error || result.status !== 0) throw new Error(`${args.join(' ')} failed: ${result.error ?? result.stderr}`);
}

// MSF-02 Table 6: fields required per (packaging, role) pair.
const TABLE6 = {
    'loc/audio': ['codec', 'bitrate', 'samplerate', 'channelConfig'],
    'loc/video': ['codec', 'bitrate', 'framerate'],
    'loc/audiodescription': ['codec', 'samplerate', 'channelConfig', 'lang'],
    'loc/signlanguage': ['codec', 'framerate'],
    'eventtimeline/eventtimeline': ['eventType'],
};

function validateNamespace(value, where) {
    assert.equal(typeof value, 'string', `${where}: namespace is a string`);
    const fields = value.split('/');
    assert.ok(fields.length >= 1 && fields.length <= 32, `${where}: 1-32 namespace fields`);
    assert.ok(fields.every(f => f.length > 0), `${where}: namespace fields are non-empty`);
}

function validate02(label, text, catalog) {
    for (const key of Object.keys(catalog)) assert.ok(ROOT_KEYS_02.has(key), `${label}: unexpected root field ${key}`);
    assert.equal(catalog.version, 'draft-02', `${label}: version`);
    assert.ok(Array.isArray(catalog.tracks) && catalog.tracks.length > 0, `${label}: tracks`);
    if (catalog.initDataList) assert.ok(text.indexOf('"initDataList"') > text.indexOf('"tracks"'), `${label}: initDataList follows tracks`);
    const names = new Set();
    const full = new Set();
    for (const track of catalog.tracks) {
        const where = `${label}/${track.name}`;
        assert.ok(typeof track.name === 'string' && track.name.length > 0, `${where}: name`);
        assert.equal(typeof track.packaging, 'string', `${where}: packaging`);
        assert.equal(typeof track.isLive, 'boolean', `${where}: isLive`);
        assert.equal(track.token, undefined, `${where}: token was removed from the track fields`);
        if (track.namespace !== undefined) validateNamespace(track.namespace, where);
        const id = `${track.namespace ?? ''}\u0000${track.name}`;
        assert.ok(!full.has(id), `${where}: Full Track Name is unique`);
        full.add(id);
        names.add(track.name);
        for (const field of TABLE6[`${track.packaging}/${track.role}`] ?? [])
            assert.notEqual(track[field], undefined, `${where}: Table 6 requires ${field}`);
        if (track.packaging === 'eventtimeline') assert.notEqual(track.eventType, undefined, `${where}: eventType`);
        if (track.packaging === 'mediatimeline') assert.ok(Array.isArray(track.depends) && track.depends.length > 0, `${where}: depends`);
    }
    const groups = new Set(catalog.tracks.filter(t => t.renderGroup !== undefined).map(t => t.renderGroup));
    const inits = new Map((catalog.initDataList ?? []).map(entry => [entry.id, entry]));
    assert.equal(inits.size, (catalog.initDataList ?? []).length, `${label}: initDataList ids are unique`);
    for (const track of catalog.tracks) {
        const where = `${label}/${track.name}`;
        for (const dependency of track.depends ?? []) {
            if (typeof dependency === 'string') assert.ok(names.has(dependency), `${where}: depends name ${dependency} resolves`);
            else assert.ok(groups.has(dependency), `${where}: depends renderGroup ${dependency} resolves`);
        }
        if (track.packaging === 'mediatimeline') {
            // Section 7.2: reference a renderGroup when every track of it is covered, never both forms.
            for (const group of groups) {
                const members = catalog.tracks.filter(t => t.renderGroup === group && t !== track).map(t => t.name);
                const named = members.filter(n => track.depends.includes(n));
                assert.ok(named.length < members.length || members.length === 0,
                    `${where}: a fully covered renderGroup ${group} must be referenced by number`);
                if (track.depends.includes(group)) assert.equal(named.length, 0, `${where}: renderGroup ${group} and its tracks are not both named`);
            }
        }
        if (track.initRef !== undefined) {
            const entry = inits.get(track.initRef);
            assert.ok(entry, `${where}: initRef resolves`);
            if (track.packaging === 'loc') {
                assert.equal(entry.type, 'object-property', `${where}: LOC init data is an object-property entry`);
                assert.equal(entry.data, track.role === 'video' ? '0x0D' : '0x0F', `${where}: property type names VIDEO_CONFIG / AUDIO_CONFIG`);
            } else {
                assert.equal(entry.type, 'inline', `${where}: ${track.packaging} init data stays inline`);
                assert.match(entry.data, /^[A-Za-z0-9+/]+=*$/, `${where}: Base64`);
            }
        } else if (track.packaging === 'loc') {
            assert.ok(/^vp09\./.test(track.codec), `${where}: only a codec without a description (VP9) has no initRef`);
        }
    }
    for (const entry of catalog.initDataList ?? []) {
        assert.ok(['inline', 'track-property', 'object-property'].includes(entry.type), `${label}: init type ${entry.type}`);
        if (entry.type !== 'inline') assert.match(entry.data, /^0x[0-9A-Fa-f]+$/, `${label}: property type in hex notation`);
    }
}

function validate01(label, catalog) {
    assert.equal(catalog.version, '1', `${label}: version`);
    for (const track of catalog.tracks) for (const field of MSF02_TRACK_FIELDS) assert.equal(track[field], undefined, `${label}/${track.name}: ${field} is MSF-02 only`);
    for (const entry of catalog.initDataList ?? []) assert.equal(entry.type, 'inline', `${label}: MSF-01 has only inline init data`);
    for (const track of catalog.tracks) for (const dependency of track.depends ?? []) assert.equal(typeof dependency, 'string', `${label}: names only`);
}

async function validateTimelines(label, dir, version) {
    for (const file of (await readdir(dir)).filter(f => /^(timeline|.*_sap)_g\d+_o\d+\.json$/.test(f))) {
        const payload = JSON.parse(await readFile(join(dir, file), 'utf8'));
        assert.ok(Array.isArray(payload), `${label}/${file}: array`);
        if (file.startsWith('timeline_')) {
            let previous = -1;
            for (const record of payload) {
                assert.ok(Array.isArray(record) && record.length === 3, `${label}/${file}: explicit entry [time, [group, object], wallclock]`);
                assert.ok(Number.isInteger(record[0]) && record[0] >= previous, `${label}/${file}: integral media times ordered by media time`);
                assert.ok(Array.isArray(record[1]) && record[1].length === 2, `${label}/${file}: location`);
                previous = record[0];
            }
        } else {
            for (const record of payload) {
                const indexKeys = Object.keys(record).filter(k => k !== 'data');
                assert.deepEqual(indexKeys, [version === '02' ? 'L' : 'l'], `${label}/${file}: one index key, uppercase in MSF-02`);
                assert.notEqual(record.data, undefined, `${label}/${file}: data field`);
            }
        }
    }
}

try {
    let checked = 0;
    for (const version of ['02', '01']) {
        for (const [fixture, packagings] of CASES) {
            for (const packaging of packagings) {
                const label = `${fixture}/${packaging}/msf${version}`;
                const dir = join(work, label.replaceAll('/', '_'));
                run(['--input', join(fixtures, `${fixture}.mp4`), '--draft', '18', '--packaging', packaging, '--sap', '--msf-timeline',
                    '--msf-version', version, '--emit-dir', dir]);
                const text = await readFile(join(dir, 'catalog.json'), 'utf8');
                const catalog = JSON.parse(text);
                if (version === '02') validate02(label, text, catalog);
                else validate01(label, catalog);
                await validateTimelines(label, dir, version);
                ++checked;
            }
        }
    }
    console.log(`MSF-02 PASS: ${checked} catalogs (MSF-02 and MSF-01) over ${CASES.length} fixtures; independent checks of the draft's field, Table 6, depends, initDataList and timeline rules.`);
    console.log('Offline artifact validation only; no relay interoperability claim.');
} finally {
    if (option === '--keep') console.log(`Artifacts: ${work}`);
    else await rm(work, { recursive: true, force: true });
}
