// Builds site/mc22nx.bin: the Switch build of Midnight Club II (ps2run.nro), the changes MC2Recomp's patcher makes to the game's
// main program (SLUS_202.09 -> BOOT.ELF) and its patch lists, encrypted with a key that only the game's own main program gives:
// SHA-256("mc22nx bundle key v1\0" + SLUS_202.09). The page (site/index.html) reads that file from the user's ISO, checks it,
// decrypts the bundle in the browser and hands out the files for the SD card. Nothing of the game is readable without the game.
//
// node tools/make_bundle.mjs <ps2run.nro> <original SLUS_202.09> <patched SLUS_202.09> <patches.txt> <toggles.txt> <out.bin>
//
// Bundle (before encryption): "MC22NX01", u32 LE length of the JSON header, the JSON header, then the data it points into.
// Encrypted file: 12-byte IV, then AES-256-GCM ciphertext with its 16-byte tag at the end.

import { createCipheriv, createHash, randomBytes } from 'node:crypto';
import { readFileSync, writeFileSync } from 'node:fs';

const [nroPath, elfPath, patchedPath, patchesPath, togglesPath, outPath] = process.argv.slice(2);
if (!outPath) {
    console.error('usage: node tools/make_bundle.mjs <ps2run.nro> <SLUS_202.09> <patched SLUS_202.09> <patches.txt> <toggles.txt> <out.bin>');
    process.exit(1);
}
const nro = readFileSync(nroPath), elf = readFileSync(elfPath), patched = readFileSync(patchedPath);

// the patched ELF as changed runs over the original, plus what it has beyond the original's end
const runs = [];
const common = Math.min(elf.length, patched.length);
for (let i = 0; i < common;) {
    if (elf[i] === patched[i]) { ++i; continue; }
    let j = i;
    while (j < common && elf[j] !== patched[j]) ++j;
    runs.push([i, j - i]);
    i = j;
}
const blobs = [];
let at = 0;
const add = (buf) => { const o = at; blobs.push(buf); at += buf.length; return o; };
const files = [
    { name: 'switch/mc22nx.nro', offset: add(nro), size: nro.length },
    { name: 'mc2/patches.txt', offset: add(readFileSync(patchesPath)), size: readFileSync(patchesPath).length },
    { name: 'mc2/toggles.txt', offset: add(readFileSync(togglesPath)), size: readFileSync(togglesPath).length },
];
const elfPatch = {
    size: patched.length,
    runs: runs.map(([o, n]) => [o, add(patched.subarray(o, o + n)), n]),
    tail: patched.length > common ? [common, add(patched.subarray(common)), patched.length - common] : null,
};
const header = Buffer.from(JSON.stringify({ version: 1, files, elfPatch, elfSha256: createHash('sha256').update(elf).digest('hex') }));
const len = Buffer.alloc(4);
len.writeUInt32LE(header.length);
const plain = Buffer.concat([Buffer.from('MC22NX01'), len, header, ...blobs]);

const key = createHash('sha256').update(Buffer.concat([Buffer.from('mc22nx bundle key v1\0'), elf])).digest();
const iv = randomBytes(12);
const cipher = createCipheriv('aes-256-gcm', key, iv);
const body = Buffer.concat([cipher.update(plain), cipher.final(), cipher.getAuthTag()]);
writeFileSync(outPath, Buffer.concat([iv, body]));
console.log(`${outPath}: ${plain.length} bytes in, ${runs.length} ELF runs, ELF sha256 ${createHash('sha256').update(elf).digest('hex')}`);
