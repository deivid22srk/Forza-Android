# Progress

## Marcos

| # | Marco | Status | Notas |
|---|---|---|---|
| 1 | Esqueleto: repo, CMake modular, app Android, `build.yml`, recompilação no CI, APK que abre e mostra log | 🟢 concluído | Pipeline completo validado no CI e instalado em dispositivo: app acha o jogo, confere SHA-1 e inicia o boot. |
| 2 | Boot: `GAPS.md`, kernel/XDK suficientes para passar das intros | 🔄 em andamento | Imports de kernel todos resolvidos (0 missing no boot real); o guest cria o device D3D8, roda a sequência de splash e o instalador BKGDINST descompacta os cabs. Falta observar um boot longo até o estado de menu para fechar o marco. |
| 3 | Vídeo: D3D8 → Vulkan renderizando menus | ⬜ | null backend no lugar do Vulkan real. |
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
- Backend Vulkan real (marco 3); o atual é no-op. **Tela preta é esperada**
  mesmo com o jogo presente: o objetivo do teste é o log do boot, não vídeo.
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

## Próximo passo

1. Instalar o APK do `088e96f` (build mais recente) e rodar o teste **longo**:
   abrir o app e deixá-lo 10–15 min sem fechar, com o aparelho carregando, até
   o instalador `BKGDINST` terminar os cabs e o boot chegar ao primeiro menu.
2. Capturar `adb logcat -s FORZA` inteiro (ou o arquivo `logs/forza.log` em
   `Android/data/com.deivid.forza/files/`) e conferir: sem `trap`/crash; ao
   fechar e reabrir, `Z:\InstallMarker*.dat` deve ser achado no lugar de
   "missing" (instalação pulada).
3. Se o boot alcançar o estado de menu estável, fechar o **Marco 2** e começar
   o **Marco 3**: backend Vulkan mínimo (clear + present) substituindo o null
   backend, depois menus.
4. Tela preta continua esperada durante o Marco 3 até o primeiro clear/present.
