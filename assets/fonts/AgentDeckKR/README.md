# AgentDeckKR — bundled Korean font for the AgentDeck dashboard

`AgentDeckKR_12.cpfont` is **Noto Sans KR** (a.k.a. Source Han Sans K), rasterized
at 12px with ASCII + full Hangul coverage via `lib/EpdFont/scripts/fontconvert_sdcard.py`.

- **License:** SIL Open Font License 1.1 (see `OFL.txt`). Copyright Adobe, with
  Reserved Font Name *Source* — this derived bitmap is named `AgentDeckKR`, which
  does not use the reserved name, so redistribution is permitted.
- **Why bundled:** the built-in UI fonts are Latin-only and `EpdFont` resolves a
  glyph's bitmap against its own font, so per-glyph fallback isn't possible. The
  dashboard instead loads this font as an auxiliary SD font and renders any line
  containing Hangul/CJK with it (`AgentDashboardActivity::loadKoreanFont` →
  `fontForText`). Independent of the user's reader font selection.

## Shipping to the device

Copy to the SD card at **`/.fonts/AgentDeckKR/AgentDeckKR_12.cpfont`** (the hidden
`/.fonts/` root the firmware scans; `/fonts/` also works). The firmware loads it on
entering the AgentDeck dashboard; if absent, Korean falls back to the reader's font
(when it's a CJK family) or renders as □.

## Regenerating

```
python lib/EpdFont/scripts/fontconvert_sdcard.py \
  --intervals ascii,hangul --size 12 --style regular \
  NotoSansKR-Regular.ttf -o AgentDeckKR_12.cpfont
```
