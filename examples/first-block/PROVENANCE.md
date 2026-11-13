# First Block provenance

## Origin

First Block was created specifically for Cartographer's alpha first-use package. The eight vertex positions and six face definitions are original numeric content in `generate_example.py`. No third-party mesh, texture, image, font, material library, or downloaded asset is included or required.

The asset and its documentation are distributed under the Cartographer repository's `LICENSE`; no separate third-party asset license is asserted.

## Construction

- Dimensions: 2 × 0.5 × 1.5 meters.
- Coordinates: right-handed, +Y up; the base rests at Y = 0.
- Topology: 8 vertices, 6 quad faces, 12 triangles when compiled.
- Material regions: one face-domain `condition.material_region` layer; every face has region ID `1`.
- UVs and textures: none.
- Project identity: `cartographer.project.examples.first-block`.

The OBJ is the original source. The `.carto` is generated from that OBJ through Cartographer's supported `import-obj` CLI command, then validated through the CLI. The generator replaces only the generated source-namespace record with the stable example identity before the final validation; it does not hand-author the schema-34 project envelope.

## Reproducibility

`generate_example.py` uses only the Python standard library and a caller-supplied Cartographer CLI. It performs no downloads and embeds no machine-specific source or output path. The two generated asset hashes are recorded in `manifest.json`. Regenerating with the same CLI version and example generator produces the same asset bytes; using another supported CLI version may upgrade the project schema and change those bytes.

The package intentionally omits the temporary `.carto.journal` created during import. It is not needed to open this clean project. Once you save your own copy, keep its new journal beside it for the application's explicit recovery workflow.
