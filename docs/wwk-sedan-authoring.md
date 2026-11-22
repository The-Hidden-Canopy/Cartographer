# WWK Favorite Sedan authoring note

Date: 2026-11-22. Status: adopted source of truth.

## Decision

The WWK Favorite Sedan — the anchor vehicle of WWK's multi-year
damage/repair arc (`vehicle:favorite-sedan`, variants clean / hood-scratched /
hood-panel) — is authored in this repository at
`projects/wwk/favorite-sedan/favorite-sedan.carto` (project schema 34,
namespace `cartographer.project.wwk.favorite-sedan`). Until now the same
geometry was hand-spliced as axis-aligned box blockout inside WWK's
`scripts/build-south-sofia-cartographer-assets.mjs` (`sedanFavoriteMesh`),
and no Cartographer-side WWK project existed.

Rationale:

- **One source of truth.** The sedan's damage states are geometry, not
  runtime data. Authoring them in a Cartographer project makes the states
  reviewable, diffable, and validatable, instead of living as string-splice
  tables in a build script.
- **Schema 34, matching the committed `examples/first-block` authoring
  source.** It is the newest schema for which a hand-authored,
  CLI-validated example is committed, and it carries everything WWK needs:
  a stable `SOURCE_NAMESPACE`, named scene objects with parent links and
  authored visibility, and per-face `condition.material_region` attribute
  layers for per-region material binding. Current Cartographer saves use
  schema 40; the v34 file remains a supported read and migrates on load.
- **Variant model = WWK's runtime model.** Each damage state is a variant
  root object whose `visible` flag is the authored state (clean visible,
  hood-scratched and hood-panel hidden), exactly how WWK's runtime flips
  objects 51/52/53 today. No new runtime mechanism is introduced.
- **Faithful geometry.** The per-variant box inventory reproduces WWK's
  hand-spliced blockout exactly (validator-checked at 5-decimal precision).
  The only structural change is splitting the lower body shell box into
  three z-contiguous boxes so the door line is an authored `doors` panel.

## What lives where

- Cartographer (`projects/wwk/favorite-sedan/`): the authored source, this
  note's project README with the WWK binding table, the export recipe
  (`EXPORT.md`), and `validate_favorite_sedan.py`, which proves the source
  parses and the variant bindings are complete.
- WWK: keeps consuming the sedan through its existing master project objects
  51/52/53 and mesh ids 19/20/21 (asset ids 1025/1026/1027, material slugs
  `sedan-favorite-*`). WWK implements the exporter described in
  `projects/wwk/favorite-sedan/EXPORT.md` to regenerate
  `wwk_favorite_sedan.gltf` from this source and then deletes its
  hand-spliced `sedanFavoriteMesh` blockout.

## Invariants

- WWK binds by namespace + variant root name, never by project-local numeric
  object or mesh id.
- Damage variants change only `hood`, `front-clip`, and (for hood-scratched)
  `wheels`; `body` and `doors` are identical across all variants and are
  asserted byte-identical by the validator.
- Every face carries a material region (1 body paint, 2 hood, 3 glass,
  4 trim, 5 wheels); the default binding maps every region of a variant to
  that variant's `sedan-favorite-*` material, preserving the current
  one-material-per-variant look.
- Any edit to the source must keep `python validate_favorite_sedan.py`
  green; the script fails closed.
