#!/usr/bin/env python3
"""Build a self-contained HTML usage report from the daemon's usage_log.jsonl.

The Windows/macOS daemons append one JSON-lines record per poll (~60s) with
session/weekly utilization, cache-hit, and sprint fields. This reads that log
and renders a dark, on-brand HTML page with inline SVG charts — no external
dependencies (stdlib only), so it runs in any Python 3.9+.

    python tools/usage_report.py [path/to/usage_log.jsonl] [-o report.html]

With no log path it defaults to %LOCALAPPDATA%\\Clawdmeter\\usage_log.jsonl
(Windows) or ~/.local/share not being used — falls back to the macOS/Linux
Clawdmeter dir. Writes report.html next to the log unless -o is given.
"""
import argparse
import datetime as dt
import html
import json
import os
import statistics
from pathlib import Path

ACCENT = "#d97757"
GREEN = "#7bb662"
AMBER = "#d9a55a"
DIM = "#8a827a"
BG = "#191917"
PANEL = "#242220"
TEXT = "#ece7e1"


def default_log_path() -> Path:
    base = os.environ.get("LOCALAPPDATA")
    if base:
        return Path(base) / "Clawdmeter" / "usage_log.jsonl"
    return Path.home() / "Library" / "Application Support" / "Clawdmeter" / "usage_log.jsonl"


def load_records(path: Path) -> list[dict]:
    recs = []
    with path.open(encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                r = json.loads(line)
            except json.JSONDecodeError:
                continue
            if "ts" in r:
                recs.append(r)
    recs.sort(key=lambda r: r["ts"])
    return recs


def _poly(points: list[tuple[float, float]]) -> str:
    return " ".join(f"{x:.1f},{y:.1f}" for x, y in points)


def line_chart(series: list[tuple[float, float]], color: str, w=680, h=180,
               ymax=100.0, label="") -> str:
    """series: list of (epoch, value). Returns an inline SVG line chart."""
    if len(series) < 2:
        return f'<div class="empty">Sem dados suficientes para "{html.escape(label)}"</div>'
    t0, t1 = series[0][0], series[-1][0]
    span = max(t1 - t0, 1)
    pad = 8
    pts = []
    for t, v in series:
        x = pad + (t - t0) / span * (w - 2 * pad)
        y = pad + (1 - min(v, ymax) / ymax) * (h - 2 * pad)
        pts.append((x, y))
    grid = ""
    for frac in (0.25, 0.5, 0.75):
        gy = pad + frac * (h - 2 * pad)
        grid += f'<line x1="{pad}" y1="{gy:.0f}" x2="{w-pad}" y2="{gy:.0f}" stroke="{DIM}" stroke-opacity="0.18"/>'
    return (
        f'<svg viewBox="0 0 {w} {h}" class="chart" xmlns="http://www.w3.org/2000/svg">'
        f'{grid}'
        f'<polyline fill="none" stroke="{color}" stroke-width="2" '
        f'stroke-linejoin="round" points="{_poly(pts)}"/>'
        f'</svg>'
    )


def hour_heatmap(recs: list[dict]) -> str:
    """Avg session % per weekday x hour — the 'peaks & times' view."""
    days = ["Seg", "Ter", "Qua", "Qui", "Sex", "Sáb", "Dom"]
    buckets: dict[tuple[int, int], list[float]] = {}
    for r in recs:
        if "s" not in r:
            continue
        d = dt.datetime.fromtimestamp(r["ts"])
        buckets.setdefault((d.weekday(), d.hour), []).append(float(r["s"]))
    if not buckets:
        return '<div class="empty">Sem dados de horário ainda</div>'
    cell, gap = 24, 3
    left, top = 40, 18
    w = left + 24 * (cell + gap)
    h = top + 7 * (cell + gap)
    svg = [f'<svg viewBox="0 0 {w} {h}" class="heat" xmlns="http://www.w3.org/2000/svg">']
    for hh in range(0, 24, 3):
        x = left + hh * (cell + gap)
        svg.append(f'<text x="{x}" y="12" fill="{DIM}" font-size="10">{hh}h</text>')
    for wd in range(7):
        y = top + wd * (cell + gap)
        svg.append(f'<text x="0" y="{y+cell*0.7:.0f}" fill="{DIM}" font-size="10">{days[wd]}</text>')
        for hh in range(24):
            x = left + hh * (cell + gap)
            vals = buckets.get((wd, hh))
            if vals:
                avg = statistics.mean(vals)
                op = 0.12 + min(avg, 100) / 100 * 0.88
                svg.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" rx="4" '
                           f'fill="{ACCENT}" fill-opacity="{op:.2f}"><title>{days[wd]} {hh}h · {avg:.0f}%</title></rect>')
            else:
                svg.append(f'<rect x="{x}" y="{y}" width="{cell}" height="{cell}" rx="4" '
                           f'fill="{PANEL}"/>')
    svg.append("</svg>")
    return "".join(svg)


def stat_card(label: str, value: str, color: str = TEXT) -> str:
    return (f'<div class="card"><div class="cval" style="color:{color}">{html.escape(value)}</div>'
            f'<div class="clbl">{html.escape(label)}</div></div>')


