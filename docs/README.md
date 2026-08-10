# Freshwater documentation

`protocol.md` lives at this level because it is the single normative
host ↔ card wire contract — every component README links here rather
than duplicating command tables. Everything else is grouped below.

| Location | Contents |
| -------- | -------- |
| [`protocol.md`](protocol.md) | Framing, addressing, every console command, CDC uploads. If this and the firmware disagree, trust the firmware (`h` on the card console) and fix the document. |
| `reference/` | Engineering references: [`rs485_console_architecture.md`](reference/rs485_console_architecture.md) (bus turnaround, half-duplex discipline, host-tool layout), [`note_filter_butterworth.md`](reference/note_filter_butterworth.md) (Channel per-voice DF4 LPF), [`svn_publishing.md`](reference/svn_publishing.md) (release export flow), plus the Firmware Engineering Reference (.docx) and Synth Firmware Architecture (.pdf). |
| `manuals/` | Channel / Effect card user manuals (.docx). |
| `datasheets/` | Channel / Effect card datasheets (.docx). |
| `diagrams/` | Block diagrams (.drawio), Channel audio-flow diagram, SCF/HP clock-steering SVGs. |
| `img/` | Local board photos — gitignored, not part of the product tree. |

## Binary deliverables (not maintained with the code)

The `.docx` / `.pdf` / `.drawio` files predate the 8-voice SAMPLE
architecture and need a content refresh before they are distributed.
They are kept in git for authoring continuity and are **excluded from
SVN publishing**.
