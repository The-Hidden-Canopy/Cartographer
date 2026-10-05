# Heterogeneous production closure acceptance

The bounded public slice is admitted when:

- a valid cook key and receipt validate deterministically;
- a receipt with a stale source revision or digest is rejected with
  `stale_data`;
- error, confidence, and resident-memory budgets fail closed;
- failed, stale, and cancelled receipts cannot carry output;
- a newer non-admitted receipt masks an older admitted receipt in the ledger;
- the complete Release suite and this contract's focused test pass.

Run from the repository root:

```powershell
cmake --build build --config Release --target cartographer_production_closure_tests
ctest --test-dir build -C Release --output-on-failure -R cartographer_production_closure_tests
```

This evidence proves the public contract and CPU/reference admission path. It
does not claim D3D12, Vulkan, GPU timing, VANTA runtime admission, remote
workers, or final photoreal acceptance.
