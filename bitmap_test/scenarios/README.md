# Scenarios (`bitmap_test/scenarios`)

Cenários independentes de carga para benchmark.

## Objetivo

Separar lógica de workload da CLI e dos allocators para facilitar extensão e teste.

## Arquivos

- `base.py`: contrato `BenchmarkScenario` (`run(...)`).
- `stress.py`: cenário padrão de benchmark (mix 70% alloc / 30% dealloc, seed fixa).
- `allocation_only.py`: cenário estatístico de alocação pura.
- `mixed_7030.py`: cenário estatístico seedável com mix 70/30.
- `__init__.py`: exports públicos.

## Interface comum

Cada cenário implementa `run(...)` e retorna um dicionário de métricas compatível com o relatório.

## Quando criar um novo cenário

Crie novo cenário quando quiser avaliar comportamento específico (latência cauda, fragmentação extrema, fill-up, hot/cold etc.) sem alterar `run.py` ou allocators.
