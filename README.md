# Forza Motorsport — Android (não oficial)

Port nativo e **não oficial** de **Forza Motorsport (Xbox, 2005)** para
**Android arm64**, por **recompilação estática**. O código x86 do `default.xbe`
é traduzido para C++ com o [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp)
e compilado para arm64 com o NDK. Não há emulador: o jogo roda como código
nativo.

> **Aviso legal.** Este projeto não é afiliado, patrocinado ou endossado pela
> Microsoft, Turn 10 Studios ou qualquer detentor de *Forza Motorsport*.
> **Nenhum asset, XBE, ISO ou código gerado do jogo é distribuído aqui.**
> Você precisa da sua própria cópia legítima do jogo. O C++ gerado e o APK
> contêm código derivado do jogo; mantenha o repositório privado e não
> publique releases sem decidir isso conscientemente.

## Status

**Marco 1 — esqueleto.** O repositório já tem:

- Estrutura modular (`native/{core,platform,gfx,audio,runtime,game,ui}`).
- App Android com SDL3 e permissão de armazenamento.
- `build.yml` que compila **somente no GitHub Actions**.
- Pipeline de recompilação (`xbe_symbols` → `xbe_recomp` → NDK → APK).
- Build **stub** (sem o jogo) para o CI ficar verde antes de configurar o
  secret `FORZA_XBE_URL`.
- `docs/GAPS.md` com a análise do XBE: XDK **5849**, bibliotecas usadas e
  lacunas conhecidas do runtime.

O backend gráfico do marco 1 é um **null backend** (a janela abre e o jogo
tenta iniciar; nada é desenhado ainda). O backend Vulkan real, áudio, toque,
giroscópio e o menu de opções entram nos próximos marcos. Veja
[`docs/PROGRESS.md`](docs/PROGRESS.md).

## Requisitos

- Android **10+** (API 29), **arm64-v8a** apenas.
- GPU com **Vulkan 1.1** (drivers sem *dynamic rendering* ou *timeline
  semaphores* também são suportados pelo desenho do backend).
- 3–4 GB de RAM.
- ~3 GB de armazenamento para os arquivos do jogo.

Alvos de desempenho: MediaTek **Helio G80** (Mali-G52 MC2) e Snapdragon
**480+** (Adreno 619), com 30 FPS estáveis em qualidade baixa.

## Instalação

1. Instale o APK gerado pelo workflow.
2. Abra o app e conceda **acesso a todos os arquivos** (Android 11+) ou a
   permissão de armazenamento (Android 10).
3. Copie o jogo para:

   ```
   /storage/emulated/0/Forza/
   ```

   Coloque `default.xbe` **junto com as pastas de dados** (`media/`, etc.), ou
   a imagem do disco (`.iso`) na mesma pasta. Também funciona em
   `Android/data/com.deivid.forza/files/assets`.

O app procura `default.xbe` e confere o SHA-1 contra o build. Um XBE de outra
região/versão pode ser recusado.

## Build (somente GitHub Actions)

A compilação do APK e a execução do recompilador acontecem **apenas** no
GitHub Actions. Localmente, use apenas leitura de código, escrita de código e
linters (`clang-format`, `yamllint`, `actionlint`). Não rode o recompilador
nem compile o APK na sua máquina.

### Secrets do repositório

| Secret | Obrigatório | Descrição |
|---|---|---|
| `FORZA_XBE_URL` | para build completo | URL **direta** para `default.xbe` ou para a ISO do jogo; o CI extrai o XBE da ISO. |
| `FORZA_XBE_SHA256` | opcional | SHA-256 do arquivo baixado (XBE ou ISO); se presente, o CI verifica antes de extrair. |
| `FORZA_KEYSTORE_BASE64` | opcional | `.jks` de release em base64. |
| `FORZA_KEYSTORE_PASSWORD` | opcional | Senha do keystore. |
| `FORZA_KEY_ALIAS` | opcional | Alias da chave. |
| `FORZA_KEY_PASSWORD` | opcional | Senha da chave. |

Sem `FORZA_XBE_URL`, o workflow gera um **APK stub** que abre e explica como
habilitar o build completo. Com o secret, o CI baixa o XBE, roda
`xbe_symbols` e `xbe_recomp`, compila o código gerado para arm64 e gera o APK
real.

Exemplo (com `gh` autenticado):

```bash
gh secret set FORZA_XBE_URL --repo deivid22srk/Forza-Android --body "https://..."
gh secret set FORZA_XBE_SHA256 --repo deivid22srk/Forza-Android --body "<sha256>"
gh workflow run build.yml --repo deivid22srk/Forza-Android
gh run watch --repo deivid22srk/Forza-Android
```

> O token do GitHub **nunca** deve ser gravado em arquivos do repositório. Use
> `GH_TOKEN` no ambiente ou `gh auth login`.

### Rodando o build

O workflow dispara em todo push para `main`, em pull requests e em
`workflow_dispatch`. O APK fica no artifact `forza-full-<sha>` (com o jogo) ou
`forza-stub-<sha>` (sem o jogo), com o nome
`Forza-Android-<sha7>-<full|stub>.apk`.

## Arquitetura

```
native/
  core/       # tipos, logging (logcat), config: sem dependência de Android/Vulkan
  platform/   # SDL3, lifecycle, armazenamento, entrada, térmica
  gfx/        # backend Vulkan (marco 3) e null backend (marco 1)
  audio/      # saída SDL3/AAudio
  runtime/    # runtime do XboxXBE-Recomp (kernel, XDK, D3D8) + código gerado
  game/       # patches/hooks específicos do Forza
  ui/         # overlay e menu de opções (Dear ImGui, marcos seguintes)
app/          # Activity Android, permissões, importador de assets
ci/           # scripts usados pelo build.yml
docs/         # arquitetura, GAPS, performance, progresso
```

Regras de ouro:

- **Nunca edite `generated/`.** Mudanças de comportamento entram por hooks em
  `game/` ou por overrides em `runtime/`.
- Dependências apontam em uma direção: `core` ← `platform`/`gfx`/`audio` ←
  `game`/`ui`; o runtime não conhece Android.
- C++20, RAII, sem alocação por frame no caminho crítico.
- Config de execução em `forza.toml` (gravado em `Android/data/com.deivid.forza/files`);
  os modelos de plataforma (`android.toml`) e de controles (`layout.toml`) entram
  com o backend Vulkan e a camada de toque.
- Commits no formato `tipo(escopo): descrição`.

## Créditos

- [XboxXBE-Recomp](https://github.com/TGP482/XboxXBE-Recomp) — recompilador e
  runtime portável.
- [Black for Android](https://github.com/RanggaCasper/Black-Android) —
  arquitetura de referência do host Android.
- SDL3, toml++, Zydis, glslang e Dear ImGui.
- xemu e Cxbx-Reloaded, referências de comportamento do hardware/XDK.

Licença: MIT (veja [`LICENSE`](LICENSE) e [`THIRD_PARTY.md`](THIRD_PARTY.md)).
