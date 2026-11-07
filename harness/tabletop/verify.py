#!/usr/bin/env python3
"""Independent verifier for cache-aware LLM cost accounting.

Checks behaviour only, never the implementation: it loads the module by path and
compares numbers computed from fixed inputs. It is copied into both arms and is
read-only for the agent.
"""
import importlib.util
import json
import sys
from pathlib import Path

MILLION = 1_000_000


def load_module(root):
    path = Path(root) / 'src/tabletopmagnat/observability/llm_usage.py'
    spec = importlib.util.spec_from_file_location('llm_usage_under_test', path)
    module = importlib.util.module_from_spec(spec)
    sys.modules['llm_usage_under_test'] = module
    spec.loader.exec_module(module)
    return module


class Details:
    def __init__(self, **kwargs):
        for name, value in kwargs.items():
            setattr(self, name, value)


class Usage:
    """Mimics an OpenAI-compatible usage object with nested detail objects."""

    def __init__(self, prompt=0, completion=0, total=None, cached=None, reasoning=None):
        self.prompt_tokens = prompt
        self.completion_tokens = completion
        self.total_tokens = total if total is not None else prompt + completion
        if cached is not None:
            self.prompt_tokens_details = Details(cached_tokens=cached)
        if reasoning is not None:
            self.completion_tokens_details = Details(reasoning_tokens=reasoning)


def close(a, b):
    return a is not None and abs(a - b) < 1e-9


def verify(root):
    failures = []
    module = load_module(root)

    # 1. Nested detail tokens are exposed.
    try:
        got = module.usage_from_response(Usage(prompt=100, completion=20, cached=40, reasoning=8))
        if got.get('cached_input_tokens') != 40:
            failures.append(f"cached_input_tokens: expected 40, got {got.get('cached_input_tokens')!r}")
        if got.get('reasoning_tokens') != 8:
            failures.append(f"reasoning_tokens: expected 8, got {got.get('reasoning_tokens')!r}")
        if got.get('prompt_tokens') != 100 or got.get('completion_tokens') != 20:
            failures.append('existing token fields changed')
    except Exception as error:
        failures.append(f'usage_from_response with details raised {type(error).__name__}: {error}')

    # 2. Absent details default to zero, never raise.
    try:
        got = module.usage_from_response(Usage(prompt=10, completion=5))
        if got.get('cached_input_tokens') != 0 or got.get('reasoning_tokens') != 0:
            failures.append('absent details must default both new counters to 0')
    except Exception as error:
        failures.append(f'usage_from_response without details raised {type(error).__name__}: {error}')

    # 3. Negative values clamp to zero.
    try:
        got = module.usage_from_response(Usage(prompt=10, completion=5, cached=-3, reasoning=-1))
        if got.get('cached_input_tokens') != 0 or got.get('reasoning_tokens') != 0:
            failures.append('negative detail counts must clamp to 0')
    except Exception as error:
        failures.append(f'usage_from_response clamped path raised {type(error).__name__}: {error}')

    # 4. Cached input is billed at its own price when configured:
    #    600k uncached * 1.0 + 400k cached * 0.25 + 1M output * 2.0 = 2.7
    try:
        pricing = module.ModelPricing(input_per_million=1.0, output_per_million=2.0,
                                      cached_input_per_million=0.25)
        usage = {'prompt_tokens': MILLION, 'completion_tokens': MILLION,
                 'cached_input_tokens': 400_000}
        cost = module.calculate_cost(usage, pricing)
        if not close(cost, 2.7):
            failures.append(f'cached price: expected 2.7, got {cost!r}')
    except Exception as error:
        failures.append(f'cached pricing raised {type(error).__name__}: {error}')

    # 5. Without a cached price the result matches the previous behaviour:
    #    1M prompt * 1.0 + 1M output * 2.0 = 3.0
    try:
        pricing = module.ModelPricing(input_per_million=1.0, output_per_million=2.0)
        usage = {'prompt_tokens': MILLION, 'completion_tokens': MILLION,
                 'cached_input_tokens': 400_000}
        cost = module.calculate_cost(usage, pricing)
        if not close(cost, 3.0):
            failures.append(f'no cached price: expected 3.0, got {cost!r}')
    except Exception as error:
        failures.append(f'absent cached price raised {type(error).__name__}: {error}')

    # 6. A usage record without the new key behaves exactly as before.
    try:
        pricing = module.ModelPricing(input_per_million=1.0, output_per_million=2.0)
        cost = module.calculate_cost({'prompt_tokens': 500, 'completion_tokens': 500}, pricing)
        if not close(cost, 0.0015):
            failures.append(f'backward compatibility: expected 0.0015, got {cost!r}')
    except Exception as error:
        failures.append(f'legacy usage raised {type(error).__name__}: {error}')

    # 7. Reasoning tokens are already inside completion_tokens: 100 prompt +
    #    1000 completion at 1.0/2.0 = 0.0021, never 0.0037.
    try:
        pricing = module.ModelPricing(input_per_million=1.0, output_per_million=2.0)
        usage = {'prompt_tokens': 100, 'completion_tokens': 1000, 'reasoning_tokens': 800}
        cost = module.calculate_cost(usage, pricing)
        if not close(cost, 0.0021):
            failures.append(f'reasoning double count: expected 0.0021, got {cost!r}')
    except Exception as error:
        failures.append(f'reasoning path raised {type(error).__name__}: {error}')

    # 8. Cached greater than prompt must not produce a negative charge: the cached
    #    count is clamped to the prompt, so all 100 prompt tokens are cached and
    #    cost 100 * 0.25 / 1M = 0.000025.
    try:
        pricing = module.ModelPricing(input_per_million=1.0, output_per_million=2.0,
                                      cached_input_per_million=0.25)
        usage = {'prompt_tokens': 100, 'completion_tokens': 0, 'cached_input_tokens': 400}
        cost = module.calculate_cost(usage, pricing)
        if not close(cost, 100 * 0.25 / MILLION):
            failures.append(f'oversized cache: expected 0.000025, got {cost!r}')
    except Exception as error:
        failures.append(f'oversized cache raised {type(error).__name__}: {error}')

    # 9. No pricing still yields None, as before.
    try:
        if module.calculate_cost({'prompt_tokens': 1, 'completion_tokens': 1}, None) is not None:
            failures.append('calculate_cost with no pricing must stay None')
    except Exception as error:
        failures.append(f'no-pricing path raised {type(error).__name__}: {error}')

    return {'passed': not failures, 'failures': failures}


if __name__ == '__main__':
    result = verify(sys.argv[1] if len(sys.argv) > 1 else '.')
    print(json.dumps(result))
    raise SystemExit(0 if result['passed'] else 1)