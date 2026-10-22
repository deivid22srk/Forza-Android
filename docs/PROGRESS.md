# Progress

## Marcos

| # | Marco | Status | Notas |
|---|---|---|---|
| 1 | Esqueleto: repo, CMake modular, app Android, `build.yml`, recompilação no CI, APK que abre e mostra log | 🟢 concluído | Pipeline completo validado no CI e instalado em dispositivo: app acha o jogo, confere SHA-1 e inicia o boot. |
| 2 | Boot: `GAPS.md`, kernel/XDK suficientes para passar das intros | 🔄 em andamento | Imports de kernel todos resolvidos (0 missing no boot real); o guest cria o device D3D8, roda a sequência de splash e o instalador BKGDINST descompacta os cabs. Telemetria de boot longo no lugar (`4d144e2`): batimento de frames + observador do hdd. Falta o teste longo em dispositivo até o estado de menu. |
| 3 | Vídeo: D3D8 → Vulkan renderizando menus | 🔄 em andamento | **3-a concluído** (`9d9d8b2`): backend Vulkan com present real (decode do SDK + gamma + blit letterbox), overlay ImGui com FPS real (`fps_counter = true` por padrão) e fallback null. **3-b concluído** (`5981d92`): draws fixed-function (quads texturizados dos menus) com HLSL→SPIR-V em processo, texturas BGRA8/DXT/P8 e pipelines por estado. **Rodada de campo 2** (`fb1f2a7`): telemetria do caminho de desenho (LOG_FIRST em cada porta + batimento de 10 s) e contador com fonte própria a 40 px. **Rodada 3** (`298485b`): a telemetria pinou a causa raiz da tela preta — `ok_` do backend nunca era armado (nascia `false`, 19 atribuições `false`, nenhuma `true` em todo o histórico) e todo o backend vivia como o nulo; fix arma `ok_ = true` no fim do `Init` (Crítico APROVADO, CI verde). **Rodada 4** (`af47f36`): campo mostrou contador de FPS visível porém de lado, e `draws 7295 recorded 0` com 7434 falhas de compilação; reprodutor local (geradores do SDK + glslangValidator) pinou o `[[vk::binding]]` entre tipo e nome no HLSL — todo pixel shader morria; atributo agora lidera a declaração, swapchain usa `preTransform = IDENTITY` (rotacão volta ao driver) e falha de shader é cacheada. **Rodada 5** (`6f94e5c`): o campo do af47f36 apresenta frames com conteúdo preto (`draws 1487 recorded 1487`, 30 fps) e, ao sair do app, `VK_ERROR_SURFACE_LOST` desarma `ok_` para sempre (96 presents estacionados); agora sondas de conteúdo medem a média e a fatia não-preta em cada elo (readback → cópia do jogo → frame decodificado → gamma) mais o descabeço do stream 0 dos 2 primeiros draws, e um epoch novo da surface re-arma um backend já booted (Crítico: REPROVADO na 1ª rodada — re-arm sem gate `booted_` crasharia com Init falho — APROVADO após o gate, o clamp de pitch na sonda e o `<atomic>` explícito). Depth/stencil/points/etc. = 3-c. |
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
- **Marco 2 fechado em campo** (log HtUPBQPG do APK `9d9d8b2`): boot completo,
  instalador rodou, loop do front-end estável a 30 fps por 40+ s sem trap.
- Backend Vulkan (marco 3): **draws fixed-function desde o `ec8e7b1`, fechado em CI no `5981d92` (3-b)** — HLSL gerado pelo SDK compilado em processo com o shaderc (o NDK ≥ r27 parou de embutir: o CI agora o constrói do source com revisão pinada, `ci/build-shaderc.sh` + `ci/shaderc-revision.txt`), texturas
  via decoder do SDK, targets host com readback para a memória do guest antes
  do Present (o caminho de present do 3-a continua intacto). Faltam para o
  3-c: depth/stencil, points, cube/3D/depth textures, occlusion reports.
