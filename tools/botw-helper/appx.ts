import { createReadStream } from "node:fs";
import { open, rename, rm, mkdtemp } from "node:fs/promises";
import { dirname, join, resolve } from "node:path";
import { createHash } from "node:crypto";
import { createInflateRaw } from "node:zlib";

const BLOCK = 65536;
const types: Record<string, string> = {
  exe: "application/x-msdownload",
  dll: "application/x-msdownload",
  png: "image/png",
  jpg: "image/jpeg",
  jpeg: "image/jpeg",
  wav: "audio/wav",
  json: "application/json",
  txt: "text/plain",
};
export const logos = [
  "StoreLogo",
  "Square150x150Logo",
  "Square44x44Logo",
  "SplashScreen",
];
export interface Entry {
  name: string;
  offset: number;
  size: number;
  compressed: number;
  method: number;
  crc: number;
}
const number64 = (buffer: Buffer, offset: number) => {
  const value = Number(buffer.readBigUInt64LE(offset));
  if (!Number.isSafeInteger(value))
    throw new Error("ZIP exceeds safe integer range");
  return value;
};

export async function readEntries(path: string): Promise<Entry[]> {
  const file = await open(path, "r");
  try {
    const size = (await file.stat()).size;
    const read = async (offset: number, length: number) => {
      if (offset < 0 || offset + length > size)
        throw new Error("Invalid ZIP offset");
      const buffer = Buffer.alloc(length);
      const { bytesRead } = await file.read(buffer, 0, length, offset);
      if (bytesRead !== length) throw new Error("Truncated ZIP");
      return buffer;
    };
    const tail = await read(Math.max(0, size - 65557), Math.min(size, 65557));
    let end = tail.length - 22;
    while (
      end >= 0 &&
      (tail.readUInt32LE(end) !== 0x06054b50 ||
        end + 22 + tail.readUInt16LE(end + 20) !== tail.length)
    )
      end--;
    if (end < 0) throw new Error("ZIP end record missing");
    if (tail.readUInt16LE(end + 4) || tail.readUInt16LE(end + 6))
      throw new Error("Split ZIP is unsupported");
    let count = tail.readUInt16LE(end + 10),
      cd = tail.readUInt32LE(end + 16);
    if (count === 0xffff || cd === 0xffffffff) {
      const locator = await read(size - tail.length + end - 20, 20);
      if (locator.readUInt32LE(0) !== 0x07064b50)
        throw new Error("ZIP64 locator missing");
      const record = await read(number64(locator, 8), 56);
      if (record.readUInt32LE(0) !== 0x06064b50)
        throw new Error("ZIP64 end record missing");
      count = number64(record, 32);
      cd = number64(record, 48);
    }
    const entries: Entry[] = [];
    const names = new Set<string>();
    for (let i = 0; i < count; i++) {
      const header = await read(cd, 46);
      if (header.readUInt32LE(0) !== 0x02014b50)
        throw new Error("Invalid ZIP central record");
      const flags = header.readUInt16LE(8),
        method = header.readUInt16LE(10);
      if (flags & 1 || ![0, 8].includes(method))
        throw new Error("Encrypted or unsupported ZIP entry");
      const nameLength = header.readUInt16LE(28),
        extraLength = header.readUInt16LE(30);
      const data = await read(cd + 46, nameLength + extraLength);
      const name = data.subarray(0, nameLength).toString("utf8");
      let size = header.readUInt32LE(24),
        compressed = header.readUInt32LE(20),
        offset = header.readUInt32LE(42);
      for (let p = nameLength; p + 4 <= data.length;) {
        const tag = data.readUInt16LE(p),
          length = data.readUInt16LE(p + 2);
        if (p + 4 + length > data.length)
          throw new Error("Invalid ZIP extra field");
        if (tag === 1) {
          let q = p + 4;
          const next = () => {
            if (q + 8 > p + 4 + length)
              throw new Error("Truncated ZIP64 field");
            const value = number64(data, q);
            q += 8;
            return value;
          };
          if (size === 0xffffffff) size = next();
          if (compressed === 0xffffffff) compressed = next();
          if (offset === 0xffffffff) offset = next();
        }
        p += 4 + length;
      }
      const local = await read(offset, 30);
      if (local.readUInt32LE(0) !== 0x04034b50)
        throw new Error("Invalid ZIP local record");
      offset += 30 + local.readUInt16LE(26) + local.readUInt16LE(28);
      if (names.has(name)) throw new Error(`Duplicate ZIP entry: ${name}`);
      names.add(name);
      entries.push({
        name,
        offset,
        size,
        compressed,
        method,
        crc: header.readUInt32LE(16),
      });
      cd += 46 + nameLength + extraLength + header.readUInt16LE(32);
    }
    for (const entry of entries)
      if (entry.offset + entry.compressed > size)
        throw new Error("Truncated ZIP payload");
    return entries;
  } finally {
    await file.close();
  }
}

