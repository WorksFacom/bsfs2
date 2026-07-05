# Bitmap Allocators Benchmark Suite — C port

Port em C do framework Python `km/bitmap_test/`: uma suíte de benchmark que
compara estratégias de alocação de blocos sobre um bitmap (modelo: disco
250 GiB / página 4 KiB → 65.536.000 blocos, 1 bit por página).

A implementação segue fielmente a lógica do Python (`core`, `allocators`,
`scenarios`, `runners`). As divergências intencionais estão listadas no fim.

## Build

```bash
make            # gera bin/bitmap_test (gcc, -Wall -Wextra -Werror)
make clean
```

## Uso

```bash
./bin/bitmap_test --help

# Demo / stress de uma estratégia
./bin/bitmap_test baseline --demo
./bin/bitmap_test rb --stress --stress-ops 100 --stress-blocks 2560

# Todas as estratégias
./bin/bitmap_test all --demo

# Relatório markdown (report-<timestamp>.md)
./bin/bitmap_test --report

# Validador de consistência (bitmaps idênticos por grupo)
./bin/bitmap_test --validate
./bin/bitmap_test --validate --validate-bitmap-size 1000000

# Benchmark estatístico (CSV)
./bin/bitmap_test --stat-bench --stat-mode pareto --stat-seeds 1000
./bin/bitmap_test --stat-bench --stat-mode multisize_pareto --stat-seeds 100 \
  --stat-strategies baseline,avl,heap
```

O bitmap padrão (250 GiB) usa ~65 MB por alocador. Para experimentos rápidos use
`--bitmap-size N` ou `--validate-bitmap-size N`.

## Estrutura

```
src/
├── core/      bitmap, base (contrato/vtable), layout, rng, metrics,
│              stats_config, stats (seeds), io_csv, registry
├── ds/        longmap (hash long→long, usado por segtree e hybrid)
├── allocators/
│   ├── scatter/  baseline, needle, avl, avl_range, rb, heap, btree,
│   │             bplustree, hybrid, segtree, twolevel, treap
│   └── extent/   extent_treap (política lowaddr) + avl_range_extent
├── scenarios/ stress, pareto, allocation_only, multisize_pareto
├── runners/   demo, stress, validator, report, statistical (CSV)
└── main.c     CLI
```

Cada alocador embute `allocator base;` como primeiro membro e preenche a vtable
(`allocate`/`deallocate`/`count_free`/`aux_mem_bytes`/`destroy`).

## Grupos de consistência (validador)

- `scatter`: todas as 12 estratégias scatter produzem bitmap idêntico ao
  `baseline` (sempre os N menores índices livres).
- `extent_bestfit_lowaddr`: `treap_range_extent`, `rb_range_extent`,
  `bplustree_extent` — best-fit + fallback de menor endereço.
- `extent_bestfit_largest`: `avl_range_extent` (fallback de maior intervalo).

## Divergências intencionais em relação ao Python

1. **RNG.** Em vez do Mersenne Twister do CPython, usa-se um PRNG determinístico
   próprio (SplitMix64) preservando a *estrutura* das cargas (thresholds
   0.7/0.8/0.95, amostragem sem reposição, ordem das chamadas). Números
   absolutos e os CSVs diferem do Python, mas, para uma mesma seed, todas as
   estratégias veem a mesma carga, mantendo válida a comparação entre elas.
   Consequência: o `peak_memory_kib` e os tempos não são comparáveis ao Python.

2. **peak_memory_kib.** O Python usa `sys.getsizeof` (específico de objetos
   Python). Aqui calcula-se a memória auxiliar real em C (nº de nós × `sizeof`).

3. **hybrid (sift_down).** O `_sift_down` do `hybrid_heap_rb.py` tem um bug
   latente (compara contra um valor obsoleto do heap em vez da chave que
   desce), que pode quebrar a propriedade do heap e, sob certas cargas, retornar
   um mínimo incorreto. O port implementa o min-heap **correto** que o autor
   pretendia, para que `hybrid` permaneça consistente com o `baseline` em
   qualquer RNG. Ver o comentário em `src/allocators/scatter/hybrid.c`.

4. **Extents lowaddr.** Os três alocadores `*_range_extent`/`*_extent` de menor
   endereço são definidos para produzir bitmaps idênticos (mesma política; só
   muda a estrutura interna: treap/RB/B+tree). Para manter o port tratável, os
   três compartilham uma única implementação fiel de *treap de ranges com
   max_gap* (`extent_treap.c`). Isso não altera o bitmap nem qualquer métrica
   além de `peak_memory`/tempo. O `avl_range_extent` (grupo próprio) é portado
   separadamente.
