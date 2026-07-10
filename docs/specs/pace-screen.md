# Espec — Tela "Ritmo" (Pace)

> Status: aprovada em protótipo visual, aguardando implementação.
> Placa-alvo: **Waveshare ESP32-S3 Touch-AMOLED-2.16** (480×480), env `waveshare_amoled_216`.
> Idioma da UI: **português** (decisão do dono do projeto para esta tela).

## Objetivo

Responder de relance a uma única pergunta: **"no ritmo atual de uso, eu estouro o
limite da sessão antes do reset?"** — a tela Usage mostra onde o consumo *está*;
a tela Ritmo mostra onde ele *vai parar*.

## Referência visual

Protótipo aprovado (2 estados):

- **Estado 1 — no ritmo**: gauge verde, "62% / até o reset", status verde
  `No ritmo · reset 16:10`, stats `ritmo 9%/h` e `pico hoje 14h`, histograma
  de 24h majoritariamente verde.
- **Estado 2 — vai estourar**: gauge vermelho cheio, "100% / até o reset",
  status vermelho `Esgota ~14:32 · reset 16:10`, stats `ritmo 28%/h` e
  `pico hoje 11h`, histograma com horas de pico em âmbar/vermelho.

## Copy (tudo em PT)

| Elemento | Texto | Observação |
|---|---|---|
| Título | **Ritmo** | Tiempos 56, como as demais telas. ("Pace" do protótipo traduzido — decisão "o mais português possível".) |
| Sublabel do gauge | **até o reset** | Substitui "projetado no reset" (copy vetada). Lê-se junto com o número: "62% até o reset". Alternativas descartáveis: "previsão até o reset", "no ritmo atual". |
| Status verde | `No ritmo · reset 16:10` | |
| Status âmbar | `No limite · reset 16:10` | Faixa intermediária, ver regras. |
| Status vermelho | `Esgota ~14:32 · reset 16:10` | Hora estimada de bater 100%. |
| Status aquecendo | `Medindo ritmo… · reset 16:10` | ~4 min após o boot, ver regras. |
| Sem dados | `Sem dados` | BLE ainda não entregou payload válido. |
| Stat esquerda | rótulo **agora**, valor `9%/h` | Rótulo era "ritmo" no protótipo; renomeado para não repetir o título. |
| Stat direita | rótulo **pico hoje**, valor `14h` | Hora do dia com maior utilização. |
| Eixo do histograma | `0h` / `12h` / `23h` | Igual ao protótipo. |

## Layout (480×480)

De cima para baixo, tudo posicionado via `compute_layout()` (breakpoint grande,
`H >= 460`); valores finais ajustados no QA visual por screenshot:

1. **Título** "Ritmo" centralizado em `title_y` + ícone de bateria no canto
   superior direito (elemento global já existente, nada a fazer).
2. **Gauge**: `lv_arc` ~200 px, rotação 135°, span 270° (mesmo padrão do
   `arc_cache` da tela Stats), trilho `COL_BAR_BG` largura 16, indicador na cor
   do estado. Valor do arco = projeção (0–100).
3. Dentro do arco: **número grande** `NN%` (`font_styrene_48`, `COL_TEXT`, sem
   casas decimais) e **sublabel** "até o reset" (`font_styrene_16`, `COL_DIM`).
4. **Linha de status** (`font_styrene_20`, cor do estado), centralizada.
5. **Linha de stats**: duas colunas (esquerda/direita), rótulo em
   `font_styrene_16` `COL_DIM`, valor em `font_styrene_24` `COL_TEXT`.
6. **Histograma 24h**: 24 barras na base da tela, mesma geometria e coloração do
   heatmap da tela Stats (código fatorado em helper comum, ver arquitetura).
   Rótulos `0h`/`12h`/`23h` em `font_styrene_12`/`14` `COL_DIM`.

Margens laterais ≥ 20 px (cantos arredondados do painel). Cores exclusivamente
via tokens de `theme.h` (`COL_GREEN`, `COL_AMBER`, `COL_RED`, `COL_DIM`,
`COL_BAR_BG`, `COL_TEXT`) — nenhum hex novo.

O layout compacto (368×448) recebe apenas o degrau responsivo mínimo para
compilar e não quebrar nas outras placas; não é alvo de QA visual nesta versão
(nessas placas a tela nem é alcançável — ver Navegação).

## Regras de cálculo

Todos os cálculos rodam **no firmware** — nenhuma mudança nos daemons nem no
payload BLE. Insumos já presentes em `UsageData`: `session_pct`,
`session_reset_mins`, `clock_epoch`, `clock_fmt`, `hourly[24]`.

- **ritmo (%/h)**: `usage_rate_per_hour()`, nova função de `usage_rate.cpp`
  sobre o ring buffer existente (janela mínima de 4 min, 6 amostras @ ~60 s).
  Retorna `-1` enquanto a janela não aqueceu. Ritmo negativo trunca em 0.
