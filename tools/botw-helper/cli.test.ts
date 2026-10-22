import { describe, expect, test } from "bun:test";
import { mkdtemp, rm, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { validateConfig, versionParts } from "./cli";

describe("helper protocol", () => {
  test("validates keys, ranges and types", () => {
    expect(() =>
      validateConfig({ autoLaunch: true, volume: 100, fps: 30, wallpaper: -1 }),
    ).not.toThrow();
    for (const config of [
      null,
      [],
      { setupComplete: true },
      { autoLaunch: 1 },
      { volume: 101 },
      { fps: 45 },
      { wallpaper: -2 },
      { upscaler: 1.5 },
    ]) {
      expect(() => validateConfig(config)).toThrow();
    }
    expect(
      versionParts({ PackageFullName: "BotWRecompiled_10.2.3.4_x64__test" }),
    ).toEqual([10, 2, 3, 4]);
    expect(
      versionParts({ Version: { Major: 2, Minor: 3, Build: 4, Revision: 5 } }),
    ).toEqual([2, 3, 4, 5]);
  });

  test("CLI selects newest exact identity and uses LocalState for uploads", async () => {
    const directory = await mkdtemp(join(tmpdir(), "botw-helper-test-"));
    try {
      const client = join(directory, "portal.ts");
      const log = join(directory, "calls.jsonl");
      await writeFile(
        client,
        `
        import { appendFileSync } from 'node:fs';
        const log = (entry) => appendFileSync(process.env.BOTW_TEST_LOG, JSON.stringify(entry) + '\\n');
        export class DevicePortal {
          constructor(config) { log(['connection', config]); }
          async packages() { return [
            {Name:'BotWRecompiled', PackageFullName:'BotWRecompiled_1.0.0.0_x64__test'},
            {Name:'Other', PackageFullName:'Other_99.0.0.0_x64__test'},
            {Name:'BotWRecompiled', PackageFullName:'BotWRecompiled_10.0.0.0_x64__test'}
          ]; }
          async pushFile(...args) { log(['push', ...args]); }
          async launch(pkg) { log(['launch', pkg.PackageFullName]); }
        }
      `,
      );
      const configPath = join(directory, "helper.json");
      await writeFile(
        configPath,
        JSON.stringify({ autoLaunch: true, fps: 60 }),
      );
      const invoke = async (args: string[]) => {
        const child = Bun.spawn(
          [
            process.execPath,
            join(import.meta.dir, "cli.ts"),
            ...args,
            "--host",
            "192.0.2.1:12345",
            "--user",
            "test",
          ],
          {
            env: {
              ...process.env,
              BOTW_PORTAL_CLIENT: client,
              BOTW_TEST_LOG: log,
              XBDEV_PASS: "synthetic-password",
            },
            stdout: "pipe",
            stderr: "pipe",
          },
        );
        const stderr = await new Response(child.stderr).text();
        expect(await child.exited, stderr).toBe(0);
      };
      await invoke(["push-config", configPath]);
      // Only the PNG header and dimensions are needed by this CLI transport test.
      for (const [name, width] of [
        ["cover.png", 300],
        ["cover-wide.png", 620],
      ] as const) {
        const bytes = Buffer.alloc(24);
        Buffer.from("89504e470d0a1a0a", "hex").copy(bytes);
        bytes.writeUInt32BE(width, 16);
        bytes.writeUInt32BE(300, 20);
        await writeFile(join(directory, name), bytes);
      }
      await invoke(["push-cover", directory]);
      await invoke(["launch"]);
      const calls = (await Bun.file(log).text())
        .trim()
        .split("\n")
        .map((line) => JSON.parse(line));
      const pushes = calls.filter((call) => call[0] === "push");
      expect(pushes).toHaveLength(3);
      for (const call of pushes) {
        expect(call[1]).toBe("BotWRecompiled_10.0.0.0_x64__test");
        expect(call[3]).toBe("/LocalState");
      }
      expect(calls.find((call) => call[0] === "launch")).toEqual([
        "launch",
        "BotWRecompiled_10.0.0.0_x64__test",
      ]);
      expect(calls[0][1]).toEqual({
        host: "192.0.2.1",
        port: 12345,
        user: "test",
        pass: "synthetic-password",
      });
    } finally {
      await rm(directory, { recursive: true, force: true });
    }
  });
});
