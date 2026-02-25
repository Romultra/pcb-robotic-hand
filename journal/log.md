# Engineering Notebook — BSc Thesis PCB Design

Chronological log of research, notes, and progress. Entries can be quick one-liners
or longer write-ups — whatever fits the moment.

---

## 2026-02-25

### Setting up project documentation workflow

Explored how to track research, design decisions, and progress throughout the thesis
without things getting scattered. Considered several approaches:

- **Engineering notebook** (markdown in repo) vs **PKM apps** (Logseq, Obsidian) — went
  with plain markdown in the repo. Lower friction, no extra tools, version-controlled,
  and avoids context switching between apps.
- **Zotero vs Logseq** — minimal overlap. Zotero is for references and BibTeX export,
  Logseq is for note-taking. Different tools for different jobs.
- Decided against timestamps on log entries — date-level granularity is enough for a
  thesis project.
- Discussed whether to have a separate `docs/` folder — decided against it. The
  `journal/topics/` files already serve as flat, scannable reference material. A separate
  docs layer would just be redundant middle ground between the journal and the thesis.
- Renamed the role of `topics/` to be broader — not just subsystem specs but any
  deep-dive: literature reviews, preliminary research, regulatory notes, etc.

**Final structure:**
- `journal/log.md` — chronological working notes (this file)
- `journal/topics/` — deep-dives and reference material by topic
- `journal/decisions/` — design decision records
Updated CLAUDE.md and README.md to reflect the new structure.
