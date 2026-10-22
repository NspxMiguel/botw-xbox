# BotW Helper

© 2026 NSPX

## Português

Companion de desktop para configurar o launcher BotW no Xbox em Dev Mode, usando Rust + eframe/egui e uma CLI Bun. O idioma e o tema seguem o sistema. Não inclui imagens, arquivos de jogo ou chaves da Nintendo.

### Executar

Instale Rust e Bun. Mantenha este checkout e o cliente `xboxdev` lado a lado conforme a estrutura abaixo:

```text
nxbox/
  xboxdev/src/portal.ts
  botw-wiiu/helper-wt/tools/botw-helper/
```

```sh
cd tools/botw-helper
cargo build --release
./target/release/BotWHelper
```

O aplicativo executa `bun cli.ts` em uma thread de trabalho. O caminho da CLI usa este checkout no momento do build. Para movê-la, defina `BOTW_HELPER_CLI` com o caminho completo de `cli.ts` e `BOTW_PORTAL_CLIENT` com o caminho de `xboxdev/src/portal.ts`; preserve também o manifesto em `../../runtime/src/host/uwp/Package.appxmanifest`, relativo à CLI. Bun precisa estar no `PATH` do aplicativo. No Windows, execute `target/release/BotWHelper.exe`.

### Usar

1. No Xbox, ative o Device Portal em **Dev Home › Remote Access** e configure usuário e senha.
2. No PC na mesma rede, copie o endereço exibido no Xbox (IP ou `IP:porta`; porta padrão `11443`). Informe usuário e senha.
3. Ajuste o launcher e, se quiser, selecione PNG/JPG ou a pasta do seu próprio jogo Wii U que contém `meta/`.
4. Clique em **Enviar ao Xbox**. Feche e abra o aplicativo no Xbox para importar os arquivos. **Abrir aplicativo** solicita a abertura pelo Portal; não reinicia uma instância já aberta.

O endereço e o usuário são salvos em `BotWHelper/connection.json` na pasta de configuração do sistema (`~/Library/Application Support` no macOS, `%APPDATA%` no Windows). A senha só entra nesse arquivo quando **Lembrar senha neste computador** está marcado; desmarcar remove a senha salva imediatamente. O arquivo contém texto simples, com permissões somente do usuário no Unix. A senha é transmitida ao subprocesso pelo ambiente, nunca por argumento. Não compartilhe esse arquivo. As operações têm limite de 60 segundos cada.

**Manter padrão** não envia capas e mantém a imagem atual do Xbox. Para restaurar o padrão original após uma capa personalizada, remova `cover.png` e `cover-wide.png` de LocalState e limpe a notificação do tile no console. Não há imagens padrão da Nintendo no helper.

A capa quadrada é um recorte central de 300×300; a larga, de 620×300. Para o jogo próprio, são lidos `meta/iconTex.tga` e `meta/bootTvTex.tga`, incluindo TGA raw/RLE de 24/32 bits. As imagens geradas ficam em uma pasta temporária que é removida ao encerrar a seleção ou o aplicativo.

### CLI

```sh
bun tools/botw-helper/cli.ts --help
XBDEV_HOST=192.0.2.1 XBDEV_USER=dev bun tools/botw-helper/cli.ts push-config /path/to/helper.json
XBDEV_HOST=192.0.2.1 XBDEV_USER=dev bun tools/botw-helper/cli.ts push-cover /path/to/covers
XBDEV_HOST=192.0.2.1 XBDEV_USER=dev bun tools/botw-helper/cli.ts launch
```

Defina `XBDEV_PASS` no ambiente para autenticação. Também existem `--host` e `--user`; não existe argumento de senha. O cliente existente trata HTTPS com certificado local e CSRF. A CLI seleciona a maior versão instalada cuja identidade coincide exatamente com o manifesto e envia para `/LocalState` pelo Device Portal.

O JSON enviado contém exatamente estas nove chaves:

```json
{
  "autoLaunch": false,
  "fps": 60,
  "volume": 100,
  "buttonLayout": 1,
  "wallpaper": -1,
  "renderScalePercent": 0,
  "upscaler": 0,
  "fsrQuality": 0,
  "frameGeneration": 0
}
```

O console importa os valores conhecidos, preserva a configuração inicial e renomeia o JSON para `helper.applied.json`. JSON inválido ou tipos incorretos são rejeitados sem aplicar valores e renomeados para `helper.rejected.json`. A capa atualiza o tile somente quando a data de modificação muda. Falhas são registradas e o launcher continua abrindo.

## English

Desktop companion for the BotW launcher on an Xbox in Dev Mode. Built with Rust + eframe/egui, with a Bun CLI over the existing Device Portal client. Language and appearance follow the OS. No Nintendo artwork, game files, or keys are included.

### Build and run

Install Rust and Bun, keep the checkout layout shown above, then run:

```sh
cd tools/botw-helper
cargo build --release
./target/release/BotWHelper
```

On Windows, run `target/release/BotWHelper.exe`. Bun must be on the app's `PATH`. `BOTW_HELPER_CLI` overrides the build-time absolute CLI path; `BOTW_PORTAL_CLIENT` overrides the default sibling `xboxdev/src/portal.ts`. If moving the CLI, preserve its relative path to the manifest (`../../runtime/src/host/uwp/Package.appxmanifest`). This is a source-checkout application, not a standalone installer.

Enable Device Portal in **Dev Home › Remote Access**, then enter the console address and Portal credentials on a PC on the same network. Use an IP or `IP:port` (default port `11443`). Choose preferences and optional cover art, then **Send to Xbox**. Reopen the console app to import them. **Launch app** requests activation and does not restart an already running instance.

The connection file is stored in the OS configuration directory under `BotWHelper/connection.json` (`~/Library/Application Support` on macOS; `%APPDATA%` on Windows). Passwords are saved in plain text only while **Remember password on this computer** is checked. Unchecking immediately removes the saved password. Unix files are owner-only. The password reaches Bun through its environment, never command arguments. Do not share the connection file. Each operation times out after 60 seconds.

Choose a PNG/JPG or your own Wii U game folder containing `meta/iconTex.tga` and `meta/bootTvTex.tga`. Raw and RLE TGA, 24 and 32 bits, are supported. Center crops produce `cover.png` (300×300) and `cover-wide.png` (620×300) in temporary storage, cleaned up when the selection is replaced or the app exits. **Keep default** skips artwork uploads and preserves the current Xbox tile. To restore the original default after customization, remove both cover files from LocalState and clear the console tile notification.

The CLI commands and JSON example above also apply in English. Use `XBDEV_PASS` for the password, `XBDEV_HOST` / `--host` for the address, and `XBDEV_USER` / `--user` for the username. The shared client handles the console's self-signed HTTPS certificate and CSRF. The highest version matching the manifest identity is selected, and files go to `/LocalState`.

The console imports known settings before reading launcher preferences, preserves setup completion, and renames a successful import to `helper.applied.json`. Invalid JSON or incorrect value types are rejected as `helper.rejected.json` without applying values. Cover modification dates prevent repeated tile updates. Import and tile errors are logged without stopping startup.

### Validation / Validação

```sh
cd tools/botw-helper
cargo fmt --check
cargo build --release
cargo test --release
bun test cli.test.ts
bun cli.ts --help
```

Rust tests decode synthetic raw/RLE 24/32-bit TGA images, assert RGBA pixels and PNG dimensions, verify the exact JSON schema and omission of unremembered passwords. CLI tests use a fake Portal to verify package selection, upload paths and launch without contacting an Xbox. UWP must be built separately from the main checkout; this worktree does not build or deploy the console package.