- **projeção** = `clamp(session_pct + ritmo × session_reset_mins / 60, 0, 100)`.
- **estado**: projeção `< 85` → verde · `85–99` → âmbar · `≥ 100` → vermelho.
- **hora de esgotamento** (só no estado vermelho, ritmo > 0):
  `agora + (100 − session_pct) / ritmo` horas, formatada conforme `clock_fmt`
  (12/24 h) a partir de `clock_epoch`.
- **reset HH:MM** = `clock_epoch + session_reset_mins × 60`. Se
  `clock_epoch == 0` (daemon sem relógio), usar formato relativo:
  `reset em 3h42` / `esgota em ~1h20`.
- **pico hoje** = argmax de `hourly[]` (empate → hora mais cedo). Tudo zero →
  `—` (é o caso do daemon bash, que ainda não envia `hm`).
- **cores das barras**: `0` → `COL_BAR_BG` · `1–49` → verde · `50–79` → âmbar ·
  `≥ 80` → vermelho (idêntico ao heatmap da Stats).

### Estados especiais

| Situação | Comportamento |
|---|---|
| Warm-up (ring < 2 amostras ou < 4 min) | Gauge mostra `session_pct` atual, stat "agora" = `—`, status dim "Medindo ritmo…". |
| Ritmo ~0 (ocioso) | Projeção ≈ valor atual; estado verde normal. |
| `session_reset_mins == -1` | Sem projeção; gauge = valor atual, status só com o que houver. |
| Reset da sessão (pct despenca) | `usage_rate_sample()` já zera o ring; a tela volta ao warm-up naturalmente. |
| Conta Enterprise (`acct == "ent"`) | O modelo de janela de 5 h não se aplica (pct = gasto mensal). Gauge = valor atual, sem projeção; status `Reset <data>` usando `reset_date`. |
| Sem payload válido (`!valid`) | `—` no gauge e status "Sem dados" (padrão das outras telas). |

## Navegação

- **2.16 (alvo)**: o quadrante 2 do IMU (tela de cabeça para baixo) passa a
  mapear para `SCREEN_PACE` **no lugar de `SCREEN_STATS`** (`qmap` em
  `main.cpp`). Justificativa: o histograma do Ritmo já cobre a metade mais útil
  da Stats; o anel de cache-hit é o único conteúdo perdido e pode voltar depois
  como linha discreta se fizer falta. A tela Stats permanece no código
  (compilada, não mapeada).
- **Demais placas (1.8, C6)**: sem mudança — nelas só a tela Usage é alcançável
  hoje (PWR = brilho, tap = splash), e continuará assim. Dar acesso ao Ritmo
  nessas placas é follow-up, não escopo desta versão.
- Tap na tela Ritmo: `global_click_cb` (alterna com o splash), igual às demais.

## Arquitetura / arquivos tocados

| Arquivo | Mudança |
|---|---|
| `firmware/src/usage_rate.{h,cpp}` | Nova `float usage_rate_per_hour(void)` (−1 durante warm-up). API de grupos existente intocada. |
| `firmware/src/ui.h` | `SCREEN_PACE` no enum (antes de `SCREEN_COUNT`). |
| `firmware/src/ui.cpp` | `init_pace_screen()` + atualização em `ui_update()`; fatorar o heatmap da Stats (`update_heatmap_bars` + construção das barras) em helper parametrizado usado pelas duas telas; campos novos no `Layout`/`compute_layout()`. |
| `firmware/src/main.cpp` | `qmap[2] = SCREEN_PACE`; passar o ritmo para a UI (a amostragem `usage_rate_sample()` já acontece ali). |

Sem mudanças em: daemons (bash/macOS/Windows), payload BLE, `data.h`, HAL,
pastas de board.

## Plano de implementação (ordem de commits)

1. **`feat(rate): expõe usage_rate_per_hour()`** — mudança pequena e isolada em
   `usage_rate.{h,cpp}`.
2. **`refactor(ui): fatora heatmap da Stats em helper reutilizável`** — sem
   mudança visual; build + screenshot da Stats para confirmar zero regressão.
3. **`feat(ui): tela Ritmo`** — enum, `init_pace_screen()`, cálculos, layout.
4. **`feat(216): quadrante 2 → Ritmo`** — troca de uma linha no `qmap`.
5. **QA visual**: bootar temporariamente em `SCREEN_PACE` (troca do
   `ui_show_screen(SCREEN_SPLASH)` em `main.cpp`), `./screenshot.sh` nos dois
   estados (forçar dados de teste se preciso), iterar, reverter o boot padrão.
   Requer a placa no USB — fica para uma sessão local; em sessão remota o QA se
   limita a compilar os 4 envs.

Cada etapa compila nos 4 envs (`waveshare_amoled_216`, `waveshare_amoled_18`,
`waveshare_amoled_216_c6`, `waveshare_amoled_18_c6`) antes do commit.

## Fora de escopo / follow-ups

- `hm` (heatmap horário) no daemon bash — hoje só os daemons Python enviam.
- Acesso à tela Ritmo em placas sem IMU (ex.: tap cicla telas).
- Persistência do ritmo entre reboots do dispositivo (exigiria cálculo no
  daemon e campo novo no payload).
- Anel de cache-hit reposicionado (se a perda da Stats no quadrante 2 doer).
