// Read only the PKG table and its small metadata entries from the local file.
// This also handles entries located beyond the console's first 1 MB cache.
const decoder = new TextDecoder('utf-8');
const MAX_META = 262144;

async function read(file, offset, size) {
  if (!Number.isSafeInteger(offset) || offset < 0 || size < 0 || offset + size > file.size) {
    throw new Error('Invalid package metadata offset');
  }
  return new Uint8Array(await file.slice(offset, offset + size).arrayBuffer());
}

function text(bytes) {
  return decoder.decode(bytes).split('\0')[0];
}

function sfoDetails(bytes) {
  const v = new DataView(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  if (bytes.length < 20 || v.getUint32(0, true) !== 0x46535000) return {};
  const keys = v.getUint32(8, true);
  const values = v.getUint32(12, true);
  const count = Math.min(v.getUint32(16, true), 4096);
  const found = {};
  for (let i = 0; i < count && 20 + i * 16 + 16 <= bytes.length; i++) {
    const entry = 20 + i * 16;
    const ko = keys + v.getUint16(entry, true);
    const vo = values + v.getUint32(entry + 12, true);
    const len = v.getUint32(entry + 4, true);
    if (ko >= bytes.length || vo + len > bytes.length || len > 4096) continue;
    const end = bytes.indexOf(0, ko);
    if (end < 0) continue;
    found[text(bytes.subarray(ko, end))] = text(bytes.subarray(vo, vo + len));
  }
  return {
    title_name: found.TITLE || '', title_id: found.TITLE_ID || '',
    app_version: found.APP_VER || found.VERSION || '', category: found.CATEGORY || ''
  };
}

function jsonDetails(bytes) {
  const data = JSON.parse(text(bytes));
  const localized = data.localizedParameters || {};
  const preferred = localized.en_US || localized.en ||
    Object.values(localized).find((value) => value && typeof value === 'object' && value.titleName) || {};
  return {
    title_name: data.titleName || data.title || preferred.titleName || '',
    title_id: data.titleId || '',
    app_version: data.contentVersion || data.appVersion || data.version || '',
    category: data.category || '',
    has_base_app_metadata: ['applicationDrmType', 'applicationCategoryType', 'contentBadgeType']
      .some((key) => Object.prototype.hasOwnProperty.call(data, key))
  };
}

export async function parseLocalPkg(file) {
  const first = await read(file, 0, Math.min(file.size, 512));
  if (first.length < 128) throw new Error('Package header is too short');
  const magic = text(first.subarray(0, 4));
  let cnt = 0;
  if (magic === '\x7fFIH') {
    const v = new DataView(first.buffer);
    cnt = v.getUint32(0x58, true) + v.getUint32(0x5c, true) * 4294967296;
  } else if (magic !== '\x7fCNT') {
    throw new Error('Unsupported package format');
  }
  const header = await read(file, cnt, 128);
  if (text(header.subarray(0, 4)) !== '\x7fCNT') throw new Error('CNT header not found');
  const hv = new DataView(header.buffer);
  const cntType = hv.getUint32(0x04, false);
  const count = hv.getUint32(0x10, false);
  const tableOffset = hv.getUint32(0x18, false);
  if (!count || count > 2048 || tableOffset > 0x200000) throw new Error('Invalid package entry table');
  const entries = await read(file, cnt + tableOffset, count * 32);
  const ev = new DataView(entries.buffer);
  const result = {
    title_name: '', title_id: '', app_version: '', pkg_type: 'base',
    content_id: text(header.subarray(0x40, 0x70)), icon_offset: 0, icon_size: 0,
    has_base_app_metadata: false
  };
  const rows = [];
  let stringTable = null;
  for (let i = 0; i < count; i++) {
    const e = i * 32;
    const row = {
      type: ev.getUint32(e, false), nameOffset: ev.getUint32(e + 4, false),
      offset: ev.getUint32(e + 16, false), size: ev.getUint32(e + 20, false)
    };
    rows.push(row);
    if (row.type === 0x0200 && row.size < 65536) {
      stringTable = await read(file, cnt + row.offset, row.size);
    }
  }
  let category = '';
  for (const row of rows) {
    const end = stringTable && row.nameOffset < stringTable.length
      ? stringTable.indexOf(0, row.nameOffset) : -1;
    const name = end >= 0 ? text(stringTable.subarray(row.nameOffset, end)) : '';
    if ((row.type === 0x2000 || name === 'param.json') && row.size > 0 && row.size < MAX_META) {
      try {
        const details = jsonDetails(await read(file, cnt + row.offset, row.size));
        Object.assign(result, details);
        category = details.category || category;
      } catch (e) { /* Other metadata entries may still be usable. */ }
    } else if ((row.type === 0x1000 || name === 'param.sfo') && row.size > 0 && row.size < MAX_META) {
      const details = sfoDetails(await read(file, cnt + row.offset, row.size));
      for (const key of ['title_name', 'title_id', 'app_version']) {
        if (!result[key]) result[key] = details[key];
      }
      category = category || details.category;
    } else if ((row.type === 0x1200 || name === 'icon0.png') && row.size > 8 && row.size < 10 * 1024 * 1024 && cnt + row.offset + row.size <= file.size) {
      const signature = await read(file, cnt + row.offset, 8);
      if (signature[0] === 0x89 && text(signature.subarray(1, 4)) === 'PNG') {
        result.icon_offset = cnt + row.offset;
        result.icon_size = row.size;
      }
    }
    if (row.type === 0x1008 || row.type === 0x0407 || row.type === 0x0408) result.pkg_type = 'update';
  }
  if (result.pkg_type !== 'update') {
    if ((cntType & 0xff) === 1 && !result.has_base_app_metadata) result.pkg_type = 'dlc';
    else if (category.startsWith('gp')) result.pkg_type = 'update';
    else if (category.startsWith('ac') || category.startsWith('al')) result.pkg_type = 'dlc';
  }
  if (!result.title_id) {
    const match = result.content_id.match(/-([^_]+)_/);
    if (match) result.title_id = match[1];
  }
  if (!result.title_name) result.title_name = result.title_id || file.name;
  if (result.app_version && !/^v/i.test(result.app_version)) result.app_version = 'v' + result.app_version;
  return result;
}
