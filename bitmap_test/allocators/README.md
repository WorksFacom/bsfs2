# Allocators (`bitmap_test/allocators`)

Implementações das estratégias de alocação de blocos.

## Contrato esperado

Todos os allocators:

1. Herdam de `core.base_allocator.BaseAllocator`.
2. São registrados com `@register("<strategy-key>", display_name="...")`.
3. Implementam:
   - `allocate_blocks(count) -> list[int]`
   - `deallocate_blocks(indices) -> bool`
   - `count_free_blocks() -> int`

## Estratégias atuais

- `baseline.py` (`baseline`): varredura sequencial.
- `needle_sequential.py` (`needle`): varredura sequencial com agulha (first-free).
- `avl.py` (`avl`): árvore AVL.
- `avl_range.py` (`avl_range`): árvore AVL de intervalos livres.
- `rbtree.py` (`rb`): estrutura de árvore rubro-negra.
- `heap.py` (`heap`): min-heap.
- `btree.py` (`btree`): B-Tree.
- `bplustree.py` (`bplustree`): B+Tree.
- `hybrid_heap_rb.py` (`hybrid`): híbrido heap + RB.
- `segment_tree.py` (`segtree`): segment tree.
- `two_level_index.py` (`twolevel`): índice em dois níveis.
- `treap.py` (`treap`): treap.

## Registro automático

`allocators/__init__.py` importa todos os módulos e monta a lista ordenada de estratégias com `get_strategy_specs()`, usada pelo `run.py`.

## Adicionando um novo allocator

1. Criar arquivo em `allocators/`.
2. Herdar de `BaseAllocator`.
3. Aplicar `@register("nova-chave", display_name="Nome")`.
4. Garantir semântica consistente de retorno (`[]` para falha de alloc; `False` para dealloc inválido).
