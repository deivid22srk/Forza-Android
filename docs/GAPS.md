# GAPS — o que o runtime ainda não cobre para Forza Motorsport

Este documento é vivo. Ele lista o que o XBE do Forza usa, o que o runtime do
XboxXBE-Recomp já cobre e o que precisa ser implementado na nossa camada
(`native/`) sem editar `generated/`.

## Imagem analisada

| Campo | Valor |
|---|---|
| Título | Forza Motorsport |
| Title ID | `0x4D53006E` |
| Regiões | NA / Japan / Rest of World |
| Tipo | Retail (assinado) |
| XDK | **5849** (`sdk/xdk/5849` existe) |
| Entry point | `0x00268EB5` (campo do header XOR `0xA8FC57AB`) |
| Kernel thunk | `0x00485D80` (XOR `0x5B6D40B6`) |
| Base | `0x00010000` |
| Imagem | `0x61FEA0` bytes virtuais, 39 seções |

### Bibliotecas XDK linkadas

`XAPILIB 1.0.5849`, `D3D8 1.0.5849`, `D3DX8 1.0.5849`, `XGRAPHC 1.0.5849`,
`DSOUND 1.0.5849`, `XBOXKRNL 1.0.5849`, `XACTENG 1.0.5849`,
`XVOICE 1.0.5849`, `VOICMAIL 1.0.5849`, `XONLINES 1.0.5849`,
`LIBCPMT 1.0.5849`, `LIBCMT 1.0.5849`.

### Seções notáveis

`.text`, `FZGINIT`, `FZFULLUI` (código), `BINK`, `BINK32*`, `BINK16*`,
`BINK4444`, `BINK5551`, `BINKYUY2` (vídeo), `D3D`, `D3DX`, `XGRPH`
(gráficos), `DSOUND`, `WMADEC`, `XACTENG`, `XONLINE`, `XNET`, `XPP`,
`.rdata`, `.data`, `DDERR`, `DOLBY`, `XON_RD`, `BINKDATA`, `$$XTIMAGE`,
`BKGDINST`, `HASHES`, `.XTLID`.

## Imports do kernel (XBOXKRNL)

O thunk table (`0x00485D80`) tem **159 ordinais**. Comparando com os
`KERNEL_EXPORT`/`KERNEL_DATA` registrados pelo runtime do SDK, faltavam
exatamente três, agora cobertos em `native/runtime/overrides.cpp`:

| Ordinal | Export | Por que o jogo usa | Implementação |
|---|---|---|---|
| 76 | `IoQueryVolumeInformation` | XAPI converte o handle em `FILE_OBJECT` via `ObReferenceObjectByHandle` e consulta o volume; no boot pede classe 5 (`FileFsAttributeInformation`). O helper de media ID do XAPILIB (0x267489) ramifica no nome: **GDFX** lê o media ID de `FsContext[0]`; FATX lê o serial de `FsContext[0x1C]`. | Espelha as classes 1/3/5/7 de `NtQueryVolumeInformationFile` (ord 218) e responde **"GDFX"** na classe 5 (o disco do jogo é XDFS no console); classes desconhecidas devolvem `kInvalidParameter` e logam em vez de abortar. |
| 232 | `NtUserIoApcDispatcher` | Rotina de conclusão de IO sobreposto; o host completa IO de forma síncrona, então nunca tem trabalho real. | Stub que loga e retorna 0. |
| 343 | `XcPKGetKeyLen` | Tamanho da chave RSA pública; usado nas checagens de assinatura offline, que o SDK já responde com sucesso. | Devolve 0x800 (2048 bits) e loga. |
| 246 | `ObReferenceObjectByHandle` | O SDK registra o corpo do objeto com o *stamp* do dispatcher em +8/+12 — exatamente onde ficam `FsContext`/`FsContext2` do `FILE_OBJECT`. O crash de boot seguinte (SIGSEGV em `002674F4`, VA 0xE) foi o XAPI lendo `FsContext` = serial 14. | Override: objetos de arquivo ganham um `FILE_OBJECT` real (Type=5/IO_TYPE_FILE, Size=0x48) com `FsContext` apontando para um descritor GDFX falso (`[0]` = media ID `0x4D53006E`, `[0x1C]` = serial); objetos dispatcher mantêm o comportamento do SDK (`AllocSystem(32)` + `Bind`). |

