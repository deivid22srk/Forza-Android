# Progress

## Marcos

| # | Marco | Status | Notas |
|---|---|---|---|
| 1 | Esqueleto: repo, CMake modular, app Android, `build.yml`, recompilação no CI, APK que abre e mostra log | 🟢 concluído | Pipeline completo validado no CI e instalado em dispositivo: app acha o jogo, confere SHA-1 e inicia o boot. |
| 2 | Boot: `GAPS.md`, kernel/XDK suficientes para passar das intros | 🔄 em andamento | Imports de kernel todos resolvidos (0 missing no boot real); o guest cria o device D3D8, roda a sequência de splash e o instalador BKGDINST descompacta os cabs. Telemetria de boot longo no lugar (`4d144e2`): batimento de frames + observador do hdd. Falta o teste longo em dispositivo até o estado de menu. |
| 3 | Vídeo: D3D8 → Vulkan renderizando menus | 🔄 em andamento | **3-a concluído** (`9d9d8b2`): backend Vulkan com present real (decode do SDK + gamma + blit letterbox), overlay ImGui com FPS real (`fps_counter = true` por padrão) e fallback null. Draws/shaders/texturas = 3-b. |
| 4 | Corrida: pista/carro, física, áudio | ⬜ | |
| 5 | Entrada: toque, giroscópio, gamepad | ⬜ | |
| 6 | Desempenho: G80/480+, cache, resolução dinâmica, térmica | ⬜ | |
| 7 | Acabamento: menu, instalador, saves, documentação | ⬜ | |

## O que funciona

- Estrutura de diretórios em camadas (`core`, `platform`, `gfx`, `audio`,
  `runtime`, `game`, `ui`).
- `build.yml` com lint + build no GitHub Actions.
- Pipeline completo: download do XBE privado → `xbe_symbols` → `xbe_recomp` → NDK → APK arm64.
- Build **full** validado no CI (runs `39617521020` e `39618808504`): APK de 23 MB com
  `lib/arm64-v8a/libmain.so` de ~20 MB.
- Primeiro boot em dispositivo (Motorola Edge 30 Fusion, Snapdragon, Android 14):
  o app encontra `default.xbe` em `/storage/emulated/0/Forza/`, valida o SHA-1 e
  inicia as threads do guest.
- `native/runtime/overrides.cpp`: os três imports de kernel que o runtime do SDK
  não tinha (`IoQueryVolumeInformation` 76, `NtUserIoApcDispatcher` 232,
  `XcPKGetKeyLen` 343) + override do ord 246 (`ObReferenceObjectByHandle` com
  `FILE_OBJECT` real e FsContext GDFX).
- **Log de boot visível em release** (`4fb5a70`): o `log.cpp` do SDK descartava
  linhas info/warn quando `NDEBUG` — e todo build Android é release (gradle
  força Release no NDK). O substituto em `native/core/log.cpp` manda info e
  acima para o logcat e warn e acima para o arquivo; o boot passou a ser
  diagnosticável de ponta a ponta.
- Boot real no dispositivo avança (APK `4fb5a70`, dados extraídos ao lado do
  XBE em assets): `kernel imports resolved, 0 missing`, threads do guest
  iniciam no entry `00268EB5`, `d3d: CreateDevice 640x480`, telas de splash
  carregam de `D:\MEDIA\FrontEnd\...` e o instalador (`BKGDINST`) descompacta
  `ui_ntsc/mega/megaxpr/cars` de `D:\Media\cabs\*.ca2` para `Z:`.
- Aviso honesto de boot sem dados (`WarnMissingData`): se o jogo for achado
  sem ISO montada e sem a pasta `media` ao lado do XBE, o boot avisa em error
  em vez de travar em silêncio.
- **Telemetria de boot longo** (`4d144e2`): `app::Pace` loga `frames: N presented,
  X fps` a cada ~10 s (o render loop vivo ou parado) e `monitor.cpp` mede a
  árvore do `hdd/` a cada 10 s (`hdd: N files, X MiB (+Y MiB in Z s)` ou
  `hdd: quiet for …`), tornando o instalador BKGDINST visível e separando
  "instalando" de "travado". Linhas info: só no logcat em release (o arquivo
  fica com warn+).
