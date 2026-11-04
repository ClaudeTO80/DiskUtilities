'use strict';

/*
 * DiskUtilities frontend.
 * The demo data below is used only when the native WebUI bridge is absent.
 * No operation in this UI deletes, moves, renames, or edits filesystem entries.
 */

// WebUI C++ bindings return JsonCpp JSON as a string. This adapter parses it once and
// exposes the transport-neutral Promise-based API used by the UI.
//
// WebUI creates `window.webui` inside its own DOMContentLoaded handler, which runs *after*
// this (deferred) script has been evaluated. The adapter therefore cannot be installed at
// load time: installNativeBridge() waits for it from our own DOMContentLoaded handler.
function installNativeBridge() {
  // The WebSocket opens asynchronously after the bridge object exists: calls made before
  // that fail with "WebSocket is not connected", so wait for it (bounded).
  const connected = async (timeoutMs = 5000) => {
    const deadline = Date.now() + timeoutMs;
    while (typeof window.webui.isConnected === 'function' && !window.webui.isConnected() && Date.now() < deadline)
      await new Promise(resolve => setTimeout(resolve, 25));
  };
  const invoke = async (binding, ...args) => {
    await connected();
    let raw;
    // webui.call(name, ...args) is the primary route; named bindings are a fallback for
    // builds that expose them directly.
    if (typeof window.webui.call === 'function') {
      try {
        raw = await window.webui.call(binding, ...args);
      } catch (callError) {
        if (typeof window.webui[binding] !== 'function') throw callError;
        raw = await window.webui[binding](...args);
      }
    } else if (typeof window.webui[binding] === 'function') {
      raw = await window.webui[binding](...args);
    } else {
      throw new Error(`Bridge WebUI non disponibile: funzione '${binding}' non esposta. Verificare webui.js e i binding C++.`);
    }
    const value = typeof raw === 'string' ? JSON.parse(raw) : raw;
    if (value && !Array.isArray(value) && value.error) throw new Error(value.error);
    return value;
  };
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  window.diskUtilities = {
    getDefaultRoot: () => invoke('du_getDefaultRoot'),
    getDashboard: root => invoke('du_getDashboard', root),
    getLargestFiles: (root, limit = 100) => invoke('du_getLargestFiles', root, limit),
    getExtensionStatistics: root => invoke('du_getExtensionStatistics', root),
    // options: { sort, descending, filter, directoriesOnly, offset, limit } (sent as one JSON string)
    listDirectory: (path, options = {}) => invoke('du_listDirectory', path, JSON.stringify(options)),
    getDirectoryTree: (root, depth = 2) => invoke('du_getDirectoryTree', root, depth),
    cancelScan: () => invoke('du_cancelScan'),
    // File search (live filesystem, independent from the scan). `request` is sent as one JSON string:
    // { nameContains, patterns[], excludePatterns[], caseSensitive, kind, includeHidden, minSize, maxSize,
    //   modifiedAfter, modifiedBefore, createdAfter, createdBefore, maxResults }  (sizes in bytes, dates in Unix seconds)
    startSearch: (root, request) => invoke('du_startSearch', root, JSON.stringify(request)),
    cancelSearch: () => invoke('du_cancelSearch'),
    getSearchStatus: () => invoke('du_getSearchStatus'),
    // options: { offset, limit, sort: path|name|size|modified|created, descending }
    getSearchResults: (options = {}) => invoke('du_getSearchResults', JSON.stringify(options)),
    // Sub-folders of a directory (read-only), for the folder chooser. An empty path means the top of the file system.
    listFolders: path => invoke('du_listFolders', path || ''),
    /**
     * Start a scan and resolve with the final status. `onProgress` receives every polled
     * status while the scan runs. Transient polling errors are retried before giving up.
     */
    scan: async (root, onProgress) => {
      await invoke('du_startScan', root);
      let failures = 0;
      for (;;) {
        let status;
        try {
          status = await invoke('du_getScanStatus');
          failures = 0;
        } catch (error) {
          if (++failures >= 4) throw error;
          await sleep(500);
          continue;
        }
        if (status.state === 'completed' || status.state === 'cancelled') return status;
        if (status.state === 'failed') throw new Error(status.error || 'Scansione non riuscita.');
        if (typeof onProgress === 'function') onProgress(status);
        await sleep(200);
      }
    }
  };
}

/** Resolve once the WebUI bridge exists (or immediately when not served by WebUI at all). */
async function waitForWebui(timeoutMs = 3000) {
  if (window.webui) return true;
  if (typeof WebuiBridge === 'undefined') return false; // plain browser: demo mode
  const deadline = Date.now() + timeoutMs;
  while (!window.webui && Date.now() < deadline) await new Promise(resolve => setTimeout(resolve, 25));
  return !!window.webui;
}

const $ = (selector, root = document) => root.querySelector(selector);
const $$ = (selector, root = document) => [...root.querySelectorAll(selector)];
const PAGE_SIZE = 500;
const state = {
  root: '/home/utente', currentPath: '/home/utente', view: 'dashboard',
  dashboard: null, largest: [], extensions: [], directoryItems: [],
  listing: { total: 0, totalBytes: 0, truncated: false },
  tree: { expanded: new Set(), children: new Map(), loading: new Set() },
  isDemo: true, hasScan: false, scanning: false, extensionCount: null, sort: 'name', largestLimit: 100,
  seq: { refresh: 0, explorer: 0 },
  pathTarget: 'scan',
  search: { running: false, shown: 500, sort: 'path', descending: false, total: 0, rootInitialised: false, demo: null }
};
const PALETTE = ['#79a9ff', '#a18bff', '#55d4bc', '#efb965', '#60718f', '#ed8da8', '#73c7ed'];