export async function* entryChunks(
  path: string,
  entry: Entry,
): AsyncGenerator<Buffer> {
  if (!entry.compressed) return;
  const source = createReadStream(path, {
    start: entry.offset,
    end: entry.offset + entry.compressed - 1,
    highWaterMark: BLOCK,
  });
  const stream = entry.method === 8 ? source.pipe(createInflateRaw()) : source;
  if (stream !== source) source.on("error", (error) => stream.destroy(error));
  try {
    for await (const chunk of stream) yield Buffer.from(chunk);
  } finally {
    stream.destroy();
    source.destroy();
  }
}
const crcTable = Array.from({ length: 256 }, (_, n) => {
  for (let i = 0; i < 8; i++) n = n & 1 ? 0xedb88320 ^ (n >>> 1) : n >>> 1;
  return n >>> 0;
});
function crcUpdate(crc: number, bytes: Buffer) {
  for (const byte of bytes) crc = crcTable[(crc ^ byte) & 255] ^ (crc >>> 8);
  return crc;
}
function record(length: number, fields: [number, number, number | bigint][]) {
  const buffer = Buffer.alloc(length);
  for (const [offset, width, value] of fields) {
    if (width === 8) buffer.writeBigUInt64LE(BigInt(value), offset);
    else if (width === 4) buffer.writeUInt32LE(Number(value), offset);
    else buffer.writeUInt16LE(Number(value), offset);
  }
  return buffer;
}
// Python's xml.sax.saxutils.quoteattr chooses single quotes when possible.
function quote(value: string) {
  value = value
    .replaceAll("&", "&amp;")
    .replaceAll("<", "&lt;")
    .replaceAll(">", "&gt;")
    .replaceAll("\n", "&#10;")
    .replaceAll("\r", "&#13;")
    .replaceAll("\t", "&#9;");
  if (value.includes('"') && !value.includes("'")) return "'" + value + "'";
  return '"' + value.replaceAll('"', "&quot;") + '"';
}

