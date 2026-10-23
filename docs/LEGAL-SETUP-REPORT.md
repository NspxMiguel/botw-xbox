# Decrypted game setup report

Setup now accepts game files the player has already extracted and decrypted from their own Wii U.
The key handling and encrypted source readers, their diagnostics and fixtures, the Python disc
extractor and its tests, and runtime artwork fetching have been removed. The four guide pages
contain neutral preparation instructions in English and Portuguese, without external guide links.

## Player behavior

A player with a decrypted `code/`, `content/`, `meta/` dump keeps USB auto-scan, manual folder picking,
the RPX SHA-256 version check, base plus update v208 merging, optional DLC mapping to `aoc/`, USB play,
internal import, resumable extraction and automatic launch. Retained format IDs preserve the
identity of existing extraction journals.

Direct `.wua` import and ZIP archives containing decrypted folders remain available. `.7z` still
requires PC extraction, as it did before this change. ZIP archives containing `.wua` are now recognized
and receive the same PC extraction guidance; direct nested archive extraction was not implemented
in the previous setup reader. Once extracted on a PC, the resulting folder or `.wua` follows the
normal setup path.

Artwork uses `meta/iconTex.tga`, `meta/bootTvTex.tga` and `meta/bootDrcTex.tga`, with the existing local
fallback. Previously downloaded artwork is no longer read or refreshed; setup performs no artwork
network requests. The generic QR utility remains, with a neutral URL fixture, but setup generates no QR.

## Setup screens in English and Portuguese

The following strings are transcribed from `runtime/src/host/uwp/strings.h`. Line breaks are shown
inside each cell. A guide page has its page count and Continue action. Progress screens add the
cancel hint below. The OS folder picker keeps its system-provided text.

| Screen | English | Portuguese |
| --- | --- | --- |
| Welcome | Welcome<br>Play your own Wii U copy of Breath of the Wild on Xbox.<br>Use game files you already extracted and decrypted yourself, updated to 1.5.0 (v208).<br>Continue | Boas-vindas<br>Jogue sua cópia de Breath of the Wild do Wii U no Xbox.<br>Use os arquivos que você já extraiu e descriptografou, atualizados para 1.5.0 (v208).<br>Continuar |
| Guide 1 | Prepare your game files<br>Use a copy of the game you extracted yourself from your own Wii U.<br>Start with a decrypted folder containing code/, content/ and meta/.<br>1 / 4<br>Continue | Prepare os arquivos do jogo<br>Use uma cópia do jogo que você mesmo extraiu do seu próprio Wii U.<br>Comece com uma pasta descriptografada contendo code/, content/ e meta/.<br>1 / 4<br>Continuar |
| Guide 2 | Prepare your game files<br>Use your already extracted and decrypted base game and update 1.5.0 (v208).<br>Include your decrypted DLC if you own it; DLC is optional.<br>2 / 4<br>Continue | Prepare os arquivos do jogo<br>Use o jogo base e a atualização 1.5.0 (v208) que você já extraiu e descriptografou.<br>Inclua seu DLC descriptografado se você o tiver; ele é opcional.<br>2 / 4<br>Continuar |
| Guide 3 | Prepare your game files<br>Copy the base game, then copy the update over it, replacing files.<br>Keep the DLC content/ as aoc/, beside code/, content/ and meta/.<br>3 / 4<br>Continue | Prepare os arquivos do jogo<br>Copie o jogo base e depois a atualização por cima, substituindo arquivos.<br>Guarde content/ do DLC como aoc/, ao lado de code/, content/ e meta/.<br>3 / 4<br>Continuar |
| Guide 4 | Prepare your game files<br>Copy the merged decrypted folder to a USB drive as BotW/.<br>You can also use a decrypted .wua or a .zip containing a decrypted folder. Extract .7z or an archive containing .wua on a PC first.<br>Keep the USB drive connected during setup.<br>4 / 4<br>Continue | Prepare os arquivos do jogo<br>Copie a pasta descriptografada unida para o USB como BotW/.<br>Você também pode usar um .wua descriptografado ou um .zip contendo uma pasta descriptografada. Extraia .7z ou um arquivo contendo .wua no PC primeiro.<br>Mantenha o USB conectado durante a configuração.<br>4 / 4<br>Continuar |
| USB scan | Insert the USB drive with your game<br>Looking for your game… Keep the USB drive connected. | Conecte o USB com seu jogo<br>Buscando seu jogo… Mantenha o USB conectado. |
| No source | Insert the USB drive with your game<br>Couldn't find the game. Select its folder manually.<br>Select game folder / Scan again / Back | Conecte o USB com seu jogo<br>Não foi possível achar o jogo. Selecione a pasta manualmente.<br>Selecionar pasta do jogo / Buscar novamente / Voltar |
| Incomplete source | Files missing<br>Couldn't find the game. Select its folder manually. | Faltam arquivos<br>Não foi possível achar o jogo. Selecione a pasta manualmente. |
| Accepted formats | Insert the USB drive with your game<br>Couldn't find the game. Select its folder manually.<br>Decrypted folder: code/ + content/ + meta/ (+ optional aoc/). / .wua: complete, already decrypted Cemu archive. / .zip / .7z: archive containing a decrypted folder or .wua. Extract .7z or an archive containing .wua on a PC first. | Conecte o USB com seu jogo<br>Não foi possível achar o jogo. Selecione a pasta manualmente.<br>Pasta descriptografada: code/ + content/ + meta/ (+ aoc/ opcional). / .wua: arquivo completo do Cemu, já descriptografado. / .zip / .7z: arquivo contendo uma pasta descriptografada ou .wua. Extraia .7z ou um arquivo contendo .wua no PC primeiro. |
| Archive or split folder preparation | Preparing your game on the USB drive | Preparando seu jogo no USB |
| Executable verification | Checking your game | Conferindo seu jogo |
| Wrong version | Checking your game<br>This executable is not version 1.5.0 (v208).<br>Use your already extracted and decrypted update 1.5.0 (v208) and merge it over the base game.<br>Back | Conferindo seu jogo<br>Este executável não é da versão 1.5.0 (v208).<br>Use a atualização 1.5.0 (v208) que você já extraiu e descriptografou e copie por cima do jogo base.<br>Voltar |
| Storage choice | Where would you like to play?<br>Play from the USB drive / Import to internal storage / Back | Onde você quer jogar?<br>Jogar pelo USB / Importar para o armazenamento interno / Voltar |
| Internal import | Copying your game | Copiando seu jogo |
| Controller choice | Button layout<br>Which buttons do you prefer?<br>The game's prompts follow your choice. You can change it later in Settings.<br>Xbox · A is the bottom button / Nintendo · A is the right button | Posição dos botões<br>Quais botões você prefere?<br>Os avisos do jogo seguem a sua escolha. Dá pra mudar depois em Configurações.<br>Xbox · A é o botão de baixo / Nintendo · A é o botão da direita |
| Automatic start | BotW Xbox Recomp<br>Next time, open the game automatically?<br>Yes / No | BotW Xbox Recomp<br>Na próxima vez, abrir o jogo automaticamente?<br>Sim / Não |
| Launcher home | BotW Xbox Recomp<br>Play / Settings / Mods | BotW Xbox Recomp<br>Jogar / Ajustes / Mods |

