# Progress

## Marcos

| # | Marco | Status | Notas |
|---|---|---|---|
| 1 | Esqueleto: repo, CMake modular, app Android, `build.yml`, recompilação no CI, APK que abre e mostra log | 🟢 concluído | Pipeline completo validado no CI e instalado em dispositivo: app acha o jogo, confere SHA-1 e inicia o boot. |
| 2 | Boot: `GAPS.md`, kernel/XDK suficientes para passar das intros | 🔄 em andamento | Primeiro boot em dispositivo parou no export 76 (`IoQueryVolumeInformation`); os 3 imports ausentes do kernel foram implementados em `native/runtime/overrides.cpp`, aguardando novo APK. |
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
  `XcPKGetKeyLen` 343).
- Modo stub: APK instalável sem o jogo, com diálogo explicando o secret.
- Host Android: SDL3, storage, gamepad, áudio SDL3, null backend.
- Runtime do SDK compilado para arm64 (kernel, D3D8, DSOUND, Bink, XAPI).
- `docs/GAPS.md` com análise real do XBE (XDK 5849, bibliotecas, seções).

## O que falta / quebrado

- Boot no dispositivo para no primeiro export de kernel sem implementação. No
  primeiro teste foi `IoQueryVolumeInformation` (76), chamado pelo XAPI logo no
  início do boot (`guest trap at 002674C8`, SIGABRT). Corrigido em
  `native/runtime/overrides.cpp`; **falta testar de novo** — pode haver outro
  problema adiante.
- Backend Vulkan real (marco 3); o atual é no-op. Tela preta é esperada no marco 1.
- Toque, giroscópio, menu de opções, instalador de assets.
- XACT/XOnline/XNet/WMADEC não validados.

## Últimos runs

| Data | Run | Resultado | Notas |
|---|---|---|---|
| 2026-10-21 | [`39617521020`](https://github.com/deivid22srk/Forza-Android/actions/runs/39617521020) | ✅ sucesso (7m1s) | Build full; artifact `forza-full-0ddc3f4…` com APK de 23 MB. |
| 2026-10-21 | [`39618808504`](https://github.com/deivid22srk/Forza-Android/actions/runs/39618808504) | ✅ sucesso (5m12s) | Build full do `c54bd9c`; APK instalado no dispositivo. |
| 2026-10-21 | teste em dispositivo (APK do `0ddc3f4`) | ❌ crash no boot | `kernel export 76 (IoQueryVolumeInformation) is not implemented` → `guest trap` → SIGABRT; log: <https://pastebin.com/raw/2BFQhsGf>. |

## Próximo passo

1. Build novo no CI com `native/runtime/overrides.cpp` e instalar o APK novo.
2. Capturar `adb logcat -s FORZA` e ver até onde o boot chega agora (tela preta
   é esperada: backend de vídeo ainda é nulo).
3. Repetir o ciclo correção/asset até o boot estabilizar; então começar o
   backend Vulkan mínimo (clear/present).