// Match pack_appx.py: stored ZIP64 entries, descriptors, manifest first, 64 KiB hashes.
export async function repackAppx(
  input: string,
  directory: string,
  output: string,
) {
  if (resolve(input) === resolve(output))
    throw new Error("Input and output must differ");
  const entries = (await readEntries(input)).filter(
    (e) =>
      !e.name.endsWith("/") &&
      ![
        "AppxSignature.p7x",
        "AppxBlockMap.xml",
        "[Content_Types].xml",
      ].includes(e.name),
  );
  entries.sort(
    (a, b) =>
      Number(b.name === "AppxManifest.xml") -
        Number(a.name === "AppxManifest.xml") ||
      Buffer.compare(Buffer.from(a.name), Buffer.from(b.name)),
  );
  if (entries[0]?.name !== "AppxManifest.xml")
    throw new Error("AppxManifest.xml missing");
  for (const logo of logos) {
    if (!entries.some((e) => e.name === `Assets/${logo}.png`))
      throw new Error(`Missing package asset: ${logo}.png`);
    const file = Bun.file(join(directory, `${logo}.png`));
    const bytes = Buffer.from(await file.slice(0, 24).arrayBuffer());
    const dimensions: Record<string, [number, number]> = {
      StoreLogo: [50, 50],
      Square150x150Logo: [150, 150],
      Square44x44Logo: [44, 44],
      SplashScreen: [620, 300],
    };
    if (
      bytes.length !== 24 ||
      bytes.subarray(0, 8).toString("hex") !== "89504e470d0a1a0a" ||
      bytes.readUInt32BE(16) !== dimensions[logo][0] ||
      bytes.readUInt32BE(20) !== dimensions[logo][1]
    )
      throw new Error(`Invalid logo: ${logo}.png`);
  }
  const file = await open(output, "wx");
  let position = 0;
  const central: Buffer[] = [];
  const write = async (bytes: Buffer) => {
    let offset = 0;
    while (offset < bytes.length) {
      const { bytesWritten } = await file.write(
        bytes,
        offset,
        bytes.length - offset,
        position,
      );
      if (!bytesWritten) throw new Error("Could not write ZIP");
      offset += bytesWritten;
      position += bytesWritten;
    }
  };
  const add = async (
    arc: string,
    chunks: AsyncIterable<Buffer> | Iterable<Buffer>,
    hashes?: string[],
  ) => {
    const name = Buffer.from(arc),
      off = position;
    await write(
      Buffer.concat([
        record(30, [
          [0, 4, 0x04034b50],
          [4, 2, 45],
          [6, 2, 8],
          [12, 2, 0x21],
          [26, 2, name.length],
        ]),
        name,
      ]),
    );
    let crc = 0xffffffff,
      size = 0,
      pending = Buffer.alloc(BLOCK),
      used = 0;
    for await (const chunk of chunks) {
      crc = crcUpdate(crc, chunk);
      size += chunk.length;
      if (hashes)
        for (let p = 0; p < chunk.length;) {
          const length = Math.min(BLOCK - used, chunk.length - p);
          chunk.copy(pending, used, p, p + length);
          used += length;
          p += length;
          if (used === BLOCK) {
            hashes.push(createHash("sha256").update(pending).digest("base64"));
            used = 0;
          }
        }
      await write(chunk);
    }
    if (hashes && used)
      hashes.push(
        createHash("sha256").update(pending.subarray(0, used)).digest("base64"),
      );
    crc = (crc ^ 0xffffffff) >>> 0;
    await write(
      record(24, [
        [0, 4, 0x08074b50],
        [4, 4, crc],
        [8, 8, size],
        [16, 8, size],
      ]),
    );
    central.push(
      Buffer.concat([
        record(46, [
          [0, 4, 0x02014b50],
          [4, 2, 45],
          [6, 2, 45],
          [8, 2, 8],
          [14, 2, 0x21],
          [16, 4, crc],
          [20, 4, 0xffffffff],
          [24, 4, 0xffffffff],
          [28, 2, name.length],
          [30, 2, 28],
          [42, 4, 0xffffffff],
        ]),
        name,
        record(28, [
          [0, 2, 1],
          [2, 2, 24],
          [4, 8, size],
          [12, 8, size],
          [20, 8, off],
        ]),
      ]),
    );
    return { size, crc };
  };
  try {
    const blockmap = [
      '<?xml version="1.0" encoding="UTF-8" standalone="no"?>',
      '<BlockMap xmlns="http://schemas.microsoft.com/appx/2010/blockmap" HashMethod="http://www.w3.org/2001/04/xmlenc#sha256">',
    ];
    const extensions = new Set<string>();
    for (const entry of entries) {
      const replacement = logos.some(
        (logo) => entry.name === `Assets/${logo}.png`,
      );
      const hashes: string[] = [];
      const result = await add(
        entry.name,
        replacement
          ? createReadStream(join(directory, entry.name.slice(7)), {
              highWaterMark: BLOCK,
            })
          : entryChunks(input, entry),
        hashes,
      );
      if (
        !replacement &&
        (result.size !== entry.size || result.crc !== entry.crc)
      )
        throw new Error(`Corrupt ZIP entry: ${entry.name}`);
      blockmap.push(
        `<File Name=${quote(entry.name.replaceAll("/", "\\"))} Size="${result.size}" LfhSize="${30 + Buffer.byteLength(entry.name)}">`,
        ...hashes.map((hash) => `<Block Hash="${hash}"/>`),
        "</File>",
      );
      if (entry.name.includes("."))
        extensions.add(entry.name.split(".").at(-1)!.toLowerCase());
    }
    blockmap.push("</BlockMap>");
    await add("AppxBlockMap.xml", [Buffer.from(blockmap.join(""))]);
    const ct = [
      '<?xml version="1.0" encoding="UTF-8"?>',
      '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">',
      ...[...extensions]
        .sort()
        .filter((e) => e !== "xml")
        .map(
          (e) =>
            `<Default Extension=${quote(e)} ContentType="${types[e] ?? "application/octet-stream"}"/>`,
        ),
      '<Override PartName="/AppxManifest.xml" ContentType="application/vnd.ms-appx.manifest+xml"/>',
      '<Override PartName="/AppxBlockMap.xml" ContentType="application/vnd.ms-appx.blockmap+xml"/>',
      '<Override PartName="/AppxSignature.p7x" ContentType="application/vnd.ms-appx.signature"/>',
      "</Types>",
    ];
    await add("[Content_Types].xml", [Buffer.from(ct.join(""))]);
    const cd = position;
    for (const entry of central) await write(entry);
    const end = position,
      n = central.length;
    await write(
      record(56, [
        [0, 4, 0x06064b50],
        [4, 8, 44],
        [12, 2, 45],
        [14, 2, 45],
        [24, 8, n],
        [32, 8, n],
        [40, 8, end - cd],
        [48, 8, cd],
      ]),
    );
    await write(
      record(20, [
        [0, 4, 0x07064b50],
        [8, 8, end],
        [16, 4, 1],
      ]),
    );
    await write(
      record(22, [
        [0, 4, 0x06054b50],
        [8, 2, 0xffff],
        [10, 2, 0xffff],
        [12, 4, 0xffffffff],
        [16, 4, 0xffffffff],
      ]),
    );
  } catch (error) {
    await file.close();
    await rm(output, { force: true });
    throw error;
  }
  await file.close();
}

