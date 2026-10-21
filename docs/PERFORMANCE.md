# Performance

Desempenho é requisito de projeto, não polimento final. Alvos mínimos:

| Perfil | SoC | CPU | GPU | Meta |
|---|---|---|---|---|
| Mínimo | MediaTek Helio G80 | 2× A75 + 6× A55 | Mali-G52 MC2 | 30 FPS, qualidade baixa |
| Mínimo | Snapdragon 480+ | 2× Kryo 460 Gold + 6× Silver | Adreno 619 | 30 FPS, qualidade baixa |
| Bom | Snapdragon 7-series/8-series | — | Adreno 6xx/7xx | 60 FPS opcional |

## Orçamento

- RAM: caber confortavelmente em **3 GB**; pico do processo < 1,5 GB.
- Frame: 33,3 ms a 30 FPS; alvo de CPU de jogo < 20 ms no G80.
- Draw calls: < 500/frame no caminho crítico; agrupar por pipeline.
- Sem alocação por frame no caminho de render/input.

## Build

- `arm64-v8a` apenas; `-O2`/`-O3`.
- ThinLTO opcional (`FORZA_LTO=ON`) depois que o build estabilizar.
- `-fvisibility=hidden`, `-ffunction-sections -fdata-sections`,
  `--gc-sections`.
- Baseline `armv8-a`; `-mtune=cortex-a55` por padrão (configurável).
- `ccache` no CI (tools e NDK).
- PGO avaliado depois do jogo rodar.

## Gráficos (Vulkan 1.1)

- Sem dynamic rendering e sem timeline semaphores obrigatórios.
- Pipeline cache em disco + cache de shaders SPIR-V.
- Render passes poucos e corretos para tile-based:
  `loadOp=CLEAR/DONT_CARE`, `storeOp=STORE/DONT_CARE`.
- Evitar readbacks, blits e barreiras desnecessárias.
- Texturas em ASTC/ETC2 quando suportado; mipmaps; DXT/paletas convertidos
  uma vez e cacheados.
- Alocador por blocos; buffers dinâmicos em anel.
- Resolução de render configurável (padrão ~50–60% da altura), upscale;
  resolução dinâmica para manter FPS.
- Aniso/MSAA/sombras extras desligados por padrão.
- Limitador de FPS (30/60) e V-Sync opcional.

## CPU, memória e sistema

- Threads: lógica nos núcleos grandes; compilação de shader, decode e I/O nos
  pequenos (afinidade Android).
- Streaming assíncrono de assets; I/O com buffers grandes.
- `onTrimMemory` e orçamento explícito.
- Térmica: reduzir resolução/FPS ao esquentar; histerese para evitar
  oscilação.
- Áudio de baixa latência (AAudio via SDL3), sem underruns.
- Lifecycle correto: pausar, perder/recriar surface, rotação, background.

## Medição

- Contadores internos: frame time (CPU/GPU), draw calls, memória, stalls.
- Overlay de FPS (marco 3+).
- Registrar aqui cada otimização e o ganho medido, com dispositivo/SoC.

## Resultados

| Data | Dispositivo | Build | FPS | Observações |
|---|---|---|---|---|
| — | — | — | — | Aguardando primeiro build real. |
