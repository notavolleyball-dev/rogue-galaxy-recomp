# Native-only rendering: function notes

One section per game function that [NATIVE-ONLY.md](NATIVE-ONLY.md) hooks,
written before its hook (rule 3). Sources:
- `disc/SCUS_974.90` through `tools/mipsdis.py`;
- RGDecomp's C at the commit pinned in `config/SCUS_974.90/funcmap.toml`;
- `tools/rgcpu`'s dataflow analysis, for arguments.

Mark what was confirmed in a running game, and when. Everything else is read
from the code.

## Template

### `Name_address` (size bytes; milestone)

- **Arguments:** registers, their types and meanings.
- **Reads:** structures and offsets.
- **Writes:** structures and offsets, excluding the frame's packet stack.
- **Callees:** the ones that matter.
- **Side effects to reproduce:** every write above that something else reads,
  and who reads it.
- **The hook emits:** records, and when.
- **Open:** what only a running game can settle.

## Notes

(None yet.)
