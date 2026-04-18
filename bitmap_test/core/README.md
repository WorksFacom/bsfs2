# Core (`bitmap_test/core`)

Camada base da arquitetura da suíte de benchmark.

## Arquivos

- `base_allocator.py`: classe abstrata `BaseAllocator` com estado comum (`bitmap`, `size`) e helpers de validação/rollback.
- `registry.py`: registro automático de estratégias via decorator `@register`.
- `metrics.py`: agregação centralizada de métricas de stress (latências, throughput, totais).
- `types.py`: tipos compartilhados leves para runners/scenarios.
- `__init__.py`: exports públicos da camada core.

## Responsabilidades

1. Definir contratos comuns.
2. Reduzir duplicação entre allocators.
3. Evitar acoplamento entre execução de benchmark e implementação das estruturas.

## Uso rápido

```python
from core import BaseAllocator, register
```

```python
@register("my_allocator", display_name="My Allocator")
class MyAllocator(BaseAllocator):
    ...
```
