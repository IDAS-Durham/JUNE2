import pandas as pd


def enrich_with_state_at_time(
    event_df,
    state_change_df,
    id_column: str,
    time_column: str = "time",
    state_column: str = "new_symptom_id",
):
    left = event_df.sort_values(time_column)
    right = (
        state_change_df.rename(columns={"person_id": id_column})
        .sort_values(time_column)[[id_column, time_column, state_column]]
    )

    merged = pd.merge_asof(
        left, right, on=time_column, by=id_column, direction="backward"
    )
    merged.index = left.index
    return merged.sort_index()
