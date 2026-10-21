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

## Tarefas derivadas

- [ ] Marco 2: primeiro boot no CI com o XBE real; coletar log e preencher
      `docs/PROGRESS.md`.
- [ ] Marco 3: backend Vulkan mínimo (clear/present) + ImGui overlay.
- [ ] Marco 3: D3D8 draws → Vulkan; cache de pipeline/shaders.
- [ ] Marco 4: áudio SDL3 validado; XACT/WMADEC.
- [ ] Marco 5: input (gamepad, toque, giroscópio).
- [ ] Marco 6: performance nos alvos G80/480+, térmica, resolução dinâmica.
- [ ] Marco 7: menu de opções, instalador de assets, saves, estabilidade.
