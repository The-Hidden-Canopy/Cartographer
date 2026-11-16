# glTF import loss warnings

The material decoder reports features it cannot preserve in its `losses`
list. Material-library imports now carry those entries into the application
receipt, and full-scene imports carry them into their scene warning list and
application receipt. The desktop displays all warnings for these bounded
imports. Other desktop actions still limit feedback length and now disclose
when warnings are omitted.

This changes diagnostics, not the set of material features the importer can
represent. For example, clearcoat texture maps may be omitted while clearcoat
factors are retained; the receipt names that loss. Warnings are displayed as
part of the import result and are not a persistent per-material loss record.
The regression imports an actual glTF document with a clearcoat texture and
checks the decoder, material-library receipt, scene warning list and scene
application receipt. Existing lossless imports and undo remain covered by the
same test suite. The desktop build checks the UI code, but an interactive UI
session was not exercised by this test.
