# Bilingual Book Pipeline (request → translate → read on device)

This is the end-to-end guide for *producing* bilingual EPUBs and getting them
onto an X3 / X4, complementing [bilingual-epub.md](./bilingual-epub.md) (the
on-device format contract) and [korean-font-setup.md](./korean-font-setup.md)
(rendering Korean glyphs).

**Goal:** tell an agent the book you want, have it found, translated, and
delivered to the device — read it with the in-reader Both / Original /
Translation toggle.

## What lives where

The firmware in *this* repo is a **consumer**. It already does everything the
device needs — OPDS browsing/download, the bilingual reader, and SD-card Korean
fonts — so **no firmware code is added for this pipeline.** Everything that
*produces* a bilingual EPUB lives outside this repo, keeping the open-source
firmware clean and book-logic-free.

| Stage | Component | Where |
|-------|-----------|-------|
| Request / search / pick | OpenClaw agent (off-device chat) | external (`~/github/OpenClaw/`) |
| Find & fetch source | `book_translator` source adapters | external |
| Translate | GLM 5.2 (Zhipu AI, OpenAI-compatible chat API) | external |
| Assemble bilingual EPUB | `book_translator` EPUB assembler | external |
| Publish to library | Booklore + Calibre Content Server | external (self-hosted) |
| Serve catalog | Calibre Content Server OPDS feed | external |
| Browse / download | `OpdsBookBrowserActivity` | **this repo (exists)** |
| Read bilingual | `ChapterHtmlSlimParser` + toggle | **this repo (exists)** |
| Render Korean | RIDIBatang SD font | **this repo (exists)** |

## End-to-end flow

```
[you ↔ OpenClaw agent]   off-device: "Translate Pride and Prejudice to Korean, bilingual"
        │  search (gutendex / Standard Ebooks / local file) → you pick a match
        ▼
[book_translator]        extract source text → GLM 5.2 paragraph-wise translation
        │                assemble EPUB: <p class="cp-original">…</p><p class="cp-translation">…</p>
        ▼
[Booklore → Calibre Content Server]   add to library, expose OPDS feed
        ▼
[X3 / X4 OPDS browser]   Settings → OPDS → browse/search → download to SD root
        ▼
[bilingual reader + RIDIBatang]   long-press Confirm cycles Both → Original → Translation
```

The **request and selection are entirely off-device** — you talk to the
OpenClaw agent from your phone/desktop. There is **no completion push** to the
device by design; when the agent reports done, open the OPDS browser and the new
book is there.

## Device setup (one time)

1. **Korean font** — install RIDIBatang per
   [korean-font-setup.md](./korean-font-setup.md), then **Settings → Reader →
   Font Family → RIDIBatang**. Without it, `cp-translation` paragraphs show tofu (□).
2. **OPDS server** — point the device at your Calibre Content Server:
   - **Settings → OPDS → Add server**
     ([OpdsSettingsActivity](../src/activities/settings/OpdsSettingsActivity.h),
     stored in `OpdsServerStore` → `/.crosspoint/opds.json`, up to 8 servers).
   - Fields map 1:1 to the Content Server: `name` (label), `url`
     (e.g. `http://<host>:8080/opds`), `username`, `password` (XOR-obfuscated
     with the device MAC at rest).
3. **Bilingual toggle** — **Settings → Controls → Long-press Menu → Bilingual
   Toggle**. In the reader, long-press Confirm (~0.4 s) cycles
   Both → Original only → Translation only (see [bilingual-epub.md](./bilingual-epub.md)).

## Producing a book (off-device)

The `book_translator` pipeline (in the OpenClaw repo) is driven by the OpenClaw
agent. It must, for each book:

1. **Find the source.** Pluggable adapters so sources are extensible:
   - **Project Gutenberg** via the gutendex API (public domain — safe to automate).
   - **Standard Ebooks** via its OPDS/atom catalog (better-typeset public domain).
   - **Local file** ingest of an EPUB/txt you already own (your copyright responsibility).
2. **Extract** the source into a per-chapter paragraph list.
3. **Translate** each paragraph with GLM 5.2, batched with surrounding context for
   coherence, producing `(original, korean)` pairs.
4. **Assemble** a bilingual EPUB whose every paragraph pair emits the marker
   classes the reader requires — `class="cp-original"` and `class="cp-translation"`,
   **block-level `<p>` only, token-exact** (see
   [bilingual-epub.md](./bilingual-epub.md) for the full contract). Title the book
   with a "(KO bilingual)" suffix so it's distinguishable in the library.
5. **Publish** to Booklore / the Calibre library (`calibredb add` or Booklore API)
   so Calibre Content Server serves it over OPDS.

The assembler is a content-driven extension of the reference generator already in
this repo, [`scripts/generate_bilingual_test_epub.py`](../scripts/generate_bilingual_test_epub.py)
(which emits the exact same EPUB skeleton from hard-coded sample paragraphs). Use
that script's structure as the template for real output.

## Verifying the chain

Work outward from the contract this repo owns:

1. **Format smoke test (this repo, no network):**
   ```bash
   python3 scripts/generate_bilingual_test_epub.py
   unzip -p test/epubs/bilingual-sample.epub OEBPS/chapter1.xhtml \
     | grep -o 'class="cp-[a-z]*"' | sort -u
   #   class="cp-original"
   #   class="cp-translation"
   ```
   Copy `bilingual-sample.epub` to the SD card and confirm the toggle works
   *before* wiring the full pipeline.
2. **Pipeline output (OpenClaw):** run on one short Gutenberg book; grep the
   produced EPUB for the `cp-*` classes as above; confirm it opens in Calibre.
3. **Serving:** `curl` the Content Server OPDS URL and confirm the new book
   appears as an acquisition entry.
4. **Device (human tester):** add the OPDS server, browse/search, download, open,
   and verify (a) the toggle cycles all three modes and (b) Korean renders with
   RIDIBatang selected.

## Not in scope (firmware)

By design the device only consumes. There is intentionally **no** on-device book
request, no completion push notification, and no agent-command UI — requests run
off-device and results are found via the existing OPDS browser. A future
device-initiated request would need an AgentDeck outbound message + a text-entry
hook ([KeyboardEntryActivity](../src/activities/util/KeyboardEntryActivity.cpp));
that is explicitly out of scope here.
