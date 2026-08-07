# Manifest-less preset slot

This example pack has no `package.json`. The folder name
`author.folder-only-example` is the pack ID and the pack
runs at priority `0`.

Place a producer-round-tripped preset in this owning-plugin directory as
`<localFormId>.npc` — for example `00005983.npc`. This source directory
intentionally contains no active or fabricated preset payload.

Add a `package.json` at the pack root only when the pack needs a non-default
priority, pack-wide `requires`, or explicit assignments.
