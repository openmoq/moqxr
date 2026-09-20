// Observe media delivered by Playa's real player, without a headless decoder.
import { X509Certificate } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [playa, modules, url, namespace, cert, token, timeout = '25', quicPackage = ''] = process.argv.slice(2);
// Draft-18 USE_VALUE=3, token type=16: both integers have one-byte vi64 encodings.
const authTokens = [new Uint8Array(Buffer.concat([Buffer.from([3, 16]), readFileSync(token)]))];
const load = (pkg) => import(pathToFileURL(resolve(playa, `packages/${pkg}/dist/index.js`)));
const { MoqtConnection } = await load('webtransport');
const { MoqtPlayer } = await load('player');
const states = new Map();
let finished = false;
let resolveDone;
let rejectDone;
const done = new Promise((resolve, reject) => { resolveDone = resolve; rejectDone = reject; });
done.catch(() => {});
const timer = setTimeout(() => rejectDone(new Error('catalog/media deadline exceeded')), Number(timeout) * 1000);
const player = new MoqtPlayer({
  url, namespace: namespace.split('/'), authTokens, draftVersion: 18, catalogBootstrap: 'subscribe',
  createConnection: () => new MoqtConnection(18),
  createTransport: async (endpoint) => {
    if (quicPackage) {
      const { connectQuic } = await import(pathToFileURL(resolve(quicPackage)));
      return connectQuic(endpoint, { ca: readFileSync(cert) });
    }
    const webtransport = resolve(modules, 'node_modules/@fails-components/webtransport');
    const manifest = JSON.parse(readFileSync(resolve(webtransport, 'package.json'), 'utf8'));
    const { WebTransport, quicheLoaded } = await import(pathToFileURL(resolve(webtransport, manifest.exports['.'].node.import)));
    await quicheLoaded;
    const hash = new X509Certificate(readFileSync(cert)).fingerprint256.replaceAll(':', '');
    const transport = new WebTransport(endpoint, {
      protocols: ['moqt-18'],
      serverCertificateHashes: [{ algorithm: 'sha-256', value: Buffer.from(hash, 'hex') }],
    });
    await transport.ready;
    return transport;
  },
});
player.on('session_established', () => console.log(JSON.stringify({ event: 'setup_accepted' })));
player.on('error', ({ error }) => {
  console.error(`${error.severity}: ${error.message}`);
  if (error.severity === 'fatal') rejectDone(error);
});
player.on('catalog_received', ({ catalog }) => {
  try {
    for (const kind of ['video', 'audio']) {
      const track = catalog.tracks.find((t) => t.role === kind);
      if (!track || track.packaging !== 'cmaf') throw new Error(`catalog missing CMAF ${kind}`);
      const previous = states.get(track.name);
      if (previous && previous.kind !== kind) throw new Error('catalog changed track role');
      if (!previous) states.set(track.name, { kind, track: track.name, groups: new Set(), boxes: new Set(), bytes: 0, objects: 0 });
    }
    console.log(JSON.stringify({ event: 'catalog', tracks: [...states.keys()] }));
  } catch (error) { rejectDone(error); }
});
player.on('media_object', (obj) => {
  if (!obj.payload?.length) return;
  console.log(JSON.stringify({ event: 'payload', track: obj.trackName, bytes: obj.payload.length }));
  try {
    const state = states.get(obj.trackName);
    if (!state) throw new Error(`media for unadvertised track ${obj.trackName}`);
    const data = Buffer.from(obj.payload);
    let offset = 0;
    let media = false;
    while (offset < data.length) {
      if (data.length - offset < 8) throw new Error('truncated CMAF box');
      const size = data.readUInt32BE(offset);
      if (size < 8 || offset + size > data.length) throw new Error('invalid CMAF box size');
      const type = data.toString('ascii', offset + 4, offset + 8);
      state.boxes.add(type);
      media ||= type === 'mdat' && size > 8;
      offset += size;
    }
    if (media) state.groups.add(String(obj.groupId));
    state.bytes += data.length;
    state.objects++;
    if (states.size === 2 && [...states.values()].every((t) => t.groups.size >= 2 && t.boxes.has('moof'))) resolveDone();
  } catch (error) { rejectDone(error); }
});
try {
  player.load().then(() => player.play()).catch(rejectDone);
  await done;
  for (const t of states.values()) console.log(JSON.stringify({
    event: 'media', ...t, groups: [...t.groups], boxes: [...t.boxes],
  }));
  finished = true;
  console.log('PASS: Playa received catalog and progressing audio/video CMAF groups');
} catch (error) {
  console.error(`FAIL: ${error.stack ?? error}`);
} finally {
  clearTimeout(timer);
  // Bound teardown even if a native transport worker fails to terminate.
  const shutdown = setTimeout(() => process.exit(finished ? 0 : 1), 2000);
  try { await player.destroy(); } catch {}
  clearTimeout(shutdown);
  process.exit(finished ? 0 : 1);
}
