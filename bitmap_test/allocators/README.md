# Allocators (`bitmap_test/allocators`)

Implementações das estratégias de alocação de blocos, organizadas em duas
famílias por **comportamento de alocação**:

- **`scatter/`** — `allocate_blocks(count)` devolve os `count` menores índices
  livres, contíguos ou não. Não tentam manter o pedido em um único extent.
- **`extent/`** — `allocate_blocks(count)` devolve um **único trecho contíguo**
  quando existe, recorrendo a *scatter* apenas como fallback. O espaço livre é
  modelado como intervalos disjuntos para que a contiguidade possa ser buscada
  diretamente.

## Contrato esperado

Todos os allocators:

1. Herdam de `core.base_allocator.BaseAllocator` (os de extent herdam de
   `extent.extent_base.RangeExtentAllocator`, que por sua vez herda de
   `BaseAllocator`).
2. São registrados com `@register("<strategy-key>", display_name="...")`.
3. Implementam:
   - `allocate_blocks(count) -> list[int]`
   - `deallocate_blocks(indices) -> bool`
   - `count_free_blocks() -> int`

## Estratégias atuais

### Scatter (`scatter/`)

- `baseline.py` (`baseline`): varredura sequencial.
- `needle_sequential.py` (`needle`): varredura sequencial com agulha (first-free).
- `avl.py` (`avl`): árvore AVL de blocos reciclados + cursor `next_fresh`.
- `avl_range.py` (`avl_range`): árvore AVL de intervalos livres, mas drena o
  intervalo mais à esquerda bloco a bloco — comportamento *scatter* (bitmap
  idêntico ao `baseline`).
- `rbtree.py` (`rb`): árvore rubro-negra.
- `heap.py` (`heap`): min-heap.
- `btree.py` (`btree`): B-Tree.
- `bplustree.py` (`bplustree`): B+Tree.
- `hybrid_heap_rb.py` (`hybrid`): híbrido heap + RB.
- `segment_tree.py` (`segtree`): segment tree esparsa.
- `two_level_index.py` (`twolevel`): índice em dois níveis.
- `treap.py` (`treap`): treap.

### Extent (`extent/`)

Política comum dos três novos (em `extent_base.py`): **best-fit** de um trecho
contíguo (menor intervalo com tamanho ≥ `count`), com **fallback de menor
endereço** (scatter a partir dos intervalos mais baixos) e **coalescência** de
vizinhos na liberação.

- `extent_base.py`: `RangeExtentAllocator`, a política compartilhada. As
  subclasses implementam apenas as operações da estrutura de dados.
- `avl_range_extent.py` (`avl_range_extent`): AVL de intervalos com `max_gap`.
  Usa fallback de *maior* intervalo (política própria; veja `CONSISTENCY_GROUP`).
- `treap_range_extent.py` (`treap_range_extent`): treap de intervalos com `max_gap`.
- `rb_range_extent.py` (`rb_range_extent`): rubro-negra de intervalos com `max_gap`.
- `bplustree_extent.py` (`bplustree_extent`): B+Tree de intervalos; folhas
  encadeadas, nós internos com `max_size` agregado para best-fit.

## Validação

`runners/validator_runner.py` compara bitmaps **dentro de cada grupo de
consistência** (`BaseAllocator.CONSISTENCY_GROUP`): todos os *scatter* devem
coincidir com o `baseline`; os três extent com fallback de menor endereço devem
coincidir entre si; o `avl_range_extent` forma seu próprio grupo.

## Registro automático

`allocators/__init__.py` importa todos os módulos das duas subpastas e monta a
lista ordenada de estratégias com `get_strategy_specs()`, usada pelo `run.py`.

## Adicionando um novo allocator

1. Criar arquivo em `allocators/scatter/` ou `allocators/extent/`.
2. Herdar de `BaseAllocator` (ou de `RangeExtentAllocator` para extent).
3. Aplicar `@register("nova-chave", display_name="Nome")`.
4. Importar a classe em `allocators/__init__.py` e adicionar a chave em
   `preferred_order`.
5. Garantir semântica consistente de retorno (`[]` para falha de alloc; `False`
   para dealloc inválido).
