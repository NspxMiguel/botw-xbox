import { expect, test } from "bun:test";
import { mkdtemp, rm } from "node:fs/promises";
import { join, resolve } from "node:path";
import { tmpdir } from "node:os";
import { repackAppx } from "./appx";

async function python(code: string, ...args: string[]) {
  const process = Bun.spawn(["python3", "-c", code, ...args], {
    stdout: "pipe",
    stderr: "pipe",
  });
  const [stdout, stderr, status] = await Promise.all([
    new Response(process.stdout).text(),
    new Response(process.stderr).text(),
    process.exited,
  ]);
  if (status) throw new Error(stderr);
  return stdout;
}

for (const deflated of [false, true]) {
  test(`repack ${deflated ? "deflated" : "stored ZIP64"} appx matches Python packer and independent hashes`, async () => {
    const directory = await mkdtemp(join(tmpdir(), "botw-appx-"));
    try {
      const input = join(directory, "input.appx"),
        output = join(directory, "output.appx"),
        reference = join(directory, "reference.appx");
      const packer = resolve(import.meta.dir, "../pack_appx.py");
      await python(
        `
import os, sys, struct, zlib, subprocess, zipfile
root, packer, deflated = sys.argv[1:]
stage = os.path.join(root, 'stage')
cover = os.path.join(root, 'covers')
os.makedirs(os.path.join(stage, 'Assets'))
os.makedirs(cover)
def png(w, h):
    def chunk(tag, data):
        return struct.pack('>I',len(data)) + tag + data + struct.pack('>I',zlib.crc32(tag + data))
    return b'\\x89PNG\\r\\n\\x1a\\n' + chunk(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0)) + chunk(b'IDAT',zlib.compress((b'\\0' + b'\\xff\\0\\0' * w)*h)) + chunk(b'IEND',b'')
for name,w,h in [('StoreLogo',50,50),('Square150x150Logo',150,150),('Square44x44Logo',44,44),('SplashScreen',620,300)]:
    open(os.path.join(stage,'Assets',name+'.png'),'wb').write(b'old logo')
    open(os.path.join(cover,name+'.png'),'wb').write(png(w,h))
open(os.path.join(stage,'AppxManifest.xml'),'w').write('<Package><Identity Name="Example" Publisher="CN=Test"/></Package>')
open(os.path.join(stage,'payload.bin'),'wb').write(bytes(range(256))*700)
open(os.path.join(stage,'empty.txt'),'wb').close()
open(os.path.join(stage,'Assets','a&b.txt'),'wb').write(b'escaped name')
open(os.path.join(stage,'Assets','a"b.txt'),'wb').write(b'quoted name')
subprocess.run([sys.executable,packer,stage,os.path.join(root,'stored.appx')],check=True)
if deflated == 'true':
    with zipfile.ZipFile(os.path.join(root,'stored.appx')) as source, zipfile.ZipFile(os.path.join(root,'input.appx'),'w',compression=zipfile.ZIP_DEFLATED) as target:
        for name in source.namelist(): target.writestr(name,source.read(name))
        target.writestr('AppxSignature.p7x',b'obsolete signature')
else:
    os.rename(os.path.join(root,'stored.appx'),os.path.join(root,'input.appx'))
for name in os.listdir(cover):
    open(os.path.join(stage,'Assets',name),'wb').write(open(os.path.join(cover,name),'rb').read())
subprocess.run([sys.executable,packer,stage,os.path.join(root,'reference.appx')],check=True)
`,
        directory,
        packer,
        String(deflated),
      );
      await repackAppx(input, join(directory, "covers"), output);
      expect(
        await python(
          `
import sys, zipfile, xml.etree.ElementTree as ET, hashlib, base64, os
path, covers, reference = sys.argv[1:]
assert open(path,'rb').read() == open(reference,'rb').read(), 'Output differs from pack_appx.py'
with zipfile.ZipFile(path) as z:
    assert z.testzip() is None
    assert 'AppxSignature.p7x' not in z.namelist()
    for name in os.listdir(covers): assert z.read('Assets/'+name) == open(os.path.join(covers,name),'rb').read()
    bm = ET.fromstring(z.read('AppxBlockMap.xml'))
    ns = '{http://schemas.microsoft.com/appx/2010/blockmap}'
    assert len(bm) == len(z.namelist()) - 2
    for f in bm:
        name = f.attrib['Name'].replace(chr(92),'/')
        data = z.read(name)
        assert len(data) == int(f.attrib['Size'])
        assert int(f.attrib['LfhSize']) == 30 + len(name.encode())
        assert [b.attrib['Hash'] for b in f] == [base64.b64encode(hashlib.sha256(data[i:i+65536]).digest()).decode() for i in range(0,len(data),65536)]
    ct = ET.fromstring(z.read('[Content_Types].xml'))
    assert any(t.attrib.get('Extension') == 'png' and t.attrib['ContentType'] == 'image/png' for t in ct)
    assert any(t.attrib.get('PartName') == '/AppxManifest.xml' and t.attrib['ContentType'].endswith('+xml') for t in ct)
print('verified')
`,
          output,
          join(directory, "covers"),
          reference,
        ),
      ).toContain("verified");
      await expect(
        repackAppx(input, join(directory, "covers"), input),
      ).rejects.toThrow("must differ");
      if (!deflated) {
        await python(
          `
import zipfile, sys
path = sys.argv[1]
with zipfile.ZipFile(path) as z:
    entry = z.getinfo('payload.bin')
    offset = entry.header_offset + 30 + len(entry.filename.encode()) + len(entry.extra)
with open(path, 'r+b') as f:
    f.seek(offset)
    byte = f.read(1)
    f.seek(offset)
    f.write(bytes([byte[0] ^ 255]))
`,
          input,
        );
        const corruptOutput = join(directory, "corrupt.appx");
        await expect(
          repackAppx(input, join(directory, "covers"), corruptOutput),
        ).rejects.toThrow("Corrupt ZIP entry");
        expect(await Bun.file(corruptOutput).exists()).toBe(false);
      }
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });
}
