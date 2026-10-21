# Progress

## Marcos

| # | Marco | Status | Notas |
|---|---|---|---|
| 1 | Esqueleto: repo, CMake modular, app Android, `build.yml`, recompilação no CI, APK que abre e mostra log | 🟡 em andamento | Estrutura e pipeline prontos; stub APK verde; falta o secret `FORZA_XBE_URL` para o build real. |
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
- Pipeline: download do XBE → `xbe_symbols` → `xbe_recomp` → NDK → APK.
- Modo stub: APK instalável sem o jogo, com diálogo explicando o secret.
- Host Android: SDL3, storage, gamepad, áudio SDL3, null backend.
- Runtime do SDK compilado para arm64 (kernel, D3D8, DSOUND, Bink, XAPI).
- `docs/GAPS.md` com análise real do XBE (XDK 5849, bibliotecas, seções).

## O que falta / quebrado

- O build completo depende de `FORZA_XBE_URL`; sem ele, só o stub é gerado.
- Backend Vulkan real (marco 3); o atual é no-op.
- Toque, giroscópio, menu de opções, instalador de assets.
- XACT/XOnline/XNet/WMADEC não validados.
- Nenhum teste em dispositivo ainda.

## Últimos runs

| Data | Run | Resultado | Notas |
|---|---|---|---|
| — | — | — | Primeiro push a caminho. |

## Próximo passo

1. Configurar `FORZA_XBE_URL` (e opcionalmente `FORZA_XBE_SHA256`).
2. Rodar `build.yml` e ler o log do `xbe_symbols`/`xbe_recomp`.
3. Corrigir erros de compilação/integração do runtime no Android.
4. Implementar o backend Vulkan mínimo (clear/present) e o overlay de log.