const demo = {
  dashboard: {
    totalBytes: 86_472_318_976, fileCount: 18432, directoryCount: 1268, inaccessibleCount: 3,
    largestDirectories: [
      { path: '/home/utente/Video', name: 'Video', size: 28_400_000_000 },
      { path: '/home/utente/Progetti', name: 'Progetti', size: 18_200_000_000 },
      { path: '/home/utente/Download', name: 'Download', size: 12_800_000_000 },
      { path: '/home/utente/Immagini', name: 'Immagini', size: 8_100_000_000 },
      { path: '/home/utente/Documenti', name: 'Documenti', size: 3_500_000_000 }
    ]
  },
  extensions: [
    { extension: '.mp4', count: 428, totalBytes: 25_600_000_000 },
    { extension: '.zip', count: 183, totalBytes: 16_200_000_000 },
    { extension: '.jpg', count: 3920, totalBytes: 12_300_000_000 },
    { extension: '.pdf', count: 1260, totalBytes: 8_400_000_000 },
    { extension: '.cpp', count: 840, totalBytes: 4_900_000_000 },
    { extension: 'senza estensione', count: 146, totalBytes: 1_200_000_000 }
  ],
  largest: [
    { name: 'backup-completo.zip', path: '/home/utente/Download/backup-completo.zip', size: 8_540_000_000, modified: '2026-09-28T14:22:00', extension: '.zip' },
    { name: 'vacanze-2025.mp4', path: '/home/utente/Video/vacanze-2025.mp4', size: 4_280_000_000, modified: '2025-08-16T18:05:00', extension: '.mp4' },
    { name: 'archivio-progetto.zip', path: '/home/utente/Progetti/archivio-progetto.zip', size: 2_950_000_000, modified: '2026-06-11T09:41:00', extension: '.zip' },
    { name: 'registrazione-evento.mp4', path: '/home/utente/Video/registrazione-evento.mp4', size: 2_120_000_000, modified: '2024-11-02T20:10:00', extension: '.mp4' },
    { name: 'foto-originali.zip', path: '/home/utente/Immagini/foto-originali.zip', size: 1_780_000_000, modified: '2025-12-24T12:30:00', extension: '.zip' },
    { name: 'dataset.csv', path: '/home/utente/Progetti/dataset.csv', size: 1_240_000_000, modified: '2026-08-09T07:18:00', extension: '.csv' },
    { name: 'manuale-scansione.pdf', path: '/home/utente/Documenti/manuale-scansione.pdf', size: 840_000_000, modified: '2026-03-14T10:00:00', extension: '.pdf' },
    { name: 'render-finale.mp4', path: '/home/utente/Video/render-finale.mp4', size: 780_000_000, modified: '2026-10-02T16:42:00', extension: '.mp4' }
  ],
  tree: {
    name: 'utente', path: '/home/utente', children: [
      { name: 'Documenti', path: '/home/utente/Documenti', children: [] },
      { name: 'Download', path: '/home/utente/Download', children: [] },
      { name: 'Immagini', path: '/home/utente/Immagini', children: [] },
      { name: 'Progetti', path: '/home/utente/Progetti', children: [
        { name: 'DiskUtilities', path: '/home/utente/Progetti/DiskUtilities', children: [] },
        { name: 'Numina++', path: '/home/utente/Progetti/Numina++', children: [] }
      ] },
      { name: 'Video', path: '/home/utente/Video', children: [] }
    ]
  },
  directories: {
    '/home/utente': [
      { name: 'Documenti', path: '/home/utente/Documenti', type: 'directory', size: 3_500_000_000, modified: '2026-10-04T09:20:00' },
      { name: 'Download', path: '/home/utente/Download', type: 'directory', size: 12_800_000_000, modified: '2026-10-15T11:10:00' },
      { name: 'Immagini', path: '/home/utente/Immagini', type: 'directory', size: 8_100_000_000, modified: '2026-10-01T18:30:00' },
      { name: 'Progetti', path: '/home/utente/Progetti', type: 'directory', size: 18_200_000_000, modified: '2026-10-14T08:12:00' },
      { name: 'Video', path: '/home/utente/Video', type: 'directory', size: 28_400_000_000, modified: '2026-10-02T16:42:00' },
      { name: 'appunti.txt', path: '/home/utente/appunti.txt', type: 'file', size: 24_300, modified: '2026-10-12T20:00:00', extension: '.txt' }
    ],
    '/home/utente/Documenti': [
      { name: 'manuale-scansione.pdf', path: '/home/utente/Documenti/manuale-scansione.pdf', type: 'file', size: 840_000_000, modified: '2026-03-14T10:00:00', extension: '.pdf' },
      { name: 'note-riunione.docx', path: '/home/utente/Documenti/note-riunione.docx', type: 'file', size: 2_300_000, modified: '2026-10-04T09:20:00', extension: '.docx' },
      { name: 'preventivo.pdf', path: '/home/utente/Documenti/preventivo.pdf', type: 'file', size: 3_400_000, modified: '2026-09-30T14:11:00', extension: '.pdf' }
    ],
    '/home/utente/Download': [
      { name: 'backup-completo.zip', path: '/home/utente/Download/backup-completo.zip', type: 'file', size: 8_540_000_000, modified: '2026-09-28T14:22:00', extension: '.zip' },
      { name: 'installer.exe', path: '/home/utente/Download/installer.exe', type: 'file', size: 540_000_000, modified: '2026-08-17T11:00:00', extension: '.exe' },
      { name: 'manuale.pdf', path: '/home/utente/Download/manuale.pdf', type: 'file', size: 12_500_000, modified: '2026-05-02T08:00:00', extension: '.pdf' }
    ],
    '/home/utente/Immagini': [
      { name: 'foto-originali.zip', path: '/home/utente/Immagini/foto-originali.zip', type: 'file', size: 1_780_000_000, modified: '2025-12-24T12:30:00', extension: '.zip' },
      { name: 'panorama.jpg', path: '/home/utente/Immagini/panorama.jpg', type: 'file', size: 28_400_000, modified: '2026-07-10T13:11:00', extension: '.jpg' }
    ],
    '/home/utente/Progetti': [
      { name: 'DiskUtilities', path: '/home/utente/Progetti/DiskUtilities', type: 'directory', size: 1_300_000_000, modified: '2026-10-15T08:00:00' },
      { name: 'Numina++', path: '/home/utente/Progetti/Numina++', type: 'directory', size: 15_400_000_000, modified: '2026-10-14T08:12:00' },
      { name: 'dataset.csv', path: '/home/utente/Progetti/dataset.csv', type: 'file', size: 1_240_000_000, modified: '2026-08-09T07:18:00', extension: '.csv' }
    ],
    '/home/utente/Progetti/DiskUtilities': [
      { name: 'src', path: '/home/utente/Progetti/DiskUtilities/src', type: 'directory', size: 42_000_000, modified: '2026-10-15T08:00:00' },
      { name: 'CMakeLists.txt', path: '/home/utente/Progetti/DiskUtilities/CMakeLists.txt', type: 'file', size: 8_900, modified: '2026-10-15T07:40:00', extension: '.txt' }
    ],
    '/home/utente/Progetti/Numina++': [
      { name: 'include', path: '/home/utente/Progetti/Numina++/include', type: 'directory', size: 420_000_000, modified: '2026-10-14T08:12:00' },
      { name: 'tests', path: '/home/utente/Progetti/Numina++/tests', type: 'directory', size: 72_000_000, modified: '2026-10-12T16:00:00' }
    ],
    '/home/utente/Video': [
      { name: 'vacanze-2025.mp4', path: '/home/utente/Video/vacanze-2025.mp4', type: 'file', size: 4_280_000_000, modified: '2025-08-16T18:05:00', extension: '.mp4' },
      { name: 'registrazione-evento.mp4', path: '/home/utente/Video/registrazione-evento.mp4', type: 'file', size: 2_120_000_000, modified: '2024-11-02T20:10:00', extension: '.mp4' },
      { name: 'render-finale.mp4', path: '/home/utente/Video/render-finale.mp4', type: 'file', size: 780_000_000, modified: '2026-10-02T16:42:00', extension: '.mp4' }
    ]
  }
};
function formatBytes(value) {
  const bytes = Number(value) || 0;
  if (bytes < 1000) return `${bytes} B`;
  const units = ['kB', 'MB', 'GB', 'TB', 'PB'];
  let amount = bytes, index = -1;
  do { amount /= 1000; index++; } while (amount >= 1000 && index < units.length - 1);
  return `${new Intl.NumberFormat('it-IT', { maximumFractionDigits: amount < 10 ? 2 : 1 }).format(amount)} ${units[index]}`;
}
function formatNumber(value) { return new Intl.NumberFormat('it-IT').format(Number(value) || 0); }
function formatDate(value) {
  if (!value) return '—';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return new Intl.DateTimeFormat('it-IT', { year: 'numeric', month: '2-digit', day: '2-digit' }).format(date);
}
function formatDuration(ms) {
  const seconds = (Number(ms) || 0) / 1000;
  return seconds < 60 ? `${seconds.toLocaleString('it-IT', { maximumFractionDigits: 1 })} s`
    : `${Math.floor(seconds / 60)} min ${Math.round(seconds % 60)} s`;
}
function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, c => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c]);
}
function basename(path) { return String(path).split(/[\\/]/).filter(Boolean).pop() || path; }
function dirname(path) {
  const clean = String(path).replace(/[\\/]+$/, '');
  const index = Math.max(clean.lastIndexOf('/'), clean.lastIndexOf('\\'));
  return index <= 0 ? clean.slice(0, 1) || clean : clean.slice(0, index);
}
function extensionOf(name) { const i = name.lastIndexOf('.'); return i > 0 && i < name.length - 1 ? name.slice(i).toLowerCase() : 'senza estensione'; }
function isDirectory(item) { return item.type === 'directory' || item.type === 'Directory'; }
function fileIcon(item) {
  if (isDirectory(item)) return ['folder', '▰'];
  const ext = (item.extension || extensionOf(item.name)).toLowerCase();
  if (['.jpg', '.jpeg', '.png', '.gif', '.webp', '.svg'].includes(ext)) return ['image', '▧'];
  if (['.zip', '.7z', '.rar', '.gz', '.tar'].includes(ext)) return ['archive', '▱'];
  return ['', '▤'];
}
function showToast(message) {
  const toast = $('#toast'); toast.textContent = message; toast.classList.add('visible');
  clearTimeout(showToast.timer); showToast.timer = setTimeout(() => toast.classList.remove('visible'), 3600);
}
function debounce(fn, delay) {
  let timer;
  return (...args) => { clearTimeout(timer); timer = setTimeout(() => fn(...args), delay); };
}
function setFooter(text) { $('#footerStatus').textContent = text; }

