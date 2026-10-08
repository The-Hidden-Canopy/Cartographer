# Series intake closure

The supplied engineering series are source material for bounded implementation
work; they are not themselves an open working directory. On 4 October 2026,
the local Cartographer bundles under `tmp/` were moved to a private,
recoverable archive outside this checkout. The archive location is deliberately
not recorded in the public repository.

Run the read-only guard from the repository root:

```powershell
python scripts/check_series_closure.py --root .
```

`DOCSERIES_CLOSURE_PASS` means that no tracked, untracked, or ignored path
inside the checkout has a docseries bundle name. It does not mean that every
capability described by the series is implemented. Deferred capabilities and
their acceptance boundaries remain explicit in the roadmap and benchmark
ledger; changing those claims requires implementation evidence.
