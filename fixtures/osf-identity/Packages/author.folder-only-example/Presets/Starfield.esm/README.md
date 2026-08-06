# Manifest-less preset slot

This example package has no `package.json`. The folder name
`author.folder-only-example` becomes the packageId (lowercased) and the package
runs at priority `0`.

Place a producer-round-tripped preset here as `<LocalFormID>.npc` — for example
`0029A8EB.npc` to target local FormID `0029A8EB` from `Starfield.esm`. Do not
use a load-order-prefixed FormID or an EditorID as the filename. This source
directory intentionally contains no active or fabricated preset payload.

Add a `package.json` at the package root only when the package needs a
non-default priority, package-wide `requires`, or explicit assignments.