/* Native bridge contract. Return values may be direct objects or Promises.
 * The demo fallback is used only while no native bridge exists.
 */
function nativeMethod(name, ...args) {
  const bridge = window.diskUtilities;
  if (bridge && typeof bridge[name] === 'function') return Promise.resolve(bridge[name](...args));
  return null;
}
async function callBackend(name, args, fallback, onErrorValue) {
  try {
    const result = nativeMethod(name, ...(args || []));
    if (result !== null) return await result;
  } catch (error) {
    console.error(`DiskUtilities bridge ${name}:`, error);
    // Never replace a failed native result with demo data while the bridge exists:
    // that would make a failure look like valid (fake) data.
    if (window.diskUtilities) {
      showToast(`Errore bridge ${name}: ${error.message || error}`);
      return typeof onErrorValue === 'function' ? onErrorValue() : onErrorValue;
    }
  }
  state.isDemo = true;
  return typeof fallback === 'function' ? fallback() : fallback;
}
const EMPTY_DASHBOARD = () => ({ scanned: false, totalBytes: 0, fileCount: 0, directoryCount: 0, inaccessibleCount: 0, largestDirectories: [], volume: { valid: false } });
const EMPTY_LISTING = path => () => ({ found: false, path, total: 0, totalBytes: 0, truncated: false, entries: [] });
async function getDashboard(root) { return callBackend('getDashboard', [root], () => ({ ...structuredClone(demo.dashboard), scanned: true, root, volume: { valid: true, capacity: 500_000_000_000, free: 180_000_000_000, available: 170_000_000_000 }, scanDurationMs: 0 }), EMPTY_DASHBOARD); }
async function getLargestFiles(root, limit = 100) { return callBackend('getLargestFiles', [root, limit], () => structuredClone(demo.largest).slice(0, limit), []); }
async function getExtensionStatistics(root) { return callBackend('getExtensionStatistics', [root], () => structuredClone(demo.extensions), []); }

/** Demo-only implementation of the server-side listing (filter, sort, paging). */
function demoListing(path, options) {
  const all = structuredClone(demo.directories[path] || []).map(item => ({
    ...item, hasSubdirectories: isDirectory(item) && (demo.directories[item.path] || []).some(isDirectory)
  }));
  const filter = (options.filter || '').toLowerCase();
  let items = all.filter(item => (!options.directoriesOnly || isDirectory(item)) && item.name.toLowerCase().includes(filter));
  const dir = options.descending ? -1 : 1;
  items.sort((a, b) => {
    if (isDirectory(a) !== isDirectory(b)) return isDirectory(a) ? -1 : 1;
    let c = 0;
    if (options.sort === 'size') c = (a.size || 0) - (b.size || 0);
    else if (options.sort === 'modified') c = String(a.modified || '').localeCompare(String(b.modified || ''));
    else if (options.sort === 'type') c = String(a.extension || '').localeCompare(String(b.extension || ''));
    else c = a.name.localeCompare(b.name, 'it', { sensitivity: 'base' });
    return c * dir || a.name.localeCompare(b.name, 'it', { sensitivity: 'base' });
  });
  const offset = options.offset || 0, limit = options.limit || 2000;
  return { found: path in demo.directories, path, total: items.length, totalBytes: items.reduce((s, i) => s + (i.size || 0), 0),
    offset, truncated: offset + limit < items.length, entries: items.slice(offset, offset + limit) };
}
async function listDirectory(path, options = {}) {
  return callBackend('listDirectory', [path, options], () => demoListing(path, options), EMPTY_LISTING(path));
}

/* ---- Folder chooser: the browser cannot return an absolute path, so the native side lists folders ---- */
const folderBrowser = { notice: '', path: '', parent: '', canGoUp: false, seq: 0, shortcuts: [], folders: [], showHidden: false };

/** Small fake file system used when the page runs without the native bridge. */
const DEMO_FOLDERS = { '/': ['home', 'mnt', 'usr'], '/home': ['utente'], '/home/utente': ['Documenti', 'Immagini', 'Progetti'], '/home/utente/Progetti': ['DiskUtilities', 'TK4CPP'] };
function demoFolderListing(path) {
  const clean = (path || '/').replace(/\/+$/, '') || '/';
  const names = DEMO_FOLDERS[clean];
  if (!names) return { found: false, path: clean, reason: 'Il percorso non esiste (dati dimostrativi).', folders: [], shortcuts: [] };
  const parent = clean === '/' ? '' : (clean.slice(0, clean.lastIndexOf('/')) || '/');
  return { found: true, path: clean, parent, canGoUp: clean !== '/', truncated: false, reason: '',
    folders: names.map(name => ({ name, path: `${clean === '/' ? '' : clean}/${name}`, hidden: false })),
    shortcuts: [{ name: 'Cartella utente', path: '/home/utente' }, { name: 'Radice (/)', path: '/' }] };
}
async function listFolders(path) {
  return callBackend('listFolders', [path], () => demoFolderListing(path), () => ({ found: false, reason: 'Elenco cartelle non disponibile.', folders: [], shortcuts: [] }));
}

function folderCrumbs(path) {
  if (!path) return [{ name: 'Questo computer', path: '' }];
  const crumbs = [];
  const parts = path.split(/[\\/]/).filter(Boolean);
  const windows = /^[A-Za-z]:/.test(path);
  if (windows) crumbs.push({ name: 'Questo computer', path: '' });
  else crumbs.push({ name: '/', path: '/' });
  let accumulated = '';
  parts.forEach((part, i) => {
    accumulated = i === 0 ? (windows ? `${part}/` : `/${part}`) : `${accumulated.replace(/\/$/, '')}/${part}`;
    crumbs.push({ name: part, path: accumulated });
  });
  return crumbs;
}

function renderFolderBrowser(error) {
  const fb = folderBrowser;
  const crumbs = folderCrumbs(fb.path);
  $('#fbCrumbs').innerHTML = crumbs.map((c, i) => `${i ? '<span class="sep">›</span>' : ''}<button type="button" data-fb-path="${escapeHtml(c.path)}" ${i === crumbs.length - 1 ? 'aria-current="true"' : ''}>${escapeHtml(c.name)}</button>`).join('');
  $('#fbUp').disabled = !fb.canGoUp;
  $('#fbPlaces').innerHTML = fb.shortcuts.map(s => `<button type="button" data-fb-path="${escapeHtml(s.path)}" class="${s.path === fb.path ? 'active' : ''}" title="${escapeHtml(s.path)}">${escapeHtml(s.name)}</button>`).join('');
  const visible = fb.folders.filter(f => fb.showHidden || !f.hidden);
  $('#fbList').innerHTML = visible.map(f => `<button type="button" role="option" class="fb-item ${f.hidden ? 'hidden-folder' : ''}" data-fb-path="${escapeHtml(f.path)}" title="${escapeHtml(f.path)}"><span class="glyph" aria-hidden="true">▰</span><span class="name">${escapeHtml(f.name)}</span></button>`).join('');
  const status = $('#fbStatus');
  status.classList.toggle('error', !!error);
  if (error) status.textContent = error;
  else if (fb.notice) status.textContent = fb.notice;
  else if (!visible.length) status.textContent = fb.folders.length ? 'Solo cartelle nascoste: attiva “Nascoste” per vederle.' : 'Nessuna sottocartella.';
  else status.textContent = `${formatNumber(visible.length)} cartelle${fb.truncated ? ' · elenco parziale: digita il percorso per le altre' : ''}`;
  $('#fbCurrent').textContent = fb.path || 'Questo computer';
  $('#fbUse').disabled = !fb.path;
}

