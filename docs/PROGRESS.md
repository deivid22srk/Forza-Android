# Progress

## Marcos

| # | Marco | Status | Notas |
|---|---|---|---|
| 1 | Esqueleto: repo, CMake modular, app Android, `build.yml`, recompilação no CI, APK que abre e mostra log | 🟢 concluído | Pipeline completo validado no CI; APK **full** arm64 gerado. Falta testar no dispositivo. |
| 2 | Boot: `GAPS.md`, kernel/XDK suficientes para passar das intros | ⬜ | |
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
- Build **full** validado no CI (run `39617521020`): APK de 23 MB com
  `lib/arm64-v8a/libmain.so` de ~20 MB.
- Modo stub: APK instalável sem o jogo, com diálogo explicando o secret.
- Host Android: SDL3, storage, gamepad, áudio SDL3, null backend.
- Runtime do SDK compilado para arm64 (kernel, D3D8, DSOUND, Bink, XAPI).
- `docs/GAPS.md` com análise real do XBE (XDK 5849, bibliotecas, seções).

## O que falta / quebrado

- APK full ainda não testado em dispositivo (instalar, copiar o jogo e capturar `adb logcat -s FORZA`).
- Backend Vulkan real (marco 3); o atual é no-op.
- Toque, giroscópio, menu de opções, instalador de assets.
- XACT/XOnline/XNet/WMADEC não validados.

## Últimos runs

| Data | Run | Resultado | Notas |
|---|---|---|---|
| 2026-10-21 | [`39617521020`](https://github.com/deivid22srk/Forza-Android/actions/runs/39617521020) | ✅ sucesso (7m1s) | Build full; artifact `forza-full-0ddc3f4…` com APK de 23 MB. |

## Próximo passo

1. Baixar o artifact do run `39617521020` e instalar o APK no aparelho.
2. Copiar `default.xbe` (ou a ISO) para `/storage/emulated/0/Forza/` e conceder acesso.
3. Capturar `adb logcat -s FORZA` e ver até onde o boot chega.
4. Corrigir os erros de runtime e implementar o backend Vulkan mínimo (clear/present).