- **Backend Vulkan com present real** (`9d9d8b2`, marco 3-a): surface/swapchain por SDL, decode da superfície frontal com o `gpu/texture.cpp` do SDK, gamma por LUT na CPU, blit letterbox 4:3 e overlay **ImGui** com o FPS real (sem input, canto superior esquerdo; `fps_counter` no `forza.toml`, ligado por padrão). Clear/Copy escrevem a memória do guest pela CPU; recriação de surface/swapchain por epoch ao voltar do background; par staging/frame por frame em voo; fallback null se o Vulkan não subir. Os draws ainda não renderizam (3-b: HLSL→SPIR-V, texturas, pipelines) — `gpu: draw … arrives with milestone 3-b` no log prova que o jogo submeteu.
- Modo stub: APK instalável sem o jogo, com diálogo explicando o secret.
- Host Android: SDL3, storage, gamepad, áudio SDL3, null backend.
- Runtime do SDK compilado para arm64 (kernel, D3D8, DSOUND, Bink, XAPI).
- `docs/GAPS.md` com análise real do XBE (XDK 5849, bibliotecas, seções).

## O que falta / quebrado

- ~~Crash 1: `IoQueryVolumeInformation` (76) não implementado~~ → corrigido no
  `59704e9`; o boot passou da query de volume.
- ~~Crash 2 (APK `59704e9`): SIGSEGV do guest em `002674F4`, VA 0xE~~ →
  XAPI leu `FILE_OBJECT->FsContext` e encontrou o serial do *stamp* do SDK
  (14); corrigido no override do ord 246 + resposta "GDFX".
- ~~Log mudo em release (APK `e8d1f90`): silêncio após o sha1, sem crash~~ →
  causa raiz: `logx` do SDK descartava info/warn com `NDEBUG`; corrigido no
  `4fb5a70`; o mesmo jogo que antes "congelava" agora mostra boot completo.
- Fechar o Marco 2: observar um **boot longo** (10–15 min, sem fechar o app)
  até o instalador terminar e o jogo chegar ao primeiro menu; sem crash/trap.
  O segundo boot deve achar `Z:\InstallMarker*.dat` e pular a instalação.
- Os avisos `missing N:\…` são **normais**: N: é o cache de instalação do
  console; o jogo cai para `D:\Media\cabs\*.ca2` (comportamento de boot por
  disco).
- Backend Vulkan (marco 3): **present real desde o `9d9d8b2`**; faltam os draws do 3-b (HLSL→SPIR-V, texturas DXT/P8/YUY2, pipelines por estado) para os menus aparecerem. **Tela mostra as cores de clear do jogo + o FPS** até lá.
- Toque, giroscópio, menu de opções, instalador de assets.
- XACT/XOnline/XNet/WMADEC não validados.

## Últimos runs