`IoQueryFileInformation` (ord 75), embora vizinho, **não** é importado pelo
Forza, então não foi implementado.

### Media ID do disco

`kMediaId = 0x4D53006E` (o title ID) é um valor estável no lugar do media ID
do disco de varejo. O jogo usa o ID para detectar troca de disco entre
execuções, então o que importa é ser estável e não-nulo.

## Cobertura por biblioteca

| Biblioteca | Runtime do SDK | Status | Observações |
|---|---|---|---|
| `XBOXKRNL` | `runtime/kernel/*` | ✅ base | threads, waits, arquivos, memória, objetos, crypto, EEPROM; precisa validar no Android (pthread, mmap, storage). |
| `XAPILIB` | `runtime/xdk/xapi.cpp` | ✅ base | input, títulos, storage, XGetVideoMode; validar pads/touch. |
| `D3D8` | `runtime/xdk/d3d8.cpp` + `runtime/gpu/*` | ⚠️ parcial | HLE genérico de XDK 5849; falta o backend Vulkan e validar estados/shims do Forza. |
| `D3DX8` | recompilado do XBE | ⚠️ a validar | funções matemáticas/helpers linkadas no XBE; `crt_math.cpp` cobre a CRT. |
| `XGRAPHC` | parcial em `xdk/d3d8.cpp`/`gpu` | ⚠️ a validar | `XGSetSurfaceHeader`, `XGSwizzleRect`, `XGUnswizzleRect`, `XGCompressRect` etc. |
| `DSOUND` | `runtime/xdk/dsound.cpp` | ✅ base | mixer software + ADPCM/streams; trocar `audio::StartOutput` por SDL3/AAudio. |
| `XACTENG` | recompilado + DSOUND | ⚠️ a validar | engine XACT do jogo; depende de DSOUND e WMADEC. |
| `WMADEC` | recompilado | ⚠️ a validar | decode WMA; pode usar matemática da CRT. |
| `DOLBY` / `DDERR` | recompilado | ⚠️ a validar | Dolby Digital; provavelmente só em output 5.1. |
| `BINK` | `runtime/xdk/bink.cpp` | ✅ base | vídeos de intro/menus; validar acima de 30 FPS. |
| `XONLINES` / `XNET` | `runtime/xdk/xnet.cpp` (Windows) | ❌ lacuna | System Link/Live depende de Winsock; excluído no marco 1. Implementar sockets POSIX/Android ou desabilitar online. |
| `XVOICE` / `VOICMAIL` | — | ❌ lacuna | voz no Live; irrelevante para single-player. |
| `LIBCPMT` / `LIBCMT` | `runtime/xdk/crt_math.cpp` + NDK | ⚠️ parcial | math, malloc, strings; validar funções que o código gerado chama. |

## Lacunas gráficas (D3D8 → Vulkan)

O runtime atual desenha com **Direct3D 12**. No Android precisamos de um
backend **Vulkan 1.1** com fallback para drivers fracos. O `runtime/xdk/d3d8.cpp`
já é portável e chama `gpu::Backend`; o que falta:

1. **Backend Vulkan** (`native/gfx/vulkan/`):
   - `VkInstance`/`VkDevice` sem dynamic rendering e sem timeline semaphores;
   - render passes explícitos, `loadOp`/`storeOp` corretos (tile-based Mali/Adreno);
   - swapchain + recriação em `surfaceLost`/`surfaceChanged`;
   - pipeline cache em disco e cache de shaders traduzidos;
   - buffers dinâmicos em anel, sub-alocação por blocos;
   - `Clear`, `Draw`, `Copy`, `Present`, `Sync`, `SyncLate`, fences.
