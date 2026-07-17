import logging

import h5py
import pandas as pd

logger = logging.getLogger(__name__)


def load_raw_table(
    path: str,
    dataset_path: str,
    chunk_threshold_bytes: int = 500_000_000,
    chunk_rows: int = 5_000_000,
):
    with h5py.File(path, "r") as fh:
        if dataset_path not in fh:
            logger.warning("dataset %r not found in %r", dataset_path, path)
            return None

        dset = fh[dataset_path]
        if dset.nbytes <= chunk_threshold_bytes:
            return pd.DataFrame(dset[:])

        n_rows = dset.shape[0]
        chunks = [
            pd.DataFrame(dset[start : start + chunk_rows])
            for start in range(0, n_rows, chunk_rows)
        ]
        return pd.concat(chunks, ignore_index=True, copy=False)
