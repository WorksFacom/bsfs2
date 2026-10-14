# Scenarios (`bitmap_test/scenarios`)

Cenários independentes de carga para benchmark.

## Objetivo

Separar lógica de workload da CLI e dos allocators para facilitar extensão e teste.

## Arquivos

- `base.py`: contrato `BenchmarkScenario` (`run(...)`).
- `stress.py`: cenário padrão de benchmark (mix 70% alloc / 30% dealloc, seed fixa).
- `allocation_only.py`: cenário estatístico de alocação pura.
- `pareto.py`: cenário estatístico seedável com mix 80/20 e um tamanho de arquivo.
- `multisize_pareto.py`: mix 80/20 com arquivos de 1 MiB, 10 MiB e 1 GiB.
- `harness.py`: registro dos blocos alocados (`array` + remoção por troca), GC
  pausado durante a execução e amostragem da memória de pico.

Desalocações liberam uma amostra aleatória dos blocos alocados, não um arquivo
inteiro, então o espaço liberado fica espalhado pelo disco.
- `__init__.py`: exports públicos.

## Interface comum

Cada cenário implementa `run(...)` e retorna um dicionário de métricas compatível com o relatório.

## Quando criar um novo cenário

Crie novo cenário quando quiser avaliar comportamento específico (latência cauda, fragmentação extrema, fill-up, hot/cold etc.) sem alterar `run.py` ou allocators.
