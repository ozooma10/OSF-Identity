# OSF Identity release-page kit

Marketing and Nexus-page assets for OSF Identity. These files are presentation
material only; the installable release archive is produced separately by
`tools/build-release.ps1`.

The direction extends the established OSF NASA-punk mission-patch family:

- shared brushed-steel `OSF` wordmark, near-black starfield, planet limb, arc
  lettering, instrument brackets, and four-point stars;
- Identity-specific **orbital violet** accent;
- a biometric profile emblem: offset appearance contours crossed by a scan
  line, diamond endpoints, and the family ringed focus node.

The visual is intentionally about appearance selection rather than security or
surveillance. Its product line is:

> Drop-in appearances. Original records.

The descriptive subtitle is **Runtime NPC Appearance Framework**.

## Contents

| File | Size | Use |
| --- | ---: | --- |
| `branding/osf-identity-patch.{svg,png}` | 1024x1024 | Nexus main image / thumbnail |
| `branding/osf-identity-patch-emblem.{svg,png}` | 1024x1024 | Wordmark-free patch / avatar |
| `branding/osf-identity-header.{svg,png}` | 1600x520 | Top-of-description banner |
| `branding/osf-identity-emblem.{svg,png}` | 512x512 | Standalone transparent mark |
| `section-headers/svg/*.svg` | 1300x130 | Editable section-divider sources |
| `section-headers/*.png` | 1300x130 | Nexus-ready section dividers |
| `nexus-page.bbcode` | - | Paste-ready page copy |

The SVGs are the editable sources of truth. The PNGs are rendered deliverables.

## Alignment grid

- Standalone emblem: geometric centre `(256, 256)` on the 512px canvas.
- Mission patches: geometric centre `(340, 340)` in the shared OSF patch viewBox.
- Main-patch emblem: centred at `(340, 395)` in the lower identity field.
- Header patch: visible centre `(300, 260)` and 420px diameter, matching the
  OSF UI and OSF Animation headers.
- Header wordmark, rule, and subtitle: optical centre `x=1048`.

## Palette

- Brushed steel: `#d4dae1` to `#828a93`
- Orbital violet: `#bda7ed`, `#9570d1`, `#7654b5`, `#68439f`
- Deep violet: `#41266f`
- Background: `#0b0e12`; planet/panel: `#171525`
- Neutral telemetry: `#aeb6bf`, `#33424d`

## Suggested Nexus metadata

- **Title:** `OSF Identity - Runtime NPC Appearance Framework`
- **Summary:** `Assign Creation Kit or compatible .npc presets to Starfield NPCs through drop-in appearance packs - no patch plugins and no appearance data baked into saves.`
- **Main image:** `branding/osf-identity-patch.png`

Before pasting `nexus-page.bbcode`, upload the header and six section PNGs to
the Nexus gallery, then replace every `{{..._IMAGE_URL}}` token with its
Nexus-hosted image URL. Preview the page once in desktop and narrow layouts.

## Current Nexus event restriction

The Nexus Mods 25th Anniversary Mod Drive rules in effect in August 2026 say
that event submissions may not use generative AI for assets, code, or dialogue.
This release-page kit was AI-assisted, including its vector artwork. Do not use
these assets for an entry in that event unless Nexus explicitly confirms they
are eligible; the safe reading of the current rule is that they are not.

See the [official event rules](https://help.nexusmods.com/article/175-nexus-mods-25th-anniversary-mod-drive-guidelines).

## Re-rendering PNGs

Render the SVG files at their declared pixel dimensions with a transparent
background. The checked-in PNGs were rendered from the SVGs using headless
Microsoft Edge through Playwright so the viewport exactly matches the SVG.
