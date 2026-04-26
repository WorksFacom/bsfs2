# Runners (`bitmap_test/runners`)

Camada de execução/orquestração da suíte.

## Arquivos

- `benchmark_runner.py`: executa demo e stress para uma estratégia.
- `validator_runner.py`: executa carga determinística e valida consistência entre bitmaps finais.
- `report_runner.py`: roda benchmark de múltiplas estratégias e gera/salva markdown do relatório.
- `statistical_runner.py`: executa benchmark repetido por seeds e gera CSV de resultados por execução.
- `__init__.py`: exports públicos.

Observação: no modo estatístico, falhas de uma estratégia em uma seed são registradas no CSV (`status=error`) sem interromper o lote inteiro.

## Responsabilidades

1. Coordenar cenários.
2. Centralizar fluxo de benchmark/validação/relatório.
3. Manter `run.py` leve (somente interface CLI/TUI).

## Relação com `run.py`

`run.py` usa diretamente os runners:

- `BenchmarkRunner` para `--demo` e `--stress`.
- `ValidatorRunner` para `--validate`.
- `ReportRunner` para `--report`.
- `StatisticalRunner` para `--stat-bench`.
