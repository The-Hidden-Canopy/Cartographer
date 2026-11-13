# First Block example

First Block is an original practice asset: one rectangular block, 2 x 0.5 x 1.5 meters, with eight vertices and six quad faces. Each face declares material region `1`. The project contains no material, textures, or external assets.

## Files

- `first-block.carto`: self-contained project in schema 34.
- `first-block.obj`: original interchange source, including Cartographer face-region comments.
- `manifest.json`: byte counts and SHA-256 hashes for the project and OBJ.
- `generate_example.py`: standard-library-only regeneration script; requires a Cartographer CLI.
- `PROVENANCE.md`: asset origin and format notes.

The current source declares schema 40 and a minimum readable schema of 1. Opening and re-saving this example in the current native application has not been revalidated. The OBJ can also be used to exercise mesh import; its `CARTOGRAPHER_FACE_MATERIAL*` comments are Cartographer metadata that other OBJ readers may ignore. It contains no material library or textures.

## Rebuild without downloads

From this directory, run with your own CLI path and a new or empty output directory:

```powershell
python generate_example.py --cli "C:\path\to\cartographer_cli.exe" rebuilt-first-block
```

The script refuses to overwrite its output files. It imports the OBJ through the CLI, assigns a stable example namespace, validates the result, and writes a manifest. Do not point it at a project you have edited. Regeneration with a different CLI version may change the project format or bytes.