def build_html(recs: list[dict]) -> str:
    if recs:
        t0 = dt.datetime.fromtimestamp(recs[0]["ts"])
        t1 = dt.datetime.fromtimestamp(recs[-1]["ts"])
        span_txt = f"{t0:%d/%m %H:%M} — {t1:%d/%m %H:%M}"
        days = max(1, (t1.date() - t0.date()).days + 1)
    else:
        span_txt, days = "sem dados", 0

    s_vals = [r["s"] for r in recs if "s" in r]
    w_vals = [r["w"] for r in recs if "w" in r]
    ch_vals = [r["ch"] for r in recs if "ch" in r]

    cards = [
        stat_card("Registros", str(len(recs))),
        stat_card("Período", f"{days} dia(s)"),
        stat_card("Sessão média", f"{statistics.mean(s_vals):.0f}%" if s_vals else "—", ACCENT),
        stat_card("Sessão pico", f"{max(s_vals):.0f}%" if s_vals else "—", ACCENT),
        stat_card("Semanal atual", f"{w_vals[-1]:.0f}%" if w_vals else "—", AMBER),
        stat_card("Cache hit médio", f"{statistics.mean(ch_vals):.0f}%" if ch_vals else "—", GREEN),
    ]

    s_series = [(r["ts"], float(r["s"])) for r in recs if "s" in r]
    w_series = [(r["ts"], float(r["w"])) for r in recs if "w" in r]

    # Sprint block (last known)
    sprint_html = ""
    bd = next((r for r in reversed(recs) if "bd_sn" in r), None)
    if bd:
        sprint_html = (
            f'<h2>Sprint</h2><div class="panel"><div class="sprintline">'
            f'<b>{html.escape(str(bd.get("bd_sn","")))}</b>'
            f'<span>To Do <b style="color:{DIM}">{bd.get("bd_td","–")}</b></span>'
            f'<span>Doing <b style="color:{AMBER}">{bd.get("bd_dg","–")}</b></span>'
            f'<span>Done <b style="color:{GREEN}">{bd.get("bd_dn","–")}</b></span>'
            f'<span>Total <b>{bd.get("bd_tt","–")}</b></span>'
            f'</div></div>')

    gen = dt.datetime.now().strftime("%d/%m/%Y %H:%M")
    return f"""<!doctype html><html lang="pt-br"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>Clawdmeter · Relatório de uso</title>
<style>
:root{{color-scheme:dark}}
body{{margin:0;background:{BG};color:{TEXT};font-family:system-ui,-apple-system,Segoe UI,sans-serif;padding:28px;max-width:760px;margin:0 auto}}
h1{{font-size:24px;margin:0 0 2px}} h2{{font-size:16px;margin:30px 0 10px;color:{DIM};font-weight:600}}
.sub{{color:{DIM};font-size:13px;margin-bottom:22px}}
.cards{{display:grid;grid-template-columns:repeat(auto-fit,minmax(110px,1fr));gap:10px}}
.card{{background:{PANEL};border-radius:12px;padding:14px}}
.cval{{font-size:22px;font-weight:700}} .clbl{{color:{DIM};font-size:12px;margin-top:2px}}
.panel{{background:{PANEL};border-radius:12px;padding:14px;overflow-x:auto}}
.chart,.heat{{width:100%;height:auto;display:block}}
.sprintline{{display:flex;gap:18px;flex-wrap:wrap;align-items:center;font-size:14px}}
.sprintline span{{color:{DIM};font-size:12px}}
.empty{{color:{DIM};font-size:13px;padding:20px;text-align:center}}
.foot{{color:{DIM};font-size:11px;margin-top:28px}}
</style></head><body>
<h1>Relatório de uso · Clawdmeter</h1>
<div class="sub">{html.escape(span_txt)}</div>
<div class="cards">{''.join(cards)}</div>
<h2>Sessão (limite de 5h) ao longo do tempo</h2>
<div class="panel">{line_chart(s_series, ACCENT, label="Sessão %")}</div>
<h2>Semanal (limite de 7d)</h2>
<div class="panel">{line_chart(w_series, AMBER, label="Semanal %")}</div>
<h2>Quando você mais usa (sessão % média · dia × hora)</h2>
<div class="panel">{hour_heatmap(recs)}</div>
{sprint_html}
<div class="foot">Gerado em {gen} · fonte: usage_log.jsonl</div>
</body></html>"""


def main() -> int:
    ap = argparse.ArgumentParser(description="Relatório de uso do Clawdmeter")
    ap.add_argument("log", nargs="?", type=Path, default=None,
                    help="caminho do usage_log.jsonl (default: dir do Clawdmeter)")
    ap.add_argument("-o", "--out", type=Path, default=None,
                    help="arquivo HTML de saída (default: report.html ao lado do log)")
    args = ap.parse_args()

    log_path = args.log or default_log_path()
    if not log_path.exists():
        print(f"log não encontrado: {log_path}")
        print("O daemon ainda não gravou dados? Deixe-o rodar alguns polls.")
        return 1
    recs = load_records(log_path)
    if not recs:
        print(f"log vazio ou sem registros válidos: {log_path}")
        return 1
    out = args.out or log_path.with_name("report.html")
    out.write_text(build_html(recs), encoding="utf-8")
    print(f"relatório: {out}  ({len(recs)} registros)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
