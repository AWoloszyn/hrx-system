# Pipeline Corpus

These target-neutral programs describe group lanes, temporal record sequences,
and stage tile contracts. External stage declarations keep graph-planning tests
focused on the callable ABI; native execution tests supply complete stage bodies.
The corpus verifies and round-trips through the shared dialects. Target planning
fixtures consume it through `TEMPLATE` and assert their materialized graph.

The XDNA array provider currently materializes resident pipelines. Core-only
source-to-low fixtures exercise individual callables and live in `source_low/`.