- Contador de FPS: sem o backend SDL3 do ImGui (R6 confirmada em campo) —
  DisplaySize/DeltaTime manuais no thread de render.
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
| 2026-10-21 | teste em dispositivo (APK do `9d9d8b2`) | ✅ marco 2 fechado em campo | **Vulkan ready** (Adreno 660, swapchain 2400x1080); splash sequence completa; instalador gravou 44,8 MiB; `frames: 1291 presented, 30.0 fps` estável até ~51 s sem trap — o jogo passa do boot e roda o loop do front-end. Tela preta = fronteira do 3-a (só clear é desenhado). Contador FPS invisível → R6 confirmada. Log: <https://pastebin.com/raw/HtUPBQPG>. |
| 2026-10-22 | [`39701339167`](https://github.com/deivid22srk/Forza-Android/actions/runs/39701339167) | ❌ falha no Build APK | `ec8e7b1`+docs (`0ace38a`, marco 3-b): o NDK ≥ r27 não embute mais o shaderc — o FATAL_ERROR projetado no CMake do gfx matou o configure. Correção: `6ac069a` compila o shaderc do source (v2025.5 pinada) no CI e o instala via `FORZA_SHADERC_DIR`. Crítico 1ª rodada: REPROVADO (A1: `shaderc_combined` em vez do target fino; A2: headers em `libshaderc/include/`) → corrigido. |
| 2026-10-22 | [`39708959398`](https://github.com/deivid22srk/Forza-Android/actions/runs/39708959398) | ❌ falha no Build APK | `6ac069a`: **Build shaderc passou** (pipeline cache/dist funcionou); falha nos typedefs — a v2025.5 define os handles como ponteiros (`typedef struct shaderc_compiler* shaderc_compiler_t;`) e o `vulkan.cpp` usava o estilo do header antigo do NDK. Correção: `5981d92` (3 declarações sem `*`). Crítico 3ª rodada: APROVADO. |
| 2026-10-22 | [`39710808032`](https://github.com/deivid22srk/Forza-Android/actions/runs/39710808032) | ✅ sucesso | Build do `5981d92` — **marco 3-b fechado em CI**: clang-format + Android APK verdes; artifact `forza-full-5981d9231b1f4a407f9a08c5ef0968c37d8d8763` (12 MB). |
| 2026-10-22 | teste em dispositivo (APK do `5981d92`) | ❌ tela preta, sem FPS | Boot completo, 30 fps estáveis, `fps_counter = true`, mas zero linhas do caminho de desenho e overlay invisível. Log: <https://pastebin.com/raw/CWmkrhwC>. |
| 2026-10-22 | [`39758253034`](https://github.com/deivid22srk/Forza-Android/actions/runs/39758253034) | ✅ sucesso | Build do `fb1f2a7` (telemetria do caminho de desenho + contador a 40 px). Crítico: APROVADO. |
| 2026-10-22 | teste em dispositivo (APK do `fb1f2a7`) | 🔍 causa raiz pinada | Overlay agora inicializa (`ImGui overlay ready`); batimento revelou `presents 88 parked 88, draws 0` com `present parked: ok 0` — o `ok_` do backend nunca era armado. Log: <https://pastebin.com/raw/gSKYNf00>. |
| 2026-10-22 | [`39762133780`](https://github.com/deivid22srk/Forza-Android/actions/runs/39762133780) | ✅ sucesso | Build do `298485b` — **fix: `ok_ = true` no fim do `Init`** (o backend rodava como o nulo desde o 3-a). Crítico: APROVADO (sem bloqueantes). |
| 2026-10-22 | teste em dispositivo (APK do `298485b`) | 🟡 metade do caminho | **Contador de FPS visível pela 1ª vez** (porém rotacionado 90°); batimento: `presents 7297 shown 7297 parked 0` mas `draws 7295 recorded 0` — 7434× `shader compile failed: forza:63: 'declaration' : Expected`; tela = clear da CPU + overlay. Log: release `Log/44444422_10-07-29-42_203.log`. |
| 2026-10-22 | [`39768394880`](https://github.com/deivid22srk/Forza-Android/actions/runs/39768394880) | ✅ sucesso | Build do `af47f36` — **fix da rodada 4**: `[[vk::binding]]` antes do tipo no HLSL (reprodutido com glslangValidator: erro idêntico na linha 63 col 11 dos pixel shaders, corrigido), `preTransform = IDENTITY` (contador de lado), falha de shader cacheada. Crítico: APROVADO (sem bloqueantes). |
| 2026-10-22 | teste em dispositivo (APK do `af47f36`) | 🔍 sondas necessárias | Shaders compilam e o caminho de desenho funciona (`draws 1487 recorded 1487`, readback por frame, 30 fps), mas o conteúdo apresentado segue preto — o elo quebrado (saída dos draws, cópia do jogo, decode, gamma) é invisível sem medição. Bônus: ao sair do app, `vkCreateSwapchainKHR` → `VK_ERROR_SURFACE_LOST` desarma `ok_` para sempre (96 presents estacionados no fim). Log: release `Log/ssss22_10-08-22-34_951.log`. |
| 2026-10-22 | [`39776022618`](https://github.com/deivid22srk/Forza-Android/actions/runs/39776022618) | ✅ sucesso | Build do `6f94e5c` — **rodada 5**: sondas de conteúdo em cada elo do frame (readback/copy/frame/gamma + descabeço de vértices), `copies (fail N)` no batimento, guarda de staging e **re-arm da swapchain** por epoch da surface com gate `booted_`. Crítico: REPROVADO na 1ª rodada (re-arm sem `booted_` crashava com Init falho), APROVADO após os 3 ajustes. |

## Próximo passo

1. **Teste de campo do APK da rodada 5** (artifact `forza-full-6f94e5c…` do run
   [`39776022618`](https://github.com/deivid22srk/Forza-Android/actions/runs/39776022618)):
   as sondas de conteúdo dizem onde o preto entra — `probe readback` preto = os draws não
produzem pixel (o `probe vertices` mostra quad degenerado), `probe copy` preto com readback
não-preto = a cópia do jogo para o back buffer, `probe frame` preto com copy não-preto = decode,
   `probe gamma` zerando = rampa. O batimento agora traz `copies N (fail M)`.
   (a) conferir também que **sair e voltar do app não mata mais o vídeo** (re-arm por epoch);
   (b) contador de FPS ainda em pé (IDENTITY).
2. Com o elo preto pinado, consertá-lo e seguir para o **Marco 3-c**: depth/stencil, points,
   cube/3D/depth textures, occlusion reports, `compareEnable` de sombras e o contador
   `shader_fail` no batimento — abre o caminho das cenas 3D (corrida).
