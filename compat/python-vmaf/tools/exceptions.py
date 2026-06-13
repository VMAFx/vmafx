class CalibrationError(Exception):
    """Raised when stats-calculation fails and the caller has not opted in to
    the uncalibrated-normalisation fallback via ``allow_uncalibrated=True``."""


class MissingLabelStddevError(Exception):
    """Raised when ``ys_label_stddev`` is absent from a stats dict and the
    caller has not opted in to the unit-stddev assumption via
    ``assume_unit_stddev=True``."""


class EnsembleNotSupportedError(Exception):
    """Raised when a ``LocalExplainer`` receives a multi-model list and has no
    defined aggregation strategy for ensemble explanations."""


class BdRateException(Exception):
    pass


class BdRateNotEnoughPointsException(BdRateException):
    """Exception raised when there are not enough points in the BD-rate calculation."""

    def __init__(
        self, message: str = "Not enough points for BD-rate calculation; at least 4 points required"
    ):
        super().__init__(message)


class BdRateNoOverlapException(BdRateException):
    """Exception raised when there is no overlap in the BD-rate calculation."""

    def __init__(self, message: str = "No overlap in BD-rate calculation"):
        super().__init__(message)


class BdRateNonMonotonicException(BdRateException):
    """Exception raised for non-monotonic data in the BD-rate calculation."""

    def __init__(self, message: str = "Non-monotonic data found in BD-rate calculation"):
        super().__init__(message)


class BdRateZeroRateException(BdRateException):
    """Exception raised for data with zero rate in the BD-rate calculation."""

    def __init__(self, message: str = "Points with zero rate found in BD-rate calculation"):
        super().__init__(message)
