HANDLERS = {"add": lambda a, b: a + b, "sub": lambda a, b: a - b}


def dispatch(name, a, b):
    handler = HANDLERS.get(name)
    return handler(a, b) if handler else None