2. **Tradução de shaders**: o SDK gera HLSL; usar glslang → SPIR-V
   (`native/gfx/shader.cpp`), com cache em disco.
3. **Texturas**: formatos Xbox (RGBA8, DXT1/3/5, paletas P8, YUY2) →
   Vulkan; transcodificar para ASTC/ETC2 quando disponível, com mipmaps.
4. **Estados**: cache de pipeline por estado D3D8 (render states, TSS,
   blend, depth/stencil, vertex decl, shaders).
5. **Superfície/backbuffer**: `D3DDevice_Swap`, gamma ramp, `PersistDisplay`,
   readbacks de visibilidade.
6. **XG helpers**: `XGSwizzleRect`/`XGUnswizzleRect`/`XGSetSurfaceHeader`
   (se o XBE recompilado não os cobrir) e formatos de superfície.

## Lacunas de áudio

- `runtime/audio/xaudio2.cpp` é Windows/XAudio2; substituído por
  `native/audio/output.cpp` (SDL3/AAudio, float estéreo 48 kHz).
- Validar latência/underruns e o mix 5.1 → estéreo do `dsound.cpp`.
- XACT/WMADEC: validar se o código recompilado chama só DSOUND ou se precisa
  de HLE adicional.
- Pausar/retomar no background e no foco perdido.

## Diagnóstico no dispositivo

- **Logging**: o `logx` do SDK descartava linhas debug/info/warn quando
  `NDEBUG` — e todo build Android é release (o gradle força
  `CMAKE_BUILD_TYPE=Release` no NDK para todos os buildTypes). Isso deixava o
  boot inteiro invisível no logcat e no arquivo (o APK `e8d1f90` parecia
  congelar após o SHA-1; na verdade o boot avançava em silêncio). O substituto
  em `native/core/log.cpp` (mesma API, o original sai do build por regex em
  `native/runtime/CMakeLists.txt`) manda **info e acima** para o stderr — que
  `native/core/logcat.cpp` canaliza para o logcat com a tag `FORZA` — e **warn
  e acima** para o arquivo; Report segue sem prefixo. Custo por linha é
  irrelevante no volume atual (~1–5 linhas/s após o boot); vigiar quando o
  render thread real entrar (marco 3/6).
- **Dados do jogo**: o `WarnMissingData` (`game_files.cpp`) avisa em error
  quando o boot acontece sem ISO montada e sem a pasta `media` ao lado do XBE
  (layout do disco); `missing N:\…` no boot é **normal** — N: é o cache de
  instalação do console e o jogo cai para `D:\Media\cabs\*.ca2`.
- **Telemetria de boot longo** (`monitor.cpp` + batimento em `app::Pace`):
  `frames: N presented, X fps` a cada ~10 s (o loop de apresentação vivo ou
  parado) e `hdd: N files, X MiB (+Y MiB …)`/`hdd: quiet for …` a cada 10/300 s
  (o instalador BKGDINST trabalhando). Em release essas linhas são **info**:
  aparecem só no logcat (`adb logcat -s FORZA`), não no arquivo de log, que
  fica com warn+. O walk do hdd usa laço manual com `error_code` — o
  `operator++` do range-for lançaria exceção numa pasta que o instalador
  remove no meio da varredura, e a exceção numa thread detached mata o
  processo.

## Lacunas de plataforma Android

- **Storage**: `/storage/emulated/0/Forza` (all-files access) e
  `Android/data/com.deivid.forza/files/assets`; ISO via `kernel::Disc`.
- **Lifecycle**: `SDL_EVENT_WILL_ENTER_BACKGROUND` /
  `DID_ENTER_FOREGROUND`, perda/recriação de surface, rotação.
- **Input**: SDL3 gamepad (rumble), toque (volante/botões), giroscópio,
  teclado/mouse opcionais.
- **Térmica**: `android.os.PowerManager`/thermal API; reduzir resolução/FPS
  ao esquentar. (marco 6)
