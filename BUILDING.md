# Build your own Xbox package

**English** · [Português brasileiro](#português-brasileiro)

You need your own Wii U and copy of Breath of the Wild for Wii U. Dump it yourself,
decrypt it, and merge update **1.5.0 (v208)**. The result must be one folder containing
`code/`, `content/` and `meta/`; encrypted images and unmerged updates are not accepted.
You also need a PC or Mac with about **15 GB free**, internet for setup, and an
**Xbox Series in Developer Mode** on the same network.

Microsoft's Developer Mode activation requires a **Microsoft Partner Center developer
account**. Its one-time fee is Microsoft's, not ours. This project takes no payment.

Download this source repository and open a terminal in it. Install
[uv](https://docs.astral.sh/uv/getting-started/installation/) first (macOS:
`brew install uv`; Windows: `winget install --exact --id astral-sh.uv`). uv supplies
Python 3.11 or newer. Run these four commands, replacing the dump path and Xbox IP:

```sh
uv run tools/builder/botw_builder.py doctor
uv run tools/builder/botw_builder.py setup --accept-microsoft-license
uv run tools/builder/botw_builder.py build --game "/path/to/your/decrypted-dump"
uv run tools/builder/botw_builder.py install --xbox 192.168.1.100
```

After `doctor`, run the install commands it prints for your OS, then repeat it until
it passes. It checks LLVM, CMake, Ninja, Git, uv, OpenSSL and a signer. On Linux it
prints apt, pacman or dnf commands. On Windows, reopen your terminal after installing
tools; SignTool from the Windows SDK can replace osslsigncode.

Read [Microsoft's CRT/SDK license](https://go.microsoft.com/fwlink/?LinkId=2086102)
before passing `--accept-microsoft-license`. Omit that flag to read the notice and
answer `yes` interactively. Setup never accepts it for you. FidelityFX is MIT licensed;
the Cemu graphic packs are CC0. Setup downloads pinned tools and verifies the dependency
archives with SHA-256.

Everything is kept under `~/.botw-builder` by default. `build` writes
`output/BotWRecomp.appx` and `output/BotWRecomp.cer` there and removes its temporary
sources and build files. Keep the APPX private: it contains your game's recompiled code.
The certificate is public; keep the private `certificates/` directory to sign future
updates with the same identity. No Apple certificate or system keychain is used.
Nothing is uploaded during setup or build. `install` sends the package and public
certificate only to the Xbox you specify.

The default preset is `remaster` for Series X. Use `--preset vanilla` to omit graphic
packs, `--version 1.0.1.0` for a newer package version, or `--out "/external/output"`
for another output folder. Pass `--data-dir "/external/data"` to each command to move
the toolchain and certificates. Data/output folders must be outside every Git repository.
For existing toolchain data, `setup` and `build` accept `--xwin-dir`, `--vcpkg-dir`,
`--ffx-source`, `--ffx-shaders` and `--graphic-packs` (the BreathOfTheWild folder).
Run a command with `--help` for the full options.

In Xbox Dev Home, enable Remote Access and use its IP. If authentication is enabled,
add `--user YOUR_USER`; the builder asks for the password without saving it. For
noninteractive use, `--password YOUR_PASSWORD` is available. `install` uses your last
successful build, or the folder passed with `--out`. It accepts the portal's self-signed
TLS certificate. After installation, open the app in Dev Home and select your own
`code/content/meta` game folder on USB; game data is not copied into the APPX.

Troubleshooting:

- **Wrong RPX hash / missing folders:** use a decrypted Wii U dump with v208 merged;
  select the parent of `code/`, `content/`, `meta/`, not an archive or the update alone.
- **Low space:** free space first. Builds stop below 6 GiB; about 15 GB is recommended.
- **Waiting for Ninja:** another build is running. Let it finish; only one Ninja runs
  at a time. `--jobs 1` reduces compiler memory use.
- **SHA-256 mismatch:** a release file changed or the download is damaged. Stop and
  check for a builder update; do not bypass verification.
- **Portal refuses credentials or redirects:** set them again in Dev Home > Remote
  Access (console updates can reset them). Keep both devices on the same network.
- **Xbox requests Microsoft.VCLibs.140.00:** install Microsoft's x64 UWP Visual C++
  runtime through Device Portal, then retry. It is a manifest dependency, not game data.
- **Signature / update rejected:** retain your original signing identity and increase
  `--version`. Do not share or replace the private PFX casually.

Nothing derived from the game is distributed. See [LEGAL.md](LEGAL.md).

## Português brasileiro

Você precisa do seu próprio Wii U e de uma cópia de Breath of the Wild para Wii U.
Faça o dump por conta própria, descriptografe e mescle a atualização **1.5.0 (v208)**.
O resultado deve ser uma pasta com `code/`, `content/` e `meta/`; imagens criptografadas
e atualizações separadas não servem. Também precisa de um PC ou Mac com cerca de
**15 GB livres**, internet no preparo e um **Xbox Series em Modo de Desenvolvedor**
na mesma rede.

A ativação do Modo de Desenvolvedor exige uma **conta de desenvolvedor do Microsoft
Partner Center**. A taxa única é da Microsoft, não nossa. Este projeto não cobra nada.

Baixe este repositório e abra um terminal nele. Instale primeiro o
[uv](https://docs.astral.sh/uv/getting-started/installation/) (macOS:
`brew install uv`; Windows: `winget install --exact --id astral-sh.uv`). Ele fornece
Python 3.11 ou mais recente. Troque o caminho do dump e o IP do Xbox:

```sh
uv run tools/builder/botw_builder.py doctor
uv run tools/builder/botw_builder.py setup --accept-microsoft-license
uv run tools/builder/botw_builder.py build --game "/caminho/do/seu/dump-descriptografado"
uv run tools/builder/botw_builder.py install --xbox 192.168.1.100
```

Execute os comandos de instalação mostrados pelo `doctor` e repita até passar.
Ele verifica LLVM, CMake, Ninja, Git, uv, OpenSSL e o assinador. No Linux, mostra os
comandos apt, pacman ou dnf. No Windows, reabra o terminal após instalar; o SignTool
do Windows SDK pode substituir o osslsigncode.

Leia a [licença da Microsoft para CRT/SDK](https://go.microsoft.com/fwlink/?LinkId=2086102)
antes de usar `--accept-microsoft-license`. Sem a opção, o preparo mostra o aviso e
pede que você responda `yes`. Ele nunca aceita por você. FidelityFX usa licença MIT;
os packs do Cemu são CC0. O preparo baixa ferramentas fixadas e verifica os arquivos
de dependências por SHA-256.

O padrão é guardar tudo em `~/.botw-builder`. O build cria `output/BotWRecomp.appx`
e `output/BotWRecomp.cer` nessa pasta e apaga os fontes temporários e intermediários.
Não compartilhe o APPX: ele contém código recompilado do seu jogo. O certificado é
público; preserve a pasta privada `certificates/` para assinar futuras atualizações
com a mesma identidade. Nenhum certificado Apple ou chaveiro do sistema é usado.
O preparo e o build não enviam nada. `install` envia o pacote e o certificado público
somente ao Xbox informado.

O preset padrão é `remaster` para Series X. Use `--preset vanilla` para omitir os packs,
`--version 1.0.1.0` para atualizar a versão ou `--out "/pasta/externa"` para mudar a
saída. Passe `--data-dir "/dados/externos"` em cada comando para mudar ferramentas e
certificados. Dados e saída devem ficar fora de qualquer repositório Git. Para
reaproveitar ferramentas existentes, `setup` e `build` aceitam `--xwin-dir`,
`--vcpkg-dir`, `--ffx-source`, `--ffx-shaders` e `--graphic-packs` (pasta
BreathOfTheWild). Use `--help` em cada comando para ver todas as opções.

No Dev Home do Xbox, ative Remote Access e use o IP mostrado. Se houver autenticação,
passe `--user SEU_USUARIO`; a senha é pedida sem ser salva. Para execução automática,
existe `--password SUA_SENHA`. `install` usa o último build concluído, ou a pasta de
`--out`, e aceita o certificado TLS próprio do Portal. Depois, abra o app no Dev Home
e selecione sua pasta `code/content/meta` no pendrive; os dados não vão dentro do APPX.

Se algo falhar:

- **Hash errado ou pastas ausentes:** use o dump descriptografado de Wii U com v208
  mesclado e selecione a pasta que contém `code/`, `content/` e `meta/`.
- **Pouco espaço:** libere espaço antes. O build para abaixo de 6 GiB; reserve cerca de 15 GB.
- **Esperando Ninja:** outro build está rodando. Espere terminar. `--jobs 1` reduz o uso de RAM.
- **SHA-256 diferente:** o arquivo publicado mudou ou o download veio danificado.
  Procure uma atualização do builder; não pule a verificação.
- **Portal recusa ou redireciona:** configure usuário e senha novamente em Dev Home >
  Remote Access; atualizações podem apagá-los. Confira se os aparelhos estão na mesma rede.
- **Falta Microsoft.VCLibs.140.00:** instale o runtime Visual C++ UWP x64 da Microsoft
  pelo Portal e tente de novo. É uma dependência do app, não dado do jogo.
- **Assinatura ou atualização recusada:** preserve a identidade de assinatura original
  e aumente `--version`. Não compartilhe nem substitua o PFX sem cuidado.

Nada derivado do jogo é distribuído. Veja [LEGAL.md](LEGAL.md#português).
