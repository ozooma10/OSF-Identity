# Manifest-less preset slot

This example pack has no `package.json`. The folder name
`author.folder-only-example` becomes the packageId (lowercased) and the pack
runs at priority `0`.

Place a producer-round-tripped preset directly in this directory as
`<EditorID>.npc` — for example `Companion_SarahMorgan.npc`. This source
directory intentionally contains no active or fabricated preset payload.

Add a `package.json` at the pack root only when the pack needs a non-default
priority, pack-wide `requires`, or explicit assignments.
