# Controlled `.npc` fixtures

Creation Kit and CharGenMenu exports for this investigation go in separate
producer directories. Do not treat a fixture as golden until the producer can
load it again successfully. Record the producer version, source NPC/FormID,
loaded masters, and exact controlled edit alongside each set.

Required under both `CK/` and `CharGenMenu/`:

- `Baseline.npc` — unchanged source appearance.
- `HeadpartOnly.npc` — exactly one headpart differs from baseline.
- `FacialMorphOnly.npc` — exactly one facial morph/bone slider differs.
- `TintOnly.npc` — exactly one tint or color differs.
- `BodyMorphOnly.npc` — exactly one body morph differs.
- `Sarah.npc` — complete Sarah Morgan vertical-slice appearance.

The repository ships no invented `.npc` bytes. Both six-file sets passed exact
producer reloads and are golden contracts for the standalone decoder.

For each producer, copy `fixture-metadata.template.json` to
`<producer>/metadata.json`, set the exact producer/version and loaded plugin
list, and fill the controlled edit, successful round-trip flag, and SHA-256 for
all six files. Use `producer: "Creation Kit"` under `CK/` and
`producer: "CharGenMenu"` under `CharGenMenu/`.

Validate the complete cross-producer intake gate before claiming both-source
compatibility:

```powershell
python tools/re/npc_appearance_fixture_check.py
```

The check is strict: both producer sets, all twelve non-empty files, exact
metadata properties, successful per-file producer reloads, size bounds, and
matching hashes are required.