/** Open @p path in the chooser; a path that cannot be listed falls back to its nearest existing ancestor. */
async function browseTo(path) {
  const fb = folderBrowser;
  const token = ++fb.seq;
  fb.notice = '';
  let target = path, listing = null, firstError = '';
  for (let guard = 0; guard < 64; ++guard) {
    listing = await listFolders(target);
    if (token !== fb.seq) return; // a newer navigation superseded this one
    if (listing.found) break;
    if (!firstError) firstError = listing.reason || 'Cartella non disponibile.';
    const slash = Math.max(target.lastIndexOf('/'), target.lastIndexOf('\\'));
    const next = slash > 0 ? target.slice(0, slash) : '';
    if (next === target) break;
    target = /^[A-Za-z]:$/.test(next) ? `${next}/` : next;
    if (!target && !/^[A-Za-z]:/.test(path)) target = '/';
  }
  if (!listing || !listing.found) { renderFolderBrowser(firstError || 'Cartella non disponibile.'); return; }
  fb.path = listing.path || ''; fb.parent = listing.parent || ''; fb.canGoUp = !!listing.canGoUp;
  fb.folders = listing.folders || []; fb.truncated = !!listing.truncated;
  if (listing.shortcuts && listing.shortcuts.length) fb.shortcuts = listing.shortcuts;
  fb.notice = target !== path && path ? `“${path}” non è disponibile: mostrata la cartella più vicina.` : '';
  renderFolderBrowser();
  $('#fbList').scrollTop = 0;
}
async function openFolderBrowser() {
  $('#folderBrowser').classList.remove('hidden');
  $('#browseBtn').setAttribute('aria-expanded', 'true');
  await browseTo($('#rootInput').value.trim());
  $('#fbList').querySelector('.fb-item')?.focus({ preventScroll: true });
}
function closeFolderBrowser() {
  folderBrowser.seq++;
  $('#folderBrowser').classList.add('hidden');
  $('#browseBtn').setAttribute('aria-expanded', 'false');
}

function setView(view) {
  state.view = view;
  $$('.nav-item').forEach(button => button.classList.toggle('active', button.dataset.view === view));
  $$('.page-view').forEach(section => section.classList.toggle('active', section.id === `view-${view}`));
  if (view === 'explorer') refreshExplorer();
  if (view === 'search' && !state.search.rootInitialised) { state.search.rootInitialised = true; $('#sRoot').value = state.root; }
}

function renderScanNotice() {
  const notice = $('#scanNotice');
  const unscanned = !state.isDemo && !(state.dashboard && state.dashboard.scanned);
  notice.classList.toggle('hidden', !unscanned || state.scanning);
  if (unscanned) notice.textContent = state.hasScan
    ? 'Il percorso selezionato non corrisponde all’ultima scansione. Avvia una nuova scansione.'
    : 'Nessuna scansione per questo percorso: premi “Avvia scansione” per analizzarlo.';
}

function renderVolume() {
  const panel = $('#volumePanel');
  const d = state.dashboard || {};
  const v = d.volume;
  if (!v || !v.valid || !v.capacity) { panel.classList.add('hidden'); return; }
  panel.classList.remove('hidden');
  const used = Math.max(0, v.capacity - v.free);
  // Logical file sizes can exceed the space really in use (sparse/compressed files): clamp.
  const scanned = Math.min(d.totalBytes || 0, used);
  const other = used - scanned;
  const pct = value => `${Math.max(0, value / v.capacity * 100)}%`;
  $('#volumeBar').innerHTML = `<span class="seg-scanned" style="width:${pct(scanned)}"></span><span class="seg-other" style="width:${pct(other)}"></span>`;
  $('#volumeSummary').textContent = `${formatBytes(used)} usati su ${formatBytes(v.capacity)} (${(used / v.capacity * 100).toLocaleString('it-IT', { maximumFractionDigits: 1 })}%)`;
  $('#volumeLegend').innerHTML = `
    <span><i class="seg-scanned"></i>Percorso analizzato <strong>${formatBytes(scanned)}</strong></span>
    <span><i class="seg-other"></i>Altro sul volume <strong>${formatBytes(other)}</strong></span>
    <span><i class="seg-free"></i>Libero <strong>${formatBytes(v.free)}</strong></span>`;
}

function renderDashboard() {
  const d = state.dashboard || {};
  const hasData = state.isDemo || d.scanned;
  const value = (fn, v) => hasData ? fn(v) : '—';
  $('#metricSize').textContent = value(formatBytes, d.totalBytes);
  $('#metricFiles').textContent = value(formatNumber, d.fileCount);
  $('#metricDirs').textContent = value(formatNumber, d.directoryCount);
  $('#metricErrors').textContent = value(formatNumber, d.inaccessibleCount);
  $('#metricSizeFoot').textContent = d.live ? `Dimensione logica · in aggiornamento${d.elapsedMs ? ` · ${formatDuration(d.elapsedMs)}` : ''}`
    : (d.scanDurationMs ? `Dimensione logica · scansione in ${formatDuration(d.scanDurationMs)}` : 'Dimensione logica dei file');
  $('#liveBadge').classList.toggle('hidden', !d.live);
  $('#extensionCount').textContent = hasData ? formatNumber(state.extensionCount ?? state.extensions.length) : '—';
  $('#dataMode').textContent = state.isDemo ? 'DATI DIMOSTRATIVI' : 'DATI ANALIZZATI';
  $('#dataMode').classList.toggle('hidden', !state.isDemo);
  renderScanNotice();
  renderVolume();

  const extensions = [...state.extensions].sort((a, b) => b.totalBytes - a.totalBytes).slice(0, 6);
  const total = (d.live ? d.totalBytes : extensions.reduce((sum, item) => sum + item.totalBytes, 0)) || 1;
  let start = 0;
  const segments = extensions.map((item, i) => {
    const end = start + item.totalBytes / total * 100; const segment = `${PALETTE[i % PALETTE.length]} ${start}% ${end}%`; start = end; return segment;
  });
  if (segments.length && start < 99.9) segments.push(`#26324a ${start}% 100%`); // remainder: types not listed
  $('#extensionDonut').style.background = segments.length ? `conic-gradient(${segments.join(',')})` : '#26324a';
  $('#extensionLegend').innerHTML = extensions.map((item, i) => `<div class="legend-item"><span class="legend-swatch" style="background:${PALETTE[i % PALETTE.length]}"></span><span>${escapeHtml(item.extension)}</span><strong>${formatBytes(item.totalBytes)}</strong></div>`).join('') || '<p class="subtitle">Nessun dato disponibile.</p>';

  const dirs = (d.largestDirectories || []).slice(0, 5); const max = Math.max(1, ...dirs.map(x => x.size));
  $('#folderRanking').innerHTML = dirs.map(item => `<div><div class="rank-top"><span class="rank-name" title="${escapeHtml(item.path)}">${escapeHtml(item.name || basename(item.path))}</span><span class="rank-size">${formatBytes(item.size)}</span></div><div class="rank-track"><div class="rank-fill" style="width:${Math.max(1, item.size / max * 100)}%"></div></div></div>`).join('') || '<p class="subtitle">Nessuna directory disponibile.</p>';
  $('#dashboardLargestBody').innerHTML = state.largest.slice(0, 5).map(file => `<tr><td>${renderFileName(file)}</td><td>${escapeHtml(dirname(file.path))}</td><td>${formatDate(file.modified)}</td><td class="align-right size-cell">${formatBytes(file.size)}</td></tr>`).join('') || emptyRow(4, 'Nessun file disponibile.');
}
function renderFileName(file) {
  const [kind, glyph] = fileIcon({ ...file, type: 'file' });
  return `<div class="file-name"><span class="file-icon ${kind}">${glyph}</span><span>${escapeHtml(file.name || basename(file.path))}<span class="subpath">${escapeHtml(file.path || '')}</span></span></div>`;
}
function emptyRow(columns, message) { return `<tr><td colspan="${columns}" style="text-align:center;color:#8290aa;padding:30px">${escapeHtml(message)}</td></tr>`; }
function renderLargest() {
  const filter = $('#largestSearch').value.trim().toLowerCase();
  const rows = state.largest.filter(f => `${f.name} ${f.path}`.toLowerCase().includes(filter));
  $('#largestSubtitle').textContent = `${formatNumber(rows.length)} file · dimensione decrescente`;
  $('#largestBody').innerHTML = rows.map(file => `<tr><td>${renderFileName(file)}</td><td>${escapeHtml(dirname(file.path))}</td><td>${escapeHtml(file.extension || extensionOf(file.name))}</td><td class="align-right size-cell">${formatBytes(file.size)}</td></tr>`).join('') || emptyRow(4, 'Nessun file corrisponde alla ricerca.');
}
function renderTypes() {
  const filter = $('#typesSearch').value.trim().toLowerCase();
  const items = state.extensions.filter(x => x.extension.toLowerCase().includes(filter)).sort((a, b) => b.totalBytes - a.totalBytes);
  const listed = state.extensions.reduce((sum, x) => sum + x.totalBytes, 0);
  const total = (state.dashboard && state.dashboard.live ? state.dashboard.totalBytes : listed) || 1;
  $('#typesBody').innerHTML = items.map(item => {
    const ratio = item.totalBytes / total * 100;
    return `<tr><td><span class="file-name">${escapeHtml(item.extension)}</span></td><td class="align-right">${formatNumber(item.count)}</td><td class="align-right size-cell">${formatBytes(item.totalBytes)}</td><td class="bar-cell"><div style="display:flex;align-items:center;gap:10px"><div class="mini-bar" style="flex:1"><span style="width:${ratio}%"></span></div><span style="width:42px;text-align:right;color:#91a1bc;font-size:10px">${ratio.toLocaleString('it-IT',{maximumFractionDigits:1})}%</span></div></td></tr>`;
  }).join('') || emptyRow(4, 'Nessuna estensione corrisponde al filtro.');
}

