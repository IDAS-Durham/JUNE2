from dataclasses import dataclass


@dataclass
class DatasetSummary:
    path: str
    n_rows: int
    dtype: str
    nbytes: int


@dataclass
class FileSummary:
    path: str
    datasets: list[DatasetSummary]
    registries: dict[str, list[str]]
