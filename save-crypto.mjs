import { xchacha20poly1305 } from '@noble/ciphers/chacha.js';
import keyBytes from './save-key.json';

const KEY = Uint8Array.from(keyBytes);
const FORMAT = 'hjx-magic-tower-portrait-v1';
const CIPHER = 'XChaCha20-Poly1305';
const AAD = Uint8Array.from(FORMAT + '|2|' + CIPHER,c=>c.charCodeAt(0));
const SIZE = 6304;

function encode(bytes) {
  let text = '';
  for (const byte of bytes) text += String.fromCharCode(byte);
  return btoa(text);
}
function decode(text) {
  if (typeof text !== 'string') throw new Error('Invalid encrypted save');
  const bytes = Uint8Array.from(atob(text), c => c.charCodeAt(0));
  if (encode(bytes) !== text) throw new Error('Invalid encrypted save');
  return bytes;
}

// The key is bundled for offline portability. This is a tampering barrier,
// not an authoritative anti-cheat system: the client owns its runtime.
export function seal(bytes, savedAt) {
  if (!(bytes instanceof Uint8Array) || bytes.length !== SIZE) throw new Error('存档长度不正确');
  if (!globalThis.crypto?.getRandomValues) throw new Error('浏览器无法生成安全随机数，不能加密存档');
  const nonce = globalThis.crypto.getRandomValues(new Uint8Array(24));
  const payload = new Uint8Array(SIZE + 8);
  const time = Date.parse(savedAt);
  new DataView(payload.buffer).setFloat64(0, Number.isFinite(time) ? time : 0, true);
  payload.set(bytes, 8);
  const ciphertext = xchacha20poly1305(KEY, nonce, AAD).encrypt(payload);
  return { format: FORMAT, version: 2, cipher: CIPHER, nonce: encode(nonce), data: encode(ciphertext) };
}

export function open(save) {
  if (!save || save.format !== FORMAT || save.version !== 2 || save.cipher !== CIPHER)
    throw new Error('只支持新版加密存档，旧版 JSON / BIN 文件不能直接导入');
  try {
    if (typeof save.nonce !== 'string' || save.nonce.length !== 32 || typeof save.data !== 'string' || save.data.length !== 8440)
      throw new Error('Invalid encrypted save size');
    const nonce = decode(save.nonce), ciphertext = decode(save.data);
    const payload = xchacha20poly1305(KEY, nonce, AAD).decrypt(ciphertext);
    if (payload.length !== SIZE + 8) throw new Error('Invalid encrypted save size');
    const time = new DataView(payload.buffer, payload.byteOffset, payload.byteLength).getFloat64(0, true);
    if (!Number.isFinite(time) || Math.abs(time) > 8640000000000000) throw new Error('Invalid save timestamp');
    return { bytes: payload.slice(8), savedAt: time ? new Date(time).toISOString() : undefined };
  } catch (_) {
    throw new Error('存档校验失败，内容已损坏或被修改');
  }
}

export function envelope(save) {
  return { format: FORMAT, version: 2, cipher: CIPHER, nonce: save.nonce, data: save.data };
}
