#!/usr/bin/env bun
import { resolve, basename } from "node:path";
import { pathToFileURL } from "node:url";

const portuguese = Intl.DateTimeFormat()
  .resolvedOptions()
  .locale.toLowerCase()
  .startsWith("pt");
const t = (pt: string, en: string) => (portuguese ? pt : en);
const usage = t(
  `BotW Helper — © 2026 NSPX
Uso: bun cli.ts <push-config|push-cover|launch> [arquivo ou pasta] --host <IP[:porta]> [--user <usuário>]
  push-config helper.json  Envia as preferências para LocalState.
  push-cover <pasta>      Envia cover.png e cover-wide.png.
  launch                  Abre o aplicativo instalado.
Senha: variável XBDEV_PASS. Endereço/usuário: XBDEV_HOST / XBDEV_USER.
BOTW_PORTAL_CLIENT pode apontar para xboxdev/src/portal.ts.`,
  `BotW Helper — © 2026 NSPX
Usage: bun cli.ts <push-config|push-cover|launch> [file or directory] --host <IP[:port]> [--user <user>]
  push-config helper.json  Upload preferences to LocalState.
  push-cover <directory>   Upload cover.png and cover-wide.png.
  launch                   Launch the installed application.
Password: XBDEV_PASS environment variable. Address/user: XBDEV_HOST / XBDEV_USER.
BOTW_PORTAL_CLIENT can point to xboxdev/src/portal.ts.`,
);

export const ranges: Record<string, [number, number]> = {
  fps: [30, 60],
  volume: [0, 100],
  buttonLayout: [0, 1],
  wallpaper: [-1, 3],
  renderScalePercent: [0, 300],
  upscaler: [0, 3],
  fsrQuality: [0, 4],
  frameGeneration: [0, 2],
};

export function validateConfig(value: unknown): void {
  if (!value || typeof value !== "object" || Array.isArray(value))
    throw new Error("Invalid config object");
  const config = value as Record<string, unknown>;
  for (const [key, item] of Object.entries(config)) {
    if (key === "autoLaunch" || key === "debugHud") {
      if (typeof item !== "boolean") throw new Error(`Invalid ${key}`);
    } else {
      const range = ranges[key];
      if (
        !range ||
        typeof item !== "number" ||
        !Number.isInteger(item) ||
        item < range[0] ||
        item > range[1] ||
        (key === "fps" && item !== 30 && item !== 60)
      ) {
        throw new Error(`Invalid setting: ${key}`);
      }
    }
  }
}

export function versionParts(pkg: any): number[] {
  const version = pkg.Version;
  if (version && typeof version === "object") {
    return [version.Major, version.Minor, version.Build, version.Revision].map(
      (v) => Number(v) || 0,
    );
  }
  return String(
    typeof version === "string"
      ? version
      : (pkg.PackageFullName.split("_")[1] ?? "0"),
  )
    .split(".")
    .map((v) => Number(v) || 0);
}

async function main() {
  const args = Bun.argv.slice(2);
  if (!args.length || args.includes("--help") || args.includes("-h")) {
    console.log(usage);
    return;
  }
  const command = args.shift()!;
  if (!["push-config", "push-cover", "launch"].includes(command))
    throw new Error("Unknown command");
  let host = process.env.XBDEV_HOST ?? "";
  let user = process.env.XBDEV_USER;
  let input = "";
  while (args.length) {
    const argument = args.shift()!;
    if (argument === "--host" || argument === "--user") {
      const value = args.shift();
      if (!value || value.startsWith("--"))
        throw new Error(`Missing ${argument}`);
      if (argument === "--host") host = value;
      else user = value;
    } else if (!argument.startsWith("-") && !input) input = argument;
    else throw new Error(`Unexpected argument: ${argument}`);
  }
  if (!host.trim())
    throw new Error(
      "Set --host to the address in Xbox Dev Home > Remote Access",
    );
  const url = new URL(host.includes("://") ? host : `https://${host}`);
  if (
    url.protocol !== "https:" ||
    url.username ||
    url.password ||
    url.pathname !== "/" ||
    url.search ||
    url.hash
  )
    throw new Error(
      "Expected an HTTPS Portal host, without credentials or path",
    );
  const clientPath =
    process.env.BOTW_PORTAL_CLIENT ??
    resolve(import.meta.dir, "../../../../xboxdev/src/portal.ts");
  const { DevicePortal } = await import(pathToFileURL(clientPath).href);
  const portal = new DevicePortal({
    host: url.hostname,
    port: Number(url.port || process.env.XBDEV_PORT || 11443),
    user,
    pass: process.env.XBDEV_PASS,
  });
  const manifest = await Bun.file(
    resolve(import.meta.dir, "../../runtime/src/host/uwp/Package.appxmanifest"),
  ).text();
  const identity = manifest.match(/<Identity\s[^>]*\bName="([^"]+)"/)?.[1];
  if (!identity) throw new Error("App identity missing from manifest");
  // Validate local inputs before making any changes on the console.
  let files: string[] = [];
  if (command === "push-config") {
    if (basename(input) !== "helper.json")
      throw new Error("Expected helper.json");
    validateConfig(await Bun.file(input).json());
    files = [resolve(input)];
  } else if (command === "push-cover") {
    if (!input) throw new Error("Expected cover directory");
    files = ["cover.png", "cover-wide.png"].map((name) => resolve(input, name));
    for (const file of files) {
      const bytes = new Uint8Array(await Bun.file(file).arrayBuffer());
      const view = new DataView(
        bytes.buffer,
        bytes.byteOffset,
        bytes.byteLength,
      );
      const width = basename(file) === "cover.png" ? 300 : 620;
      if (
        bytes.length < 24 ||
        Buffer.from(bytes.slice(0, 8)).toString("hex") !== "89504e470d0a1a0a" ||
        view.getUint32(16) !== width ||
        view.getUint32(20) !== 300
      )
        throw new Error(`Invalid cover: ${basename(file)}`);
    }
  }
  const packages = (await portal.packages()).filter(
    (pkg: any) => String(pkg.PackageFullName).startsWith(`${identity}_`), // Portal Name is the display name
  );
  packages.sort((a: any, b: any) => {
    const av = versionParts(a),
      bv = versionParts(b);
    for (let i = 0; i < 4; i++) if (av[i] !== bv[i]) return bv[i] - av[i];
    return 0;
  });
  const pkg = packages[0];
  if (!pkg) throw new Error("BotW app is not installed on this console");
  for (const file of files)
    // The shared client extracts names using forward slashes, including on Windows.
    await portal.pushFile(
      pkg.PackageFullName,
      file.replaceAll("\\", "/"),
      "/LocalState",
    );
  if (command === "launch") await portal.launch(pkg);
  console.log(t("Concluído.", "Done."));
}

if (import.meta.main) {
  main().catch((error) => {
    console.error(
      t(
        "Não foi possível concluir. Confira o endereço e as credenciais do Portal.",
        "Could not complete. Check the Portal address and credentials.",
      ),
    );
    console.error(error instanceof Error ? error.message : String(error));
    process.exitCode = 1;
  });
}
