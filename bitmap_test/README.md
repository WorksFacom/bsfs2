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
- `--stress-ops N` (default 100)
- `--stress-blocks N` (default 2560)
- `--workload OPS:FILE_MIB` (repetível; para `--stress` e `--stat-bench`)
- `--report`
  - usa `--stress-ops` e `--stress-blocks` para definir o cenário do relatório
- `--bitmap-size N` (override direto do bitmap)
- `--disk-size-gib N` (simulação de disco; default 250)
- `--validate`
- `--validate-bitmap-size N` (somente com `--validate`)
- `--tui`
- `--help`
- `--stat-bench` (benchmark estatístico repetido)

Exemplos:

```bash
python run.py rb --demo
python run.py treap --stress
python run.py rb --stress --stress-ops 100 --stress-blocks 2560
python run.py rb --stress --workload 100:10 --workload 10:1024
python run.py --report
python run.py --report --stress-ops 100 --stress-blocks 2560
python run.py --validate
python run.py --validate --validate-bitmap-size 1000000
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
- bitmap fixo (simulação de disco) = **65.536.000 páginas/bits**
  - disco: **250 GiB**
  - página: **4 KiB**
  - mapeamento: **1 bit por página**

Exemplos:

```bash
# Allocation-only com 1000 seeds
python run.py --stat-bench --stat-mode allocation_only --stat-seeds 1000

# Mixed 70/30 com subset de estratégias
python run.py --stat-bench --stat-mode mixed_7030 --stat-seeds 1000 \
  --stat-strategies baseline,avl,heap

# Rodar múltiplos workloads em uma execução (ops:tamanho_arquivo_mib)
python run.py --stat-bench --workload 100:10 --workload 10:1024
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
- `PERFORMANCE_ANALYSIS.md` (diagnóstico e otimizações para bitmap fixo 250 GiB)
