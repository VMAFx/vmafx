# kimchi.py
# For converting Python 2 pickles to Python 3
# taken from https://rebeccabilbro.github.io/convert-py2-pickles-to-py3/

import argparse
import pickle
from pathlib import Path

import dill

from vmaf.tools.safe_pickle import load_pickle


def convert(old_pkl):
    """
    Convert a Python 2 pickle to Python 3
    """
    # Make a name for the new pickle
    new_pkl = Path(old_pkl).stem + "_p3.pkl"

    # Convert Python 2 "ObjectType" to Python 3 object
    dill._dill._reverse_typemap["ObjectType"] = object

    # Open the pickle using latin1 encoding
    with Path(old_pkl).open("rb") as f:
        loaded = load_pickle(f, encoding="latin1")

    # Re-save as Python 3 pickle
    with Path(new_pkl).open("wb") as outfile:
        pickle.dump(loaded, outfile)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description="Convert a Python 2 pickle to Python 3")

    parser.add_argument("infile", help="Python 2 pickle filename")

    args = parser.parse_args()

    convert(args.infile)