/* ---- Directory tree: lazy, one level per request ------------------------------------- */
function treeNodes(path) { return state.tree.children.get(path) || null; }
async function loadTreeChildren(path) {
  if (state.tree.children.has(path) || state.tree.loading.has(path)) return;
  state.tree.loading.add(path);
  try {
    const listing = await listDirectory(path, { directoriesOnly: true, sort: 'size', descending: true, limit: PAGE_SIZE });
    state.tree.children.set(path, { entries: listing.entries || [], more: Math.max(0, (listing.total || 0) - (listing.entries || []).length) });
  } finally { state.tree.loading.delete(path); }
}
function buildTreeNode(node, depth) {
  const expanded = state.tree.expanded.has(node.path);
  const kids = expanded ? treeNodes(node.path) : null;
  const selected = node.path === state.currentPath;
  const expandable = node.hasSubdirectories !== false;
  const chevron = expandable
    ? `<button class="tree-chevron" data-toggle="${escapeHtml(node.path)}" aria-label="${expanded ? 'Comprimi' : 'Espandi'} ${escapeHtml(node.name)}">${expanded ? '⌄' : '›'}</button>`
    : '<span class="tree-chevron"></span>';
  let children = '';
  if (expanded) {
    if (!kids) children = `<div class="tree-note" style="padding-left:${34 + depth * 14}px">Caricamento…</div>`;
    else children = `<div class="tree-children" role="group">${kids.entries.map(child => buildTreeNode(child, depth + 1)).join('')}${kids.more ? `<div class="tree-note" style="padding-left:${34 + depth * 14}px">… altre ${formatNumber(kids.more)} directory</div>` : ''}</div>`;
  }
  return `<div class="tree-node" role="treeitem" ${expandable ? `aria-expanded="${expanded}"` : ''}><div class="tree-line ${selected ? 'selected' : ''}" style="padding-left:${6 + depth * 14}px">${chevron}<button class="tree-row ${selected ? 'selected' : ''}" data-path="${escapeHtml(node.path)}" title="${escapeHtml(node.path)}"><span class="tree-folder">▰</span><span class="tree-name">${escapeHtml(node.name)}</span></button></div>${children}</div>`;
}
function renderTree() {
  const root = { name: basename(state.root) || state.root, path: state.root, hasSubdirectories: true };
  $('#directoryTree').innerHTML = buildTreeNode(root, 0);
}
async function expandTo(path) {
  // Expand every ancestor of `path` (below the root) so the selected directory is visible.
  const root = state.root.replace(/[\\/]+$/, '');
  state.tree.expanded.add(state.root);
  if (!path.startsWith(root) || path === state.root) return;
  const rest = path.slice(root.length).split(/[\\/]/).filter(Boolean);
  let current = root;
  for (const part of rest.slice(0, -1)) {
    current = `${current}/${part}`;
    state.tree.expanded.add(current);
  }
}
async function toggleTreeNode(path) {
  if (state.tree.expanded.has(path)) state.tree.expanded.delete(path);
  else state.tree.expanded.add(path);
  renderTree();
  if (state.tree.expanded.has(path) && !treeNodes(path)) { await loadTreeChildren(path); renderTree(); }
}
async function ensureTreeLoaded() {
  const missing = [...state.tree.expanded].filter(p => !treeNodes(p));
  await Promise.all(missing.map(loadTreeChildren));
}

function renderBreadcrumbs() {
  const path = state.currentPath;
  const sep = path.includes('\\') && !path.includes('/') ? '\\' : '/';
  const parts = path.split(/[\\/]/).filter(Boolean);
  const crumbs = [];
  let accumulated = '';
  if (path.startsWith('/')) crumbs.push({ name: '/', path: '/' });
  parts.forEach((part, i) => {
    accumulated = i === 0 ? (path.startsWith('/') ? `/${part}` : part) : `${accumulated}${sep}${part}`;
    crumbs.push({ name: part, path: accumulated });
  });
  $('#breadcrumbs').innerHTML = crumbs.map((c, i) => `${i ? '<span class="crumb-sep">/</span>' : ''}<button class="crumb ${i === crumbs.length - 1 ? 'current' : ''}" data-path="${escapeHtml(c.path)}" ${i === crumbs.length - 1 ? 'disabled' : ''}>${escapeHtml(c.name)}</button>`).join('');
}

/* ---- Explorer listing: sorted, filtered and paged by the native side ------------------- */
function listingOptions(offset = 0) {
  const descending = state.sort === 'size' || state.sort === 'modified';
  return { sort: state.sort, descending, filter: $('#explorerSearch').value.trim(), offset, limit: PAGE_SIZE };
}
function renderExplorerItems() {
  const items = state.directoryItems;
  $('#currentDirectoryTitle').textContent = basename(state.currentPath) || state.currentPath;
  const { total, totalBytes } = state.listing;
  $('#currentDirectoryMeta').textContent = `${formatNumber(total)} elementi · ${formatBytes(totalBytes)} dimensione aggregata`;
  const denominator = totalBytes || 1;
  $('#explorerBody').innerHTML = items.map(item => {
    const [kind, glyph] = fileIcon(item);
    const label = isDirectory(item) ? 'Directory' : (item.extension || extensionOf(item.name));
    const share = Math.min(100, (item.size || 0) / denominator * 100);
    const sizeCell = `<td class="align-right size-cell"><div class="size-with-bar"><span>${formatBytes(item.size)}</span><div class="mini-bar"><span style="width:${share}%"></span></div></div></td>`;
    const nameCell = isDirectory(item)
      ? `<td><button class="file-name open-directory" data-path="${escapeHtml(item.path)}"><span class="file-icon folder">${glyph}</span><span>${escapeHtml(item.name)}<span class="subpath">${item.accessible === false ? 'Non accessibile · ' : ''}${formatNumber(item.fileCount || 0)} file</span></span></button></td>`
      : `<td>${renderFileName(item)}</td>`;
    return `<tr>${nameCell}<td>${escapeHtml(label)}</td><td>${formatDate(item.modified)}</td>${sizeCell}</tr>`;
  }).join('');
  $('#explorerEmpty').textContent = state.scanning ? 'Scansione in corso: l’esplorazione sarà disponibile al termine.' : 'Nessun elemento da mostrare.';
  $('#explorerEmpty').classList.toggle('hidden', items.length !== 0);
  const remaining = Math.max(0, total - items.length);
  $('#explorerMore').classList.toggle('hidden', !state.listing.truncated);
  $('#explorerMore').textContent = `Carica altri ${formatNumber(Math.min(PAGE_SIZE, remaining))} elementi (${formatNumber(remaining)} rimanenti)`;
}
async function refreshExplorer() {
  const token = ++state.seq.explorer;
  renderBreadcrumbs();
  await expandTo(state.currentPath);
  const [listing] = await Promise.all([listDirectory(state.currentPath, listingOptions(0)), ensureTreeLoaded()]);
  if (token !== state.seq.explorer) return; // a newer navigation superseded this one
  state.directoryItems = listing.entries || [];
  state.listing = { total: listing.total || 0, totalBytes: listing.totalBytes || 0, truncated: !!listing.truncated };
  renderTree();
  renderExplorerItems();
  if (!state.isDemo && listing.found === false && state.dashboard && state.dashboard.scanned)
    showToast('La directory non è presente nell’ultima scansione.');
}
async function loadMoreExplorer() {
  const token = state.seq.explorer;
  const listing = await listDirectory(state.currentPath, listingOptions(state.directoryItems.length));
  if (token !== state.seq.explorer) return;
  state.directoryItems = state.directoryItems.concat(listing.entries || []);
  state.listing = { total: listing.total || 0, totalBytes: listing.totalBytes || 0, truncated: !!listing.truncated };
  renderExplorerItems();
}
async function navigateTo(path) {
  state.currentPath = path;
  await refreshExplorer();
}

