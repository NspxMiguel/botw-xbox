# botw-xbox

**English** · [Português](#português)

The Legend of Zelda: Breath of the Wild (Wii U version), statically recompiled from PowerPC to native
x86-64 and running on Xbox Series X as a UWP app with a Direct3D 12 renderer. The CPU is not
emulated: the game code is translated ahead of time into C++ and compiled.

**Source only.** This repository contains no game files, keys, recompiled game code or Nintendo assets,
and there is no download. A playable package can only be built locally from your own legally obtained
copy of the game. Nothing will be published for players before the project reaches its release bar:
60 fps locked, no shader-compilation stutter, 4K and the visual upgrades in place.

## Status (measured on Xbox Series X, October 2026)

| | |
| --- | --- |
| Performance | **45 fps** at a fixed spot on the Great Plateau with the Remaster preset; 33–56 fps while walking. Not 60 yet. |
| Resolution | Native 1080p rendering. Upscaling is off by default; FSR 1 or FSR 3.1 can be chosen in the quick menu. |
| Bottleneck | The CPU thread that turns the game's GPU commands into Direct3D 12 work (~21.7 ms per frame). The GPU is mostly idle. |
| Series S | Not tested on real hardware. |

**Works:**
- boot to gameplay, saves and gamepad input;
- in-app settings and a mods store;
- a custom dashboard cover, via the BotW Helper desktop companion;
- an in-app video recorder;
- a debug overlay showing fps, frame time and memory.

**Known issues**
- Below 60 fps in demanding areas.
- The "Remaster" graphic-pack preset (FPS++, shadows, draw distance, reflections) is compiled in, but not every category is verified on the console yet.
- Shaders are precompiled at boot; the first minutes after boot are not smooth.
- Mods on USB storage cannot be read yet.
- The Wii U GamePad screen is not shown on the TV yet.

**60 fps:** the remaining work is splitting the GPU-command thread into two overlapping threads
([docs/latte-pipelining.md](docs/latte-pipelining.md)). Rough estimate: 4–8 weeks. This is an
estimate, not a promise.

## Building

Players: see [BUILDING.md](BUILDING.md) for the four local builder commands.

See [docs/CI.md](docs/CI.md). CI builds only the game-free runtime. Recompiling the game happens on
your own computer from your own already extracted and decrypted dump; the generated code and the
resulting package must stay there. Setup accepts decrypted `code/`, `content/`, `meta/` folders,
already decrypted `.wua` archives and `.zip` folders. Extract `.7z` or an archive containing `.wua`
on a PC first. Base + update 1.5.0 (v208) + optional DLC merging and USB auto-detection are supported.
Launcher artwork comes from your dump's `meta/` files; disc art is fetched from GameTDB when online.

## Credits and license

- Static recompiler and original runtime: [BotWRecomp](https://github.com/IshaanKalra2103/BotWRecomp)
  by Ishaan Kalra (MIT).
- GPU emulation: [Cemu](https://github.com/cemu-project/Cemu) (MPL 2.0, [runtime/cemu/LICENSE.txt](runtime/cemu/LICENSE.txt));
  the D3D12 backend is ported from Cemu-UWP (see [runtime/cemu/VENDORED.md](runtime/cemu/VENDORED.md)).
- Graphic packs: [cemu_graphic_packs](https://github.com/cemu-project/cemu_graphic_packs) (CC0), not bundled.
- AMD FidelityFX (MIT).
- Xbox port: NSPX ([nspxmiguel](https://github.com/NspxMiguel)).

This project's own code is MIT licensed ([LICENSE](LICENSE)); third-party components keep their own
licenses. See [LEGAL.md](LEGAL.md). Not affiliated with or endorsed by Nintendo or Microsoft. Breath of the Wild is a trademark of
Nintendo.

---

## Português

The Legend of Zelda: Breath of the Wild (versão de Wii U), recompilado estaticamente de PowerPC para
x86-64 nativo, rodando no Xbox Series X como app UWP com renderizador Direct3D 12. Não há emulação da
CPU: o código do jogo é traduzido antes para C++ e compilado.

**Só código-fonte.** Este repositório não tem arquivos do jogo, chaves, código do jogo recompilado nem
material da Nintendo, e não há download. Um pacote jogável só pode ser montado no seu computador, a
partir da sua própria cópia legal do jogo. Nada será publicado para jogar antes de o projeto chegar ao
ponto de lançamento: 60 fps cravados, sem travadas de compilação de shader, 4K e as melhorias visuais.

## Situação (medida no Xbox Series X, outubro de 2026)

| | |
| --- | --- |
| Desempenho | **45 fps** num ponto fixo do Planalto Esquecido com o preset Remaster; 33–56 fps andando. Ainda não é 60. |
| Resolução | 1080p nativo. Upscaling desligado por padrão; FSR 1 ou FSR 3.1 podem ser escolhidos no menu rápido. |
| Gargalo | A thread da CPU que transforma os comandos de GPU do jogo em Direct3D 12 (~21,7 ms por quadro). A GPU fica quase parada. |
| Series S | Ainda não testado no aparelho. |

**Funciona:**
- abrir até o gameplay, saves e controle;
- configurações no app e loja de mods;
- capa personalizada no painel do Xbox, pelo BotW Helper;
- gravador de vídeo;
- painel de debug com fps, tempo de quadro e memória.

**Problemas conhecidos**
- Abaixo de 60 fps em áreas pesadas.
- O preset "Remaster" de packs gráficos (FPS++, sombras, distância de visão, reflexos) já é compilado junto, mas nem toda categoria foi conferida no console.
- Os shaders são pré-compilados ao abrir; os primeiros minutos não são lisos.
- Mods em pendrive ainda não são lidos.
- A tela do Wii U GamePad ainda não aparece na TV.

**60 fps:** falta dividir a thread de comandos de GPU em duas threads trabalhando ao mesmo tempo
([docs/latte-pipelining.md](docs/latte-pipelining.md)). Estimativa: 4–8 semanas. É estimativa, não
promessa.

## Como montar o pacote

Veja [BUILDING.md em português](BUILDING.md#português-brasileiro) para os quatro comandos locais.

## Créditos e licença

- Recompilador e runtime original: BotWRecomp, de Ishaan Kalra (MIT).
- Emulação de GPU: Cemu (MPL 2.0); o backend D3D12 vem do Cemu-UWP.
- Packs gráficos: cemu_graphic_packs (CC0), não incluídos.
- AMD FidelityFX (MIT).
- Porte para Xbox: NSPX ([nspxmiguel](https://github.com/NspxMiguel)).

O código próprio está sob licença MIT; componentes de terceiros mantêm suas licenças. Sem vínculo com a
Nintendo ou a Microsoft. Breath of the Wild é marca da Nintendo. Veja [LEGAL.md](LEGAL.md#português).