- **Memória**: `onTrimMemory`, orçamento para 3 GB; evitar readbacks.
- **Threads**: afinidade (lógica nos núcleos grandes, apoio nos pequenos).
- **Armazenamento de saves**: `hdd/` em `Android/data/.../files`; EEPROM.

## Riscos e incógnitas do Forza

- O Forza usa **D3DX8** e **XGRAPHC** de forma intensa (pistas, carros,
  partículas). O HLE de D3D8 pode precisar de shims específicos.
- **XACT** pode ter comportamento de threading/streaming que o DSOUND HLE
  não cobre.
- **XONLINE/XNET** pode ser inicializado mesmo em single-player; se o jogo
  travar esperando rede, será preciso stubar as chamadas.
- O jogo pode usar **XPP** (profiler) e **HASHES** (integridade) de formas
  que o recompilador precisa ignorar ou stubar.
- 39 seções e ~3 MB de código em `.text` + `FZGINIT` + `FZFULLUI`: o
  recompilador pode precisar de `[[function]]`/`[[switch]]`/`data_ranges`
  extras. Validar no primeiro run completo.
- **Entry point** e **kernel thunk** são XORados; o `xbe.h` do SDK já trata.

## Diagnóstico no dispositivo (marco 2 fechado, campo HtUPBQPG)

- O boot do jogo está **vivo e completo** no dispositivo: splash sequence
  inteira, instalador BKGDINST gravou 44,8 MiB no `hdd`, loop do front-end a
  30 fps estáveis por 40+ s sem trap. A "tela preta" do 3-a era **fronteira de
  escopo** (só o clear é desenhado), não defeito.
- O contador de FPS do 3-a não apareceu: a ressalva R6 (ImGui via backend SDL3
  fora da main thread) estava correta. No 3-b o backend SDL3 saiu e a Record
  alimenta ImGui com DisplaySize/DeltaTime manuais no thread de render.
- O 3-b desenha o caminho fixed-function completo (quads texturizados dos
  menus) espelhando o d3d12 do SDK; falhas de compilação HLSL no shaderc do
  NDK logam o erro inteiro e pulam o draw — o log de campo nomeia o conserto.
- **Aprendizado de toolchain (3-b fechado em CI, `5981d92`)**: o NDK ≥ r27
  não embute mais o shaderc (confirmado nos zips oficiais r27b/r27c/r28.2).
  O CI agora o constrói do source: revisão v2025.5 pinada
  (`ci/shaderc-revision.txt`), deps glslang/SPIRV pinadas pelo `DEPS` da
  própria revisão, target `shaderc_combined` (o `shaderc` simples é um .a
  fino sem as deps transitivas) instalado em layout compatível com o NDK
  antigo e consumido via `FORZA_SHADERC_DIR`. Os headers da v2025.5 definem
  os handles como ponteiros (`typedef struct shaderc_compiler*
  shaderc_compiler_t;`) — o `vulkan.cpp` foi alinhado. Cache do dist
  invalidado por hash da revisão + script + NDK + ABI.

## Tarefas derivadas

- [x] Marco 2: boot no dispositivo com o XBE real — fechado no campo do
      `9d9d8b2` (HtUPBQPG).
- [x] Marco 3-a: backend Vulkan mínimo (clear/present) + ImGui overlay.
- [x] Marco 3-b: D3D8 fixed-function draws → Vulkan (shaderc HLSL→SPIR-V,
      texturas BGRA8/DXT/P8, pipelines por estado, readback para guest).
- [ ] Marco 3-c: depth/stencil, points, cube/3D/depth textures, occlusion
      reports (visibilidade), anisotropy — abre as cenas 3D (corrida).
- [ ] Subir a resolução dos targets além do guest (`render_percent`) e
      mapear `spreadtap`/`kTexWide` quando isso chegar.
- [ ] Marco 4: áudio SDL3 validado; XACT/WMADEC.
- [ ] Marco 5: input (gamepad, toque, giroscópio).
- [ ] Marco 6: performance nos alvos G80/480+, térmica, resolução dinâmica.
- [ ] Marco 7: menu de opções, instalador de assets, saves, estabilidade.
