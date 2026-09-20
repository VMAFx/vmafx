MAX_ALIGN = 32


def ALIGN_CEIL(x):
    """
    # ((x) + ((x) % MAX_ALIGN ? MAX_ALIGN - (x) % MAX_ALIGN : 0))

    >>> ALIGN_CEIL(3)
    32
    >>> ALIGN_CEIL(32)
    32
    >>> ALIGN_CEIL(33)
    64

    """
    y = MAX_ALIGN - x % MAX_ALIGN if x % MAX_ALIGN != 0 else 0
    return x + y


if __name__ == "__main__":
    import doctest

    doctest.testmod()
