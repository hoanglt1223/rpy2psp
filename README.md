# rpy2psp

Convert Ren'Py visual novels into native PSP homebrew. Scripts are compiled on PC into a compact bytecode, images become GPU-ready textures, and a small native engine plays them on a PSP with custom firmware. No Python runs on the PSP.

## Status

Early. Phase 01 (prior art and feature census) is done; the compiler and engine are not written yet.

- Supported subset: dialogue-driven visual novels (labels, menus, jumps, images, transitions, music, simple variables). Games that rely on custom Python code or Python-driven screens are reported as unsupported. Details: [docs/dev/census.md](docs/dev/census.md).
- Prior art: [docs/dev/prior-art.md](docs/dev/prior-art.md).
- Census tool: [tools/census](tools/census).

Roadmap: [anyport plans](https://github.com/hoanglt1223/anyport/tree/main/plans/261023-1558-renpy-to-psp).

## Legal

Tool and engine only. No game scripts or assets are included. Convert only games you have the right to use.

## License

MIT