async function refreshAll() {
  if (state.scanning) return; // partial results are on screen; the final refresh runs when the scan ends
  const token = ++state.seq.refresh;
  const [dashboard, largest, extensions] = await Promise.all([
    getDashboard(state.root), getLargestFiles(state.root, state.largestLimit), getExtensionStatistics(state.root)
  ]);
  if (token !== state.seq.refresh) return; // superseded by a newer refresh
  state.dashboard = dashboard;
  state.largest = largest || [];
  state.extensions = extensions || [];
  state.extensionCount = null;
  // Adopt the root as normalised by the native side so tree and listing paths compare equal.
  if (dashboard.scanned && dashboard.root && dashboard.root !== state.root) {
    if (state.currentPath === state.root) state.currentPath = dashboard.root;
    state.root = dashboard.root;
  }
  state.tree.children.clear();
  renderDashboard(); renderLargest(); renderTypes();
  $('#rootPath').textContent = state.root;
  if (!state.scanning) setFooter(state.isDemo ? 'Dati dimostrativi · bridge C++ non collegato'
    : (dashboard.scanned ? `Scansione completata${dashboard.scanDurationMs ? ` in ${formatDuration(dashboard.scanDurationMs)}` : ''}` : 'In attesa di scansione'));
  $('#statusDot').style.background = state.isDemo || !dashboard.scanned ? '#efb965' : '#54d6bd';
  if (state.view === 'explorer') await refreshExplorer();
}

/**
 * Show the partial results carried by a polled scan status. The shape matches the final queries
 * (dashboard, largest files, extension statistics), so the normal render functions are reused and the
 * final refresh simply replaces the numbers with the exact ones.
 */
function applyLive(status) {
  const live = status.live || { extensions: [], directories: [], largestFiles: [], extensionCount: 0 };
  state.dashboard = {
    scanned: true, live: true, root: status.root || state.root,
    totalBytes: status.bytesFound || 0, fileCount: status.filesFound || 0,
    directoryCount: status.directoriesFound || 0, inaccessibleCount: status.inaccessibleFound || 0,
    elapsedMs: status.elapsedMs || 0, volume: status.volume || { valid: false },
    largestDirectories: live.directories || []
  };
  state.largest = live.largestFiles || [];
  state.extensions = live.extensions || [];
  state.extensionCount = live.extensionCount || 0;
  renderDashboard(); renderLargest(); renderTypes();
}

function setScanUi(running, status) {
  const button = $('#scanBtn');
  button.disabled = false;
  button.textContent = running ? 'Annulla scansione' : 'Avvia scansione';
  $('#chooseRootBtn').disabled = running;
  const bar = $('#scanProgress');
  bar.classList.toggle('hidden', !running);
  if (running && status) {
    const visited = formatNumber(status.entriesVisited);
    const phase = status.phase === 'indexing' ? 'Indicizzazione' : 'Scansione in corso';
    const elapsed = status.elapsedMs ? ` · ${formatDuration(status.elapsedMs)}` : '';
    const rate = status.elapsedMs > 1000 ? ` · ${formatNumber(Math.round(status.entriesVisited / (status.elapsedMs / 1000)))} elementi/s` : '';
    const size = status.bytesFound ? ` · ${formatBytes(status.bytesFound)}` : '';
    const current = status.currentPath ? ` · ${status.currentPath}` : '';
    setFooter(`${phase} · ${visited} elementi${size}${elapsed}${rate}${current}`);
  }
}
async function startScan() {
  const bridge = window.diskUtilities;
  if (state.scanning) {
    if (bridge && typeof bridge.cancelScan === 'function') {
      try { await bridge.cancelScan(); showToast('Richiesta di annullamento inviata.'); }
      catch (error) { showToast(`Impossibile annullare: ${error.message || error}`); }
    }
    return;
  }
  if (!bridge || typeof bridge.scan !== 'function') {
    const message = 'Bridge nativo non collegato: la GUI è in modalità dimostrativa e non può avviare una scansione reale.';
    setFooter(message); showToast(message);
    console.error(message, { webui: window.webui, diskUtilities: window.diskUtilities });
    return;
  }
  state.scanning = true;
  const initial = { state: 'running', phase: 'scanning', root: state.root, entriesVisited: 0 };
  setScanUi(true, initial);
  applyLive(initial); // never show numbers of a previous scan as if they belonged to this one
  $('#statusDot').style.background = '#efb965';
  renderScanNotice();
  try {
    const result = await bridge.scan(state.root, status => { setScanUi(true, status); applyLive(status); });
    state.isDemo = false;
    state.scanning = false; // refreshAll must write the final footer text
    if (result.state === 'completed') {
      state.hasScan = true;
      state.currentPath = state.root = result.root || state.root;
      state.tree.expanded.clear();
      await refreshAll();
      showToast('Scansione completata.');
    } else {
      await refreshAll();
      showToast('Scansione annullata. Lo snapshot precedente, se presente, è stato mantenuto.');
    }
  } catch (error) {
    console.error('DiskUtilities scansione:', error);
    const message = `Scansione non riuscita: ${error.message || error}`;
    state.scanning = false;
    await refreshAll().catch(() => {}); // back to the last published snapshot, if any
    setFooter(message);
    showToast(message);
  } finally {
    state.scanning = false;
    setScanUi(false);
    renderScanNotice();
  }
}

/* ---------------------------------------------------------------------------------------------------------------
 * File search view
 * ------------------------------------------------------------------------------------------------------------- */
const SEARCH_POLL_MS = 250;
const SEARCH_PAGE = 500;

function formatDateTime(value) {
  if (!value) return '—';
  const date = new Date(value);
  if (Number.isNaN(date.getTime())) return String(value);
  return new Intl.DateTimeFormat('it-IT', { year: 'numeric', month: '2-digit', day: '2-digit', hour: '2-digit', minute: '2-digit' }).format(date);
}
function splitPatterns(text) { return String(text || '').split(/[;\n]/).map(part => part.trim()).filter(Boolean); }

/** Date input (YYYY-MM-DD, local time) -> Unix seconds at the start or at the very end of that day; null if empty. */
function dateToSeconds(value, endOfDay) {
  if (!value) return null;
  const date = new Date(`${value}T${endOfDay ? '23:59:59' : '00:00:00'}`);
  return Number.isNaN(date.getTime()) ? NaN : Math.floor(date.getTime() / 1000);
}
function sizeToBytes(valueId, unitId) {
  const raw = $(valueId).value.trim();
  if (raw === '') return null;
  const number = Number(raw.replace(',', '.'));
  if (!Number.isFinite(number) || number < 0) return NaN;
  return Math.round(number * Number($(unitId).value));
}

