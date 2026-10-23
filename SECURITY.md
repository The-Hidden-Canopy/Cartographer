# Security policy

Cartographer 0.1 is a local, headless C++ foundation. Report suspected parser,
path traversal, project persistence, or data-corruption issues privately to the
repository owner before public disclosure. Include a minimal reproducer and
state whether the issue affects authoring truth, derived output, or only a
diagnostic path.

The OBJ importer and project loader are untrusted-input boundaries. They must
reject invalid indices, non-finite values, unsupported future versions, and
external path traversal rather than silently repairing input.

