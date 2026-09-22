// Observe real Playa player or connection delivery, without claiming rendered frames.
import { X509Certificate } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { resolve } from 'node:path';
import { pathToFileURL } from 'node:url';

const [playa, modules, url, namespace, cert, token, timeout = '25', quicPackage = '', subscriberApi = 'player'] = process.argv.slice(2);
// Draft-18 USE_VALUE=3, token type=16: both integers have one-byte vi64 encodings.
const authTokens = [new Uint8Array(Buffer.concat([Buffer.from([3, 16]), readFileSync(token)]))];
const load = (pkg) => import(pathToFileURL(resolve(playa, `packages/${pkg}/dist/index.js`)));
const { MoqtConnection } = await load('webtransport');
const { MoqtPlayer, CatalogManager } = await load('player');
const { CmafAssembler } = await load('browser');
const states = new Map();
const initialized = new Set();
let finished = false;
let stopping = false;
let resolveDone;
let rejectDone;
const done = new Promise((resolve, reject) => { resolveDone = resolve; rejectDone = reject; });
done.catch(() => {});
const timer = setTimeout(() => rejectDone(new Error('catalog/media deadline exceeded')), Number(timeout) * 1000);
const config = {
  url, namespace: namespace.split('/'), authTokens, draftVersion: 18, catalogBootstrap: 'subscribe',
  // This fixture has its own finite init/media deadline and never renders frames.
  cmafBootstrapTimeoutMs: 0,
  onQlogEvent: process.env.CAT4MOQ_DEBUG ? (event) => {
    if (event.type.startsWith('subgroup_'))
      console.log(JSON.stringify(event, (_, value) => typeof value === 'bigint' ? String(value) : value));
    if (event.type === 'control_message_parsed')
      console.log(JSON.stringify({ type: event.type, message: event.message.type,
        requestId: event.message.requestId, trackAlias: event.message.trackAlias },
        (_, value) => typeof value === 'bigint' ? String(value) : value));
  } : undefined,
  logLevel: process.env.CAT4MOQ_DEBUG ? 'debug' : 'none',
  createCmafAssembler: (options) => new CmafAssembler(options),
  // Exercise the player's real init resolution without claiming rendered frames.
  // Omitting this adapter leaves its CMAF bootstrap state uninitialized.
  createMediaSource: () => ({
    mediaElement: null, onFirstFrame: null, onError: null, onStall: null,
    initialize(config) {
      for (const kind of ['video', 'audio']) {
        const entry = config[kind];
        if (!entry?.codec || !entry.initData?.length) throw new Error(`missing ${kind} initialization`);
        const data = Buffer.from(entry.initData);
        const boxes = new Set();
        for (let offset = 0; offset < data.length;) {
          if (data.length - offset < 8) throw new Error('truncated initialization box');
          const size = data.readUInt32BE(offset);
          if (size < 8 || offset + size > data.length) throw new Error('invalid initialization box');
          boxes.add(data.toString('ascii', offset + 4, offset + 8));
          offset += size;
        }
        if (!boxes.has('ftyp') || !boxes.has('moov')) throw new Error(`invalid ${kind} CMAF initialization`);
        initialized.add(kind);
        console.log(JSON.stringify({ event: 'init', kind, bytes: data.length }));
      }
      return true;
    },
    appendChunk(kind) {
      if (!initialized.has(kind)) throw new Error(`media before ${kind} initialization`);
    },
    reset() {}, destroy() {},
  }),
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
};
const player = subscriberApi === 'player' ? new MoqtPlayer(config) : null;
const connection = player ? null : new MoqtConnection(18);
player?.on('session_established', () => console.log(JSON.stringify({ event: 'setup_accepted' })));
player?.on('error', ({ error }) => {
  console.error(`${error.severity}: ${error.message}`);
  if (error.severity === 'fatal') rejectDone(error);
});
function observeCatalog({ catalog }) {
  try {
    for (const kind of ['video', 'audio']) {
      const track = catalog.tracks.find((t) => t.role === kind);
      if (!track || track.packaging !== 'cmaf') throw new Error(`catalog missing CMAF ${kind}`);
      const previous = states.get(track.name);
      if (previous && previous.kind !== kind) throw new Error('catalog changed track role');
      if (!previous) states.set(track.name, { kind, track: track.name, groups: new Set(), boxes: new Set(), bytes: 0, objects: 0 });
    }
    console.log(JSON.stringify({ event: 'catalog', tracks: [...states.keys()],
      initializations: catalog.initDataList?.length ?? 0,
      trackInit: catalog.tracks.map((t) => ({ name: t.name, initRef: t.initRef, inline: Boolean(t.initData) })),
    }));
  } catch (error) { rejectDone(error); }
}
function observeMedia(obj) {
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
    if (initialized.size === 2 && states.size === 2 &&
        [...states.values()].every((t) => t.groups.size >= 2 && t.boxes.has('moof'))) resolveDone();
  } catch (error) { rejectDone(error); }
}
player?.on('catalog_received', observeCatalog);
player?.on('media_object', observeMedia);
async function runConnection() {
  connection.onError = rejectDone;
  connection.onClose = (code) => { if (!finished) rejectDone(new Error(`subscriber closed: ${code}`)); };
  connection.onQlogEvent = config.onQlogEvent;
  await connection.connect(await config.createTransport(url), { authTokens });
  console.log(JSON.stringify({ event: 'setup_accepted' }));
  const enc = new TextEncoder();
  const ns = namespace.split('/').map((part) => enc.encode(part));
  const manager = new CatalogManager(namespace);
  let subscribed = false;
  const catalogOptions = {
    onObject: (object) => {
      if (!object.payload?.length) return;
      try {
        const catalog = manager.processCatalogObject(object.payload);
        observeCatalog({ catalog });
        if (subscribed) return;
        subscribed = true;
        const tracks = ['video', 'audio'].map((kind) => catalog.tracks.find((t) => t.role === kind));
        const init = {};
        for (const track of tracks) {
          if (!track) throw new Error('catalog missing media track');
          const entry = catalog.initDataList?.find((item) => item.id === track.initRef);
          const encoded = track.initData ?? (entry?.type === 'inline' ? entry.data : undefined);
          if (!encoded) throw new Error(`missing inline initialization for ${track.name}`);
          init[track.role] = { codec: track.codec,
            initData: Uint8Array.from(atob(encoded), (c) => c.charCodeAt(0)) };
        }
        config.createMediaSource().initialize(init);
        // Resolved aliases are owned by each TrackSubscription; no provisional
        // player alias map is involved. This is transport delivery coverage.
        Promise.all(tracks.map((track) => connection.subscribeTrack(ns, enc.encode(track.name), {
          filter: { type: 'NextGroupStart' },
          onObject: (object) => observeMedia({ ...object, trackName: track.name }),
        }))).catch(rejectDone);
      } catch (error) { rejectDone(error); }
    },
  };
  let reportedAbsent = false;
  while (!stopping) {
    try {
      await connection.subscribeTrack(ns, enc.encode('catalog'), catalogOptions);
      return;
    } catch (error) {
      // A denied publisher may leave no catalog to subscribe to. Keep checking
      // for the whole observation interval; never turn authorization/transport
      // errors into evidence that the publisher was denied.
      if (!/^Subscribe failed: .* \(16\)$/.test(error.message ?? '')) throw error;
      if (!reportedAbsent) console.log(JSON.stringify({ event: 'catalog_absent', code: 16 }));
      reportedAbsent = true;
      await new Promise((resolve) => setTimeout(resolve, 250));
    }
  }
}
try {
  if (player) player.load().then(() => player.play()).catch(rejectDone);
  else runConnection().catch(rejectDone);
  await done;
  for (const t of states.values()) console.log(JSON.stringify({
    event: 'media', ...t, groups: [...t.groups], boxes: [...t.boxes],
  }));
  finished = true;
  console.log('PASS: Playa received catalog and progressing audio/video CMAF groups');
} catch (error) {
  console.error(`FAIL: ${error.stack ?? error}`);
} finally {
  stopping = true;
  clearTimeout(timer);
  // Bound teardown even if a native transport worker fails to terminate.
  const shutdown = setTimeout(() => process.exit(finished ? 0 : 1), 2000);
  try { if (player) await player.destroy(); else await connection.close(); } catch {}
  clearTimeout(shutdown);
  process.exit(finished ? 0 : 1);
}