/** Read the form into a request for the native layer, or return { error } describing what is wrong. */
function collectSearchRequest() {
  const request = {
    nameContains: $('#sName').value.trim(),
    patterns: splitPatterns($('#sPatterns').value),
    excludePatterns: splitPatterns($('#sExclude').value),
    caseSensitive: $('#sCase').checked,
    kind: $('#sKind').value,
    includeHidden: $('#sHidden').checked,
    minSize: sizeToBytes('#sMinSize', '#sMinUnit'),
    maxSize: sizeToBytes('#sMaxSize', '#sMaxUnit'),
    modifiedAfter: dateToSeconds($('#sModAfter').value, false),
    modifiedBefore: dateToSeconds($('#sModBefore').value, true),
    createdAfter: dateToSeconds($('#sCreAfter').value, false),
    createdBefore: dateToSeconds($('#sCreBefore').value, true),
    maxResults: Number($('#sMax').value) || 10000
  };
  for (const key of ['minSize', 'maxSize', 'modifiedAfter', 'modifiedBefore', 'createdAfter', 'createdBefore'])
    if (Number.isNaN(request[key])) return { error: 'Valore non valido nei filtri di dimensione o data.' };
  if (request.minSize !== null && request.maxSize !== null && request.minSize > request.maxSize)
    return { error: 'La dimensione minima supera quella massima.' };
  if (request.modifiedAfter !== null && request.modifiedBefore !== null && request.modifiedAfter > request.modifiedBefore)
    return { error: 'L’intervallo delle date di modifica è invertito.' };
  if (request.createdAfter !== null && request.createdBefore !== null && request.createdAfter > request.createdBefore)
    return { error: 'L’intervallo delle date di creazione è invertito.' };
  if (request.kind === 'directories' && (request.minSize !== null || request.maxSize !== null))
    return { error: 'Le cartelle non hanno una dimensione: rimuovi il filtro sulla dimensione o cerca i file.' };
  return { request };
}

function showSearchError(message) {
  const box = $('#sError');
  box.textContent = message || '';
  box.classList.toggle('hidden', !message);
}

/* Demo fallback (no native bridge): filters the demo ranking so the form can be tried in a plain browser. */
function globToRegExp(pattern, caseSensitive) {
  const source = pattern.replace(/[.+^${}()|\\]/g, '\\$&').replace(/\*/g, '.*').replace(/\?/g, '.');
  return new RegExp(`^${source}$`, caseSensitive ? '' : 'i');
}
function demoSearch(request) {
  const text = request.caseSensitive ? request.nameContains : request.nameContains.toLowerCase();
  const include = request.patterns.map(p => globToRegExp(p, request.caseSensitive));
  const exclude = request.excludePatterns.map(p => globToRegExp(p, request.caseSensitive));
  return structuredClone(demo.largest).filter(file => {
    if (request.kind === 'directories') return false;
    const name = request.caseSensitive ? file.name : file.name.toLowerCase();
    const when = Date.parse(file.modified) / 1000;
    return name.includes(text) && (!include.length || include.some(re => re.test(file.name))) && !exclude.some(re => re.test(file.name))
      && (request.minSize === null || file.size >= request.minSize) && (request.maxSize === null || file.size <= request.maxSize)
      && (request.modifiedAfter === null || when >= request.modifiedAfter) && (request.modifiedBefore === null || when <= request.modifiedBefore);
  }).map(file => ({ ...file, created: '' }));
}

function searchSorted(hits) {
  const { sort, descending } = state.search;
  const keyed = hit => sort === 'size' ? hit.size : sort === 'name' ? hit.name : sort === 'modified' ? hit.modified : sort === 'created' ? hit.created : hit.path;
  return [...hits].sort((a, b) => { const x = keyed(a), y = keyed(b); const c = x < y ? -1 : x > y ? 1 : 0; return descending ? -c : c; });
}

function renderSearchRows(hits) {
  $('#searchBody').innerHTML = hits.length ? hits.map(hit => {
    const [kind, glyph] = fileIcon(hit);
    return `<tr><td><div class="file-name"><span class="file-icon ${kind}">${glyph}</span><span>${escapeHtml(hit.name)}</span></div></td>` +
      `<td class="path-cell" title="${escapeHtml(hit.path)}">${escapeHtml(dirname(hit.path))}</td>` +
      `<td class="align-right size-cell">${isDirectory(hit) ? '—' : escapeHtml(formatBytes(hit.size))}</td>` +
      `<td>${escapeHtml(formatDateTime(hit.modified))}</td><td>${escapeHtml(formatDateTime(hit.created))}</td>` +
      `<td><button class="copy-btn" type="button" data-copy="${escapeHtml(hit.path)}">Copia percorso</button></td></tr>`;
  }).join('') : emptyRow(6, 'Nessun risultato.');
  $$('.th-sort').forEach(button => {
    const active = button.dataset.sort === state.search.sort;
    if (active) button.setAttribute('aria-sort', state.search.descending ? 'descending' : 'ascending'); else button.removeAttribute('aria-sort');
  });
}

function renderSearch(status, page) {
  const hits = page.hits || [];
  state.search.total = page.total || 0;
  renderSearchRows(hits);
  const running = status.state === 'running';
  $('#searchTitle').textContent = `${formatNumber(page.total)} ${page.total === 1 ? 'risultato' : 'risultati'}`;
  const parts = [`${formatNumber(status.examined)} elementi esaminati`, `${formatNumber(status.directories)} cartelle`];
  if (status.elapsedMs) parts.push(formatDuration(status.elapsedMs));
  if (status.inaccessible) parts.push(`${formatNumber(status.inaccessible)} non accessibili`);
  if (hits.length < page.total) parts.push(`mostrati ${formatNumber(hits.length)}`);
  $('#searchMeta').textContent = parts.join(' · ');
  $('#searchMore').classList.toggle('hidden', hits.length >= page.total);
  setFooter(running ? `Ricerca in corso · ${formatNumber(page.total)} risultati · ${status.currentPath || ''}` :
    `Ricerca ${status.state === 'cancelled' ? 'annullata' : 'completata'} · ${formatNumber(page.total)} risultati`);

  const notes = [];
  if (running) notes.push('Durante la ricerca l’elenco si riordina man mano che arrivano nuovi risultati.');
  if (status.truncated) notes.push(`Raggiunto il limite di risultati: la ricerca si è fermata e l’elenco è parziale. Restringi i filtri o aumenta il limite.`);
  if (status.creationUnavailable) notes.push(`La data di creazione non è disponibile per ${formatNumber(status.creationUnavailable)} elementi (non supportata dal sistema o dal file system): sono stati esclusi.`);
  if (status.state === 'cancelled') notes.push('Ricerca annullata: sono mostrati i risultati trovati fino a quel momento.');
  const notice = $('#searchNotice');
  notice.textContent = notes.join(' ');
  notice.classList.toggle('hidden', notes.length === 0);
}

function setSearchRunning(running) {
  state.search.running = running;
  $('#searchLive').classList.toggle('hidden', !running);
  const go = $('#sGo');
  go.textContent = running ? 'Annulla ricerca' : 'Cerca';
  go.classList.toggle('button-primary', !running);
  go.classList.toggle('button-secondary', running);
}

async function fetchSearchPage() {
  const { shown, sort, descending } = state.search;
  const bridge = window.diskUtilities;
  const [status, page] = await Promise.all([bridge.getSearchStatus(), bridge.getSearchResults({ offset: 0, limit: shown, sort, descending })]);
  return { status, page };
}

