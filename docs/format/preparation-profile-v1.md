# Preparation profile v1

Preparation profiles are bounded, destination-neutral policy records consumed
by the shared preparation operation. They preserve reproducible channel and
lookdev choices without storing an output path, mutable UI selection, native
engine type, credential, cancellation state, or publication authority.

The canonical UTF-8 text grammar is:

```text
CARTOGRAPHER_PREPARATION_PROFILE 1
IDENTITY "<stable profile identity>"
CHANNELS <active-render-uv> <unique-lightmap-uv> <generate-tangents> "<material-region-layer-or-empty>" "<vertex-color-layer-or-empty>"
LOOKDEV <include-lookdev> <require-lookdev-for-material-regions>
END
```

Boolean fields are exactly `0` or `1`. Empty quoted layer names mean that the
corresponding optional layer is disabled. Profile identity is a bounded safe
token; layer names are bounded printable text. Disabling the active render UV
also requires lightmap selection and tangent generation to be disabled.
Required lookdev cannot be enabled while lookdev generation is disabled.

The reader is bounded to 64 KiB, accepts only version 1, validates every
effective setting, and rejects trailing records. The canonical SHA-256 digest
is computed from the exact normalized serialization and is recorded in each
prepared-scene envelope. Loading a profile does not grant a provider or
destination any mutation authority.

The CLI can create and inspect the built-in portable profile:

```text
cartographer_cli write-default-preparation-profile portable.carto-prep
cartographer_cli inspect-preparation-profile portable.carto-prep
cartographer_cli prepare project.carto project.prepared --profile portable.carto-prep scene
```
