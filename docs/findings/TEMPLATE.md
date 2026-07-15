# NN — <short topic>

- **Date:** YYYY-MM-DD
- **Hypothesis under test:** H_ (from 00-hypotheses.md)
- **Setup:** adapter model, COM port, vehicle state (READY / charging / off),
  ELM327 init used (protocol, headers)

## What I did
Commands run and PIDs queried (e.g. `python scanner.py` option 1,
`22 B0 4C` on header 7E5), how the vehicle was driven/charged during the read.

## Raw artifacts
- Decisive frames — paste request + raw response hex (e.g. `7E5 22 B0 4C` →
  `7ED 04 62 B0 4C 00 0F`).
- Reference value if any (dealer tool / dash reading) for calibration.

## Observations
Bullet the concrete facts. Hex in, decoded value out. No interpretation yet.

## Interpretation
What the bytes seem to mean (formula, scale, unit). Note confidence and any
alternative explanations.

## Verdict
- Hypothesis → confirmed / rejected / partial / inconclusive
- Update `00-hypotheses.md` status accordingly.

## Next
The single most informative next test.