async function runSearch() {
  const bridge = window.diskUtilities;
  if (state.search.running) {
    if (bridge && typeof bridge.cancelSearch === 'function') {
      try { await bridge.cancelSearch(); } catch (error) { showToast(`Impossibile annullare: ${error.message || error}`); }
    }
    return;
  }
  showSearchError('');
  const collected = collectSearchRequest();
  if (collected.error) { showSearchError(collected.error); return; }
  const root = $('#sRoot').value.trim() || state.root;
  state.search.shown = SEARCH_PAGE;

  if (!bridge || typeof bridge.startSearch !== 'function') { // demo mode
    const hits = searchSorted(demoSearch(collected.request));
    renderSearch({ state: 'completed', examined: hits.length, directories: 0, elapsedMs: 0 }, { total: hits.length, hits: hits.slice(0, state.search.shown) });
    showToast('Modalità dimostrativa: la ricerca reale richiede l’applicazione nativa.');
    return;
  }
  setSearchRunning(true);
  $('#searchBody').innerHTML = emptyRow(6, 'Ricerca in corso…');
  try {
    await bridge.startSearch(root, collected.request);
  } catch (error) {
    setSearchRunning(false);
    showSearchError(String(error.message || error));
    return;
  }
  let failures = 0;
  try {
    for (;;) {
      let snapshot;
      try { snapshot = await fetchSearchPage(); failures = 0; }
      catch (error) { if (++failures >= 4) throw error; await new Promise(r => setTimeout(r, 500)); continue; }
      renderSearch(snapshot.status, snapshot.page);
      if (snapshot.status.state !== 'running') {
        if (snapshot.status.state === 'failed') showSearchError(snapshot.status.failure || 'Ricerca non riuscita.');
        break;
      }
      await new Promise(r => setTimeout(r, SEARCH_POLL_MS));
    }
  } catch (error) {
    console.error('DiskUtilities ricerca:', error);
    showSearchError(`Ricerca non riuscita: ${error.message || error}`);
  } finally {
    setSearchRunning(false);
  }
}

/** Re-fetch the current page after a change of sort or page size (no new search). */
async function refreshSearchResults() {
  const bridge = window.diskUtilities;
  if (!bridge || typeof bridge.getSearchResults !== 'function' || state.search.running) return;
  try {
    const { status, page } = await fetchSearchPage();
    renderSearch(status, page);
  } catch (error) { showToast(`Errore: ${error.message || error}`); }
}

function resetSearchForm() {
  $('#searchForm').reset();
  $('#sRoot').value = state.root;
  showSearchError('');
}

function bindSearchEvents() {
  $('#searchForm').addEventListener('submit', event => { event.preventDefault(); runSearch(); });
  $('#sReset').addEventListener('click', resetSearchForm);
  $('#sBrowse').addEventListener('click', () => {
    state.pathTarget = 'search';
    $('#pathEyebrow').textContent = 'CARTELLA DI RICERCA';
    $('#rootInput').value = $('#sRoot').value.trim() || state.root;
    closeFolderBrowser();
    $('#pathDialog').showModal();
  });
  $('#searchMore').addEventListener('click', async () => { state.search.shown += SEARCH_PAGE; await refreshSearchResults(); });
  $$('.th-sort').forEach(button => button.addEventListener('click', async () => {
    const key = button.dataset.sort;
    state.search.descending = state.search.sort === key ? !state.search.descending : (key === 'size' || key === 'modified' || key === 'created');
    state.search.sort = key;
    if (!window.diskUtilities) { await runSearch(); return; }
    await refreshSearchResults();
  }));
  $('#searchBody').addEventListener('click', async event => {
    const button = event.target.closest('[data-copy]'); if (!button) return;
    try { await navigator.clipboard.writeText(button.dataset.copy); showToast('Percorso copiato.'); }
    catch { showToast(button.dataset.copy); } // clipboard may be unavailable in some browsers: show the path instead
  });
}

function bindEvents() {
  bindSearchEvents();
  $$('.nav-item').forEach(button => button.addEventListener('click', () => setView(button.dataset.view)));
  $$('[data-goto]').forEach(button => button.addEventListener('click', () => setView(button.dataset.goto)));
  $('#chooseRootBtn').addEventListener('click', () => { state.pathTarget = 'scan'; $('#pathEyebrow').textContent = 'NUOVA ANALISI'; $('#rootInput').value = state.root; closeFolderBrowser(); $('#pathDialog').showModal(); });
  $('#browseBtn').addEventListener('click', () => { if ($('#folderBrowser').classList.contains('hidden')) openFolderBrowser(); else closeFolderBrowser(); });
  $('#folderBrowser').addEventListener('click', event => {
    const target = event.target.closest('[data-fb-path]');
    if (target) browseTo(target.dataset.fbPath);
  });
  $('#fbUp').addEventListener('click', () => { if (folderBrowser.canGoUp) browseTo(folderBrowser.parent); });
  $('#fbHidden').addEventListener('change', event => { folderBrowser.showHidden = event.target.checked; renderFolderBrowser(); });
  $('#fbUse').addEventListener('click', () => {
    if (!folderBrowser.path) return;
    $('#rootInput').value = folderBrowser.path;
    $('#pathForm').requestSubmit();
  });
  $('#fbList').addEventListener('keydown', event => {
    const items = [...$$('.fb-item', $('#fbList'))]; const i = items.indexOf(document.activeElement);
    if (event.key === 'ArrowDown' && items.length) { event.preventDefault(); items[Math.min(items.length - 1, i + 1)].focus(); }
    else if (event.key === 'ArrowUp' && items.length) { event.preventDefault(); items[Math.max(0, i - 1)].focus(); }
    else if (event.key === 'Backspace' && folderBrowser.canGoUp) { event.preventDefault(); browseTo(folderBrowser.parent); }
  });
  $('#pathDialog').addEventListener('close', closeFolderBrowser);
  $('#pathForm').addEventListener('submit', async event => {
    event.preventDefault(); const value = $('#rootInput').value.trim(); if (!value) return;
    if (state.pathTarget === 'search') { $('#sRoot').value = value; state.pathTarget = 'scan'; $('#pathDialog').close(); return; }
    state.root = value; state.currentPath = value; state.hasScan = false; state.tree.expanded.clear();
    $('#pathDialog').close();
    await refreshAll(); showToast('Percorso aggiornato. Avvia la scansione per analizzare i dati reali.');
  });
  $('#applyRootBtn').addEventListener('click', event => { event.preventDefault(); $('#pathForm').requestSubmit(); });
  $('#scanBtn').addEventListener('click', startScan);
  $('#explorerSearch').addEventListener('input', debounce(refreshExplorer, 220));
  $('#explorerSort').addEventListener('change', event => { state.sort = event.target.value; refreshExplorer(); });
  $('#explorerMore').addEventListener('click', loadMoreExplorer);
  $('#largestSearch').addEventListener('input', debounce(renderLargest, 120));
  $('#largestLimit').addEventListener('change', async event => {
    state.largestLimit = Number(event.target.value) || 100;
    state.largest = await getLargestFiles(state.root, state.largestLimit) || [];
    renderLargest(); renderDashboard();
  });
  $('#typesSearch').addEventListener('input', debounce(renderTypes, 120));
  $('#directoryTree').addEventListener('click', async event => {
    const toggle = event.target.closest('[data-toggle]');
    if (toggle) { await toggleTreeNode(toggle.dataset.toggle); return; }
    const button = event.target.closest('[data-path]');
    if (button) await navigateTo(button.dataset.path);
  });
  $('#breadcrumbs').addEventListener('click', async event => {
    const button = event.target.closest('[data-path]'); if (!button || button.disabled) return;
    await navigateTo(button.dataset.path);
  });
  $('#explorerBody').addEventListener('click', async event => {
    const button = event.target.closest('.open-directory'); if (!button) return;
    await navigateTo(button.dataset.path);
  });
}

window.addEventListener('DOMContentLoaded', async () => {
  bindEvents();
  if (!window.diskUtilities && await waitForWebui()) installNativeBridge();
  // With WebUI, initialise from the native default path and show the empty state until the
  // user starts a scan. Browser-only mode stays demo-only.
  if (window.diskUtilities && typeof window.diskUtilities.getDefaultRoot === 'function') {
    try {
      state.root = await window.diskUtilities.getDefaultRoot();
      state.currentPath = state.root;
      state.isDemo = false;
    } catch (error) { console.error('DiskUtilities startup:', error); }
  }
  // A present bridge means real data, even if the default root could not be fetched.
  if (window.diskUtilities) state.isDemo = false;
  await refreshAll();
});