export async function repackAndSign(
  input: string,
  directory: string,
  output: string,
  pfx: string,
) {
  const ossl = Bun.which("osslsigncode"),
    signtool = process.platform === "win32" ? Bun.which("signtool") : null;
  if (!ossl && !signtool)
    throw new Error(
      "Install osslsigncode or Windows signtool to sign the package",
    );
  if (!(await Bun.file(pfx).exists()))
    throw new Error(`Signing certificate missing: ${pfx}`);
  if (resolve(input) === resolve(output))
    throw new Error("Input and output must differ");
  const temp = await mkdtemp(join(dirname(resolve(output)), ".cover-repack-"));
  try {
    const unsigned = join(temp, "unsigned.appx"),
      signed = join(temp, "signed.appx");
    await repackAppx(input, directory, unsigned);
    const command = ossl
      ? [
          ossl,
          "sign",
          "-pkcs12",
          pfx,
          "-pass",
          "",
          "-h",
          "sha256",
          "-in",
          unsigned,
          "-out",
          signed,
        ]
      : [signtool!, "sign", "/fd", "SHA256", "/f", pfx, "/p", "", unsigned];
    const child = Bun.spawn(command, { stdout: "inherit", stderr: "inherit" });
    if ((await child.exited) !== 0) throw new Error("Package signing failed");
    await rename(ossl ? signed : unsigned, resolve(output));
  } finally {
    await rm(temp, { recursive: true, force: true });
  }
}
