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

- ~~Boot no dispositivo para no primeiro export de kernel sem implementação~~
  **Corrigido**: `IoQueryVolumeInformation` (76) e os outros dois imports
  ausentes estão em `native/runtime/overrides.cpp`; o APK `59704e9` não
  aborta mais.
- Com o APK novo o app fica vivo esperando o jogo: o teste de 2026-10-21
  terminou em `no game files in /storage/emulated/0/Forza ... waiting for
  them` — a pasta do jogo estava vazia. É o `app::FindGame` em loop, não um
  bug.
- Ainda falta o jogo completo no aparelho: só o `default.xbe` não basta, o
  jogo vai abrir os arquivos de dados do disco. Ideal: a **ISO** inteira em
  `/storage/emulated/0/Forza/` (qualquer nome `.iso`) ou o disco extraído
  junto do XBE.
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
| 2026-10-21 | teste em dispositivo (APK do `59704e9`) | ⚠️ sem crash, sem jogo | App fica vivo; log: `no game files in /storage/emulated/0/Forza … waiting for them` (pasta vazia); log: <https://pastebin.com/raw/sg0EnNjn>. |

## Próximo passo

1. Por o jogo de volta em `/storage/emulated/0/Forza/`: o `default.xbe` (SHA-1
   `42f796aa…`) **e** os dados do disco — o ideal é a **ISO inteira**
   (qualquer nome, extensão `.iso`); o runtime monta imagens XISO, ou os
   arquivos extraídos ao lado do XBE.
2. Capturar `adb logcat -s FORZA` e ver até onde o boot chega agora (tela
   preta é esperada: backend de vídeo ainda é nulo; o que importa é o log).
3. Repetir o ciclo correção/asset até o boot estabilizar; então começar o
   backend Vulkan mínimo (clear/present).
