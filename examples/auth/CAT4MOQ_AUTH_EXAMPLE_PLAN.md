# CAT4MoQ auth example status

The original legacy-token implementation plan is superseded by
[the publisher design](../../docs/cat4moq-design.md) and
[the implementation plan](../../docs/cat4moq-plan.md). Runnable commands and
current options are in [README.md](README.md).

The example now demonstrates structured credentials with explicit C4M-01,
moqx compatibility, and Red5 COSE compatibility profiles. Legacy preencoded
wrappers remain explicit options. Token acquisition uses the shared bounded
decoder; no issuer signing or relay policy enforcement is implemented here.

Its generated objects are opaque deterministic text, with one subgroup per
group and ten paced objects per second. The live interoperability harness
separately verifies catalog and real audio/video payload reception.

Focused regression coverage checks source/profile conflicts, malformed and
oversized credentials, command argument substitution, numeric limits, and
subgroup closure. The old proposed `--expect`, `--subject`, and
`--cat-token-file` options were never the executable's interface; use the
current README and `--help`.
