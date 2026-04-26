# Bitmap Allocators Benchmark Suite

Suíte de benchmark para comparar estratégias de alocação de blocos em bitmap com arquitetura modular (`core`, `allocators`, `scenarios`, `runners`).

## Quick start

```bash
# TUI interativa
python run.py

# Executar uma estratégia específica
python run.py bplustree --stress

# Executar todas (demo)
python run.py all --demo

# Gerar relatório markdown
python run.py --report

# Validar consistência entre estratégias
python run.py --validate
```

## Estrutura atual

```text
bitmap_test/
├── run.py          # CLI/TUI e orquestração
├── core/           # Contratos base, registry e métricas
├── allocators/     # Implementações de alocadores
├── scenarios/      # Cenários de carga (stress/fragmentação/burst)
└── runners/        # Execução de benchmark, validação e relatório
```

## CLI suportada

`run.py` mantém os comandos:
- `--demo`
- `--stress`
- `--report`
- `--validate`
- `--tui`
- `--help`
- `--stat-bench` (benchmark estatístico repetido)

Exemplos:

```bash
python run.py rb --demo
python run.py treap --stress
python run.py --report
python run.py --validate
```

## Benchmark estatístico (robusto)

Dois modos separados:

1. `allocation_only` (apenas alocação)
2. `mixed_7030` (70% alocação / 30% desalocação)

Cenário médio padrão:
- 100 arquivos × 10 MiB
- bloco = 4 KiB
- 2560 blocos por arquivo
- 256.000 blocos alvo
- bitmap padrão = **2x** (512.000 blocos)

Exemplos:

```bash
# Allocation-only com 1000 seeds
python run.py --stat-bench --stat-mode allocation_only --stat-seeds 1000

# Mixed 70/30 com subset de estratégias
python run.py --stat-bench --stat-mode mixed_7030 --stat-seeds 1000 \
  --stat-strategies baseline,avl,heap
```

Artefatos gerados:
- `benchmark_runs_<mode>_<YYYYMMDD-HHMMSS>.csv` (resultado por seed/estratégia)

## Estratégias disponíveis

As estratégias são registradas automaticamente por decorator (`@register`) em `allocators/` e expostas no CLI:

`baseline`, `avl`, `rb`, `heap`, `btree`, `bplustree`, `hybrid`, `segtree`, `twolevel`, `treap`.

## Como estender

1. Novo allocator: implementar classe em `allocators/` herdando `BaseAllocator` e usar `@register("chave")`.
2. Novo cenário: criar classe em `scenarios/` implementando `BenchmarkScenario`.
3. Nova saída/fluxo: evoluir runners em `runners/`.

## Documentação por pasta

- `core/README.md`
- `allocators/README.md`
- `scenarios/README.md`
- `runners/README.md`
