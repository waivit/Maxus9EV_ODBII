# 16 — Cell-voltage spread: what external sources say (research note, not a finding)

- **Date:** 2026-11-17
- **Hypothesis under test:** none. This is desk research to support a possible
  assessment band on the inspection report (`poc6_report` page 2, hub handoff
  H-0135). Nothing here was measured on our car.
- **Why:** the user wants the report to say whether the cell spread is good or
  bad. Design's H-0135 rule is "no good/bad labels unless a criterion has a
  source", so the criterion needs sources first.

## What the sources say

| Source | Criterion | How it was checked | Applies to us? |
| --- | --- | --- | --- |
| Hyundai TSB **24-01-034H** (April 2024), IONIQ Electric 2020–21MY, NHTSA copy `MC-10254336` | Before a BMS/MCU/VCU update: *"Verify Cell Voltage Deviation does not differ by more than 40mV or 0.04V. (Difference between maximum and minimum cell voltage)."* | Text extracted from the primary PDF by us | Gates a software update, not a health verdict. Different maker and pack. |
| Kia TSB, Niro EV 2019–22MY, NHTSA copy `MC-10236658` | The BMS monitors *"voltage deviation between battery cells"* and sets DTC P1AA600. No threshold is given. | Text extracted from the primary PDF by us | Shows that makers use cell deviation as a fault indicator. Gives no number. |
| Generational (UK used-EV battery diagnostics), ~10,000 tests, reported 2026-08-28 | Median **8 mV**. *"Above 50 mV requires investigation"*, *"at 100 mV it indicates a potential underlying fault."* Flagged cars are retested **at rest** after a rebalance. | Secondary: news articles (Charged EVs, Fleet News) quoting the company | Mixed fleet, single company, measurement conditions of the main dataset not stated. |
| Spec databases (EV Database, CarNewsChina) | MAXUS MIFA 9: **CATL, ternary NMC, 90 kWh**, 400 V. Cell count not published. | Secondary | Confirms the chemistry we inferred from cell voltages. No cell-count reference. |

Our own reading (note 11): spread **22 mV** (3.612 V cell 26, 3.590 V cell 81),
parked, READY. That is within Hyundai's 40 mV gate and above Generational's 8 mV
median.

## Interpretation (limits)

- No source is **MAXUS-specific**. Every number above is borrowed from another
  maker or a third-party fleet dataset.
- Spread depends on the **measurement state**. It widens under load, near empty
  or full, and with uneven temperature. A band is only meaningful for a reading
  taken at rest. The ±5 A "at rest" gate proposed for the report is **our own
  choice**, not from a source.
- Spread measures **balance**, not health. Pack capacity is limited by the
  weakest cell, so a wide spread can reduce usable capacity even when the
  BMS-reported SOH is high. The two are complementary, and neither can be
  derived from the other.

## Proposed bands (for Design and the user to approve; not implemented)

| Spread | Wording |
| --- | --- |
| ≤ 40 mV | within the limit one maker uses (Hyundai TSB) |
| 41–50 mV | above Hyundai's limit, below the "investigate" level |
| 51–100 mV | worth investigating (Generational) |
| > 100 mV | may indicate an underlying fault (Generational) |
| \|current\| > 5 A | measured under load, so it cannot be assessed |

## Sources
- https://static.nhtsa.gov/odi/tsbs/2024/MC-10254336-0001.pdf
- https://static.nhtsa.gov/odi/tsbs/2023/MC-10236658-0001.pdf
- https://chargedevs.com/newswire/generational-finds-cell-voltage-imbalance-in-one-in-85-used-evs-tested-in-the-uk/
- https://www.fleetnews.co.uk/news/one-in-85-used-evs-has-a-battery-cell-imbalance
- https://ev-database.org/car/1837/Maxus-MIFA-9
- https://db.carnewschina.com/maxus/maxus-mifa-9/2024/560km-180kw-273900-24710
