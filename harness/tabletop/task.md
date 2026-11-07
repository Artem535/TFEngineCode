# Task: cache-aware LLM cost accounting

`src/tabletopmagnat/observability/llm_usage.py` prices an LLM completion from
token counts, but it ignores two things OpenAI-compatible providers report inside
the usage object:

- `usage.prompt_tokens_details.cached_tokens` — prompt tokens served from cache,
  which are billed at a lower rate;
- `usage.completion_tokens_details.reasoning_tokens` — reasoning tokens, which are
  already included in `completion_tokens`.

Because of this the recorded cost is wrong for any provider that reports cached
prompt tokens, and the reasoning share is invisible.

Implement the following, keeping existing behaviour intact:

1. `usage_from_response` returns two more keys besides the existing three:
   `cached_input_tokens` from `prompt_tokens_details.cached_tokens` and
   `reasoning_tokens` from `completion_tokens_details.reasoning_tokens`. Both
   default to `0` when the detail object or the field is absent, and negative or
   unparsable values become `0`.
2. `ModelPricing` gains an optional `cached_input_per_million: float | None`
   with a default of `None` and the same non-negative constraint as the other
   prices.
3. `calculate_cost` bills cached prompt tokens at `cached_input_per_million` when
   it is set, and at `input_per_million` when it is `None` so existing callers are
   unaffected. Uncached prompt tokens are `prompt_tokens - cached_input_tokens`,
   clamped at zero, and a cached count larger than `prompt_tokens` must never
   produce a negative charge. Reasoning tokens are already part of
   `completion_tokens` and must not be charged a second time.
4. A usage record that carries none of the new keys must produce exactly the same
   cost as before, and `calculate_cost` still returns `None` when no pricing is
   configured.

Verify your change with the project's own test suite and with `verify.py`, both
described in the environment notes.