| Data | Run | Resultado | Notas |
|---|---|---|---|
| 2026-10-21 | [`39617521020`](https://github.com/deivid22srk/Forza-Android/actions/runs/39617521020) | ✅ sucesso (7m1s) | Build full; artifact `forza-full-0ddc3f4…` com APK de 23 MB. |
| 2026-10-21 | [`39618808504`](https://github.com/deivid22srk/Forza-Android/actions/runs/39618808504) | ✅ sucesso (5m12s) | Build full do `c54bd9c`; APK instalado no dispositivo. |
| 2026-10-21 | teste em dispositivo (APK do `0ddc3f4`) | ❌ crash no boot | `kernel export 76 (IoQueryVolumeInformation) is not implemented` → `guest trap` → SIGABRT; log: <https://pastebin.com/raw/2BFQhsGf>. |
| 2026-10-21 | [`39652879274`](https://github.com/deivid22srk/Forza-Android/actions/runs/39652879274) | ✅ sucesso (5m4s) | Build full do `59704e9` com os overrides de kernel; artifact `forza-full-59704e95…`. |
| 2026-10-21 | teste em dispositivo (`59704e9`, pasta do jogo vazia) | ⚠️ sem crash, sem jogo | `no game files in /storage/emulated/0/Forza … waiting for them` — o `FindGame` esperando os arquivos; log: <https://pastebin.com/raw/sg0EnNjn>. |
| 2026-10-21 | teste em dispositivo (APK do `59704e9`, só XBE na pasta assets) | ❌ SIGSEGV no boot | Passou da query de volume; crash em `002674F4` (VA 0xE): XAPI leu `FsContext` = serial do stamp. Log: <https://pastebin.com/raw/UBrktR6Y>. |
| 2026-10-21 | [`39661479458`](https://github.com/deivid22srk/Forza-Android/actions/runs/39661479458) | ✅ sucesso (5m3s) | Build full do `e8d1f90` (FILE_OBJECT real + GDFX); artifact `forza-full-e8d1f90…`. |
| 2026-10-21 | teste em dispositivo (APK do `e8d1f90`, só XBE nos assets) | ⚠️ silêncio sem crash | O log FORZA para 0,88 s após o sha1, sem trap/tombstone — causa raiz era o filtro de log em release, não um hang. Log: <https://pastebin.com/raw/bkExFGeV>. |
| 2026-10-21 | [`39669748652`](https://github.com/deivid22srk/Forza-Android/actions/runs/39669748652) | ✅ sucesso | Build full do `4fb5a70` (log do boot visível em release + `WarnMissingData`); artifact `forza-full-4fb5a70…`. |
| 2026-10-21 | teste em dispositivo (APK do `4fb5a70`, dados extraídos nos assets) | ✅ boot vivo e progredindo | `kernel imports resolved, 0 missing`; threads do guest; `d3d: CreateDevice 640x480`; splash screens de `D:\MEDIA`; instalador `BKGDINST` descompactando cabs para `Z:`; usuário fechou aos ~10 s. Log: <https://pastebin.com/raw/SErnmrpw>. Crítico: APROVADO (ressalvas resolvidas no `088e96f`). |
| 2026-10-21 | teste em dispositivo (APK do `4733154`, dados em `/storage/emulated/0/Forza`) | ✅ boot vivo; silêncio de 9 s a classificar | Splash em sequência correta (Disclaimer → Junkiexl → Team4za → Frame2, ~2-3 s cada); instalador extraindo `ui_ntsc/megaxpr` para `Z:`; probes de `N:\…` com fallback de disco normais; usuário fechou aos ~17 s sem crash. Log: <https://pastebin.com/raw/m55cxkMy>. |
| 2026-10-21 | [`39680166521`](https://github.com/deivid22srk/Forza-Android/actions/runs/39680166521) | ✅ sucesso | Build do `4d144e2` (telemetria de boot longo: batimento de frames + observador do hdd). Crítico: APROVADO (ressalvas A1/A2 de robustez do walk corrigidas antes do push). |
| 2026-10-21 | [`39686330375`](https://github.com/deivid22srk/Forza-Android/actions/runs/39686330375) | ✅ sucesso | Build do `9d9d8b2` (marco 3-a: Vulkan + FPS). Crítico: APROVADO; ressalvas R1-R5/R7 de sincronização corrigidas antes do push. |

## Próximo passo

1. **Teste de campo do APK do `9d9d8b2`** (Vulkan + FPS): abrir o app com os
   dados em `/storage/emulated/0/Forza` e conferir, em `adb logcat -s FORZA`:
   (a) `gpu: Vulkan ready on …` (setup passou no aparelho); (b) contador de FPS
   no canto superior esquerdo (~30 com o cap); (c) alternar background/foreground
   ~5× e ver a imagem voltar (rebuild de surface); (d) boot longo 10–15 min até
   o menu, com `frames:`/`hdd:` e, no menu, até 8 linhas `gpu: draw … 3-b`
   (fecha o **Marco 2**).
2. A tela mostra **as cores de clear do jogo + o FPS** — os menus desenhados
   vêm no **3-b** (HLSL→SPIR-V, texturas dos formatos do Xbox, pipelines por
   estado). Sem tombstone em `ImGui_ImplSDL3_NewFrame` é o outro ponto a vigiar
   (consultas SDL3 fora da main thread — R6 do Crítico, aceito nesta etapa).
3. Ao fechar o Marco 2, seguir para o **Marco 3-b**: shaders/texturas/draws —
   menus de verdade.
