// Deterministic page-record reader for XTC/XTCH containers.
//
// WHY THIS EXISTS: more than one tool in this repo has walked pages by SEARCHING for the `XTH\0` /
// `XTG\0` magic and advancing past each hit. That under-reports — a payload can contain the magic
// bytes, and once the scan lands on one, the records after it are skipped — so a "walker" reported
// 12 records for a 14-page container and 1683 for a 1684-page one. A verification tool that visits
// 99.94% of the pages and prints nothing wrong is worse than no tool, because it reads as a pass.
//
// The container already has a page index. Use it. This reader throws on anything it cannot prove
// (index out of bounds, page count disagreeing with the index, a record that runs past the buffer)
// rather than returning a partial list, so "the walker agreed with the writer" is always a real
// statement about every page.
//
// Container layout (see src/xtch_writer.h):
//   0x00  magic 'XTCH'
//   0x04  version major/minor
//   0x06  pageCount      (uint16 LE)
//   0x18  indexOffset    (uint64 LE) — pageCount * 16 bytes of { offset:u64, length:u32, flags:u32 }
//   ...   page records at their indexed offsets, each: 4-byte magic + 18-byte header + planes
'use strict';

const PAGE_INDEX_ENTRY = 16;

/**
 * Read every page record from an XTC/XTCH container, in container order.
 *
 * @param {ArrayBuffer|Uint8Array|Buffer} input
 * @returns {{count: number, meta: object, pages: Uint8Array[], index: Array<{offset:number,length:number,flags:number}>}}
 * @throws {Error} if the header, the index, or any record is inconsistent
 */
function xtcPageRecords(input) {
  const buf = Buffer.isBuffer(input)
    ? input
    : (input instanceof Uint8Array ? Buffer.from(input.buffer, input.byteOffset, input.byteLength)
                                   : Buffer.from(input));
  if (buf.length < 0x20) throw new Error(`container shorter than a header (${buf.length} bytes)`);
  if (buf.toString('latin1', 0, 4) !== 'XTCH') {
    throw new Error(`bad container magic: ${JSON.stringify(buf.toString('latin1', 0, 4))}`);
  }

  const pageCount = buf.readUInt16LE(0x06);
  const indexOffset = Number(buf.readBigUInt64LE(0x18));
  const indexBytes = pageCount * PAGE_INDEX_ENTRY;
  if (indexOffset + indexBytes > buf.length) {
    throw new Error(`page index out of bounds: ${pageCount} entries at ${indexOffset}`);
  }

  const index = [];
  const pages = [];
  for (let i = 0; i < pageCount; i++) {
    const e = indexOffset + i * PAGE_INDEX_ENTRY;
    const offset = Number(buf.readBigUInt64LE(e));
    const length = buf.readUInt32LE(e + 8);
    const flags = buf.readUInt32LE(e + 12);
    if (length <= 0 || offset + length > buf.length || offset < indexOffset + indexBytes) {
      throw new Error(`invalid page index ${i}: offset ${offset} length ${length} ` +
                      `(container ${buf.length} bytes)`);
    }
    const rec = buf.subarray(offset, offset + length);
    const magic = rec.toString('latin1', 0, 4);
    if (magic !== 'XTH\0' && magic !== 'XTG\0' && magic !== 'XTH\u0000') {
      throw new Error(`page ${i} at ${offset} has magic ${JSON.stringify(magic)}, not a page record`);
    }
    index.push({ offset, length, flags });
    pages.push(rec);
  }
  // The count in the header and the number of records produced are the same number by
  // construction; asserting it keeps that true if anyone changes the loop.
  if (pages.length !== pageCount) {
    throw new Error(`walked ${pages.length} records, header says ${pageCount}`);
  }
  return {
    count: pageCount,
    meta: { versionMajor: buf[4], versionMinor: buf[5], indexOffset },
    pages,
    index,
  };
}

/** Plane payload of a page record: 22-byte record header, then planes. */
function pagePlanes(record) {
  const magic = record.toString('latin1', 0, 4);
  const planeBytes = magic === 'XTH\0' ? 96000 : 48000;
  if (record.length < 22 + planeBytes) {
    throw new Error(`page record too short: ${record.length} bytes, expected ${22 + planeBytes}`);
  }
  return { magic, planes: record.subarray(22, 22 + planeBytes), is2Bit: magic === 'XTH\0' };
}

module.exports = { xtcPageRecords, pagePlanes };