Incomplete sources list their actual paths and localized issue notes, followed by the instruction
to copy the missing files into that folder and reconnect the USB drive. Storage adds the actual game
size, USB and console free/capacity figures. For an already imported game, Continue replaces the
internal import action. The wrong-version screen appends the detected title version when available.
No separate completion screen was added; setup returns to the launcher home.

## Setup notices and controls

| Situation | English | Portuguese |
| --- | --- | --- |
| `wrong_folder` | That folder doesn't contain Breath of the Wild. | Essa pasta não contém o Breath of the Wild. |
| `drive_removed` | The USB drive was removed. Reconnect it to continue. | O USB foi removido. Conecte novamente para continuar. |
| `setup_failed` | Unable to prepare the game. Check the USB, files and free space, then try again. | Não foi possível preparar o jogo. Confira o USB, os arquivos e o espaço livre e tente novamente. |
| `unsupported` | Unsupported format — extract on a PC and import from USB. | Formato não suportado — extraia no PC e importe pelo USB. |
| `usb_space_insufficient` | Free the extracted game size plus 2 GB on the USB drive, then try again. | Libere o tamanho do jogo extraído mais 2 GB no USB e tente novamente. |
| `space_unknown` | Free space unavailable. Reconnect the drive and try again. | Espaço livre indisponível. Reconecte o USB e tente novamente. |
| `space_insufficient` | Import needs the game size plus 2 GB free. | Para importar, libere o tamanho do jogo mais 2 GB. |
| `existing_import` | An internal game folder already exists. Remove the imported copy in Settings first. | Já existe uma pasta de jogo interna. Remova a cópia importada nos Ajustes primeiro. |
| `dlc_optional` | DLC not found. It is optional; you can add aoc/ later. | DLC não encontrado. É opcional; você pode adicionar aoc/ depois. |
| `version_found` | Found title version:  | Versão encontrada:  |
| `copy_missing` | Copy these files to the USB drive, into  | Copie estes arquivos para o pendrive, na pasta  |
| `copy_missing_end` | , and plug it in again | , e conecte de novo |
| `fix_missing` | Copy this item from your own dump. | Copie este item do seu próprio dump. |
| `fix_size` | wrong size — copy the complete file again | tamanho incorreto — copie o arquivo completo de novo |
| `fix_incomplete` | incomplete or unreadable — copy the complete file again | incompleto ou ilegível — copie o arquivo completo de novo |
| `fix_title` | title ID:  | ID do título:  |
| `fix_version` |  is version v |  é versão v |
| `fix_version_end` | ), needs v208 = 1.5.0: copy the update over the base game | ), precisa de v208 = 1.5.0: copie a atualização sobre o jogo base |
| `space_game` | Game:  | Jogo:  |
| `space_usb` | USB:  | USB:  |
| `space_console` | Console:  | Console:  |
| `space_free` | Free:  | Livre:  |
| `space_of` |  of  |  de  |
| `setup_hints` | A: select · B: back · D-pad / stick: move | A: selecionar · B: voltar · Direcional / analógico: mover |
| `guide_hints` | ← / →: page · A: continue · B: back | ← / →: página · A: continuar · B: voltar |
| `diagnostic_hints` | Ⓐ Scan again  Ⓑ Back  Ⓧ What can I use? · ↑/↓ Scroll | Ⓐ Buscar novamente  Ⓑ Voltar  Ⓧ O que posso usar? · ↑/↓ Rolar |
| `setup_cancel` | B: cancel · Keep the USB drive connected | B: cancelar · Mantenha o USB conectado |
| `retry` | Scan again | Buscar novamente |
| `autoboot_missing` | Automatic start is on, but the game was not found.<br>Connect the USB drive with your game, or set it up again. | O início automático está ligado, mas o jogo não foi encontrado.<br>Conecte o pendrive com o seu jogo ou configure de novo. |
| `setup` | Set up | Configurar |

