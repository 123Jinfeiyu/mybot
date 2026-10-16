class ControllerError(Exception):
    """Controller failure; previously issued hardware effects are not reversible."""


class ScriptError(ControllerError, ValueError):
    pass


class DriverError(ControllerError):
    pass


class BudgetExceeded(ControllerError):
    pass