## Files changed

Paths below are relative to the repository; UWP paths share `runtime/src/host/uwp/`.

- `app.cpp`: removes online artwork loading, external guide URLs and QR generation, key-related
  screen branches and errors; keeps local art and decrypted setup.
- `setup.h`, `setup_storage.h`, `diagnostics.h`: remove encrypted source discovery, key scanning,
  decryption and associated diagnostics; retain folder and archive setup, merging and stable format IDs.
- `formats/reader.h`, `formats/layout.h`: retain decrypted WUA/ZIP readers, integrity validation,
  streaming extraction and title layering; remove encrypted filesystem readers.
- `strings.h`: replaces guide text and accepted-format descriptions in both languages and removes
  obsolete enum entries together with their matching table rows.
- `polish.h`, `tests/polish_tests.cc`: remove unused remote artwork identity parsing and its fixture.
- `Package.appxmanifest`: removes encrypted image and encrypted metadata associations; retains WUA,
  ZIP, 7z and decrypted game content access. Internet permission remains for mod downloads.
- `tests/setup_tests.cc`, `tests/diagnostic_tests.cc`: remove encrypted fixtures; retain synthetic
  SHA-256, WUA, ZIP integrity/path validation, title merge, discovery, transitions and diagnostics.
  Recognition fixtures cover wrapped WUA files in ZIP and 7z, including uppercase extensions.
- `tests/check-syntax.sh`: updates its description of the remaining checks.
- `runtime.cpp`: fixes an existing Mac syntax failure by using `setenv` outside Windows;
  Windows continues using its existing environment API for the profiler flag.
- Root `README.md`, UWP `README.md`, `docs/boot-crash-fst-links.md`: describe decrypted input,
  local artwork and retained regression coverage; remove obsolete extraction instructions.
- `pyproject.toml`, `uv.lock`: remove the unused Python cryptography dependency and its transitive packages.
- This report records the resulting screens, behavior and validation.

The standalone Python disc extractor, its Python fixture file and `formats/crypto.h` were deleted.
No files under `runtime/cemu` were changed.

## Validation

- UWP host CMake/Ninja suite: **6 of 6 tests passed**.
- Platform CMake/Ninja suite: **6 of 6 tests passed**.
- `tests/check-syntax.sh`: **passed**, including its Microsoft SDK and C++/WinRT APP partition check.
  The SDK emitted the existing `getenv` deprecation warning from `runtime/src/fsr.h`.
- `tests/check-sdk-syntax.sh`: **passed** for `app.cpp`, `launcher.cpp` and `runtime.cpp` with SDK 26100.
- `git diff --check`: **passed**.
- The acceptance search outside `runtime/cemu` has **four retained defensive matches**:
  `.gitignore` lines 23–24 prevent accidental tracking of encrypted disc images;
  `.github/workflows/ci-build.py` lines 11–12 reject them as CI inputs. These are rejection rules,
  with no parsing, decryption, download or player guidance. Removing them would weaken the existing
  game-free repository and CI safeguards. There are no runtime, tool or documentation matches.
- The broader audit leaves only unrelated terms: GPU synchronization tickets, PowerPC instructions,
  memory-footprint names and Mac package-manager paths. No console key handling remains outside the
  explicitly excluded third-party subtree.
- Both temporary test build directories were removed after validation. No full UWP game build ran.

These checks validate portable logic and SDK syntax. Xbox device behavior and full package linking
remain for the orchestrator after merging.
