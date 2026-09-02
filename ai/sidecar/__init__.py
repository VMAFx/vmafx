# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
# Copyright 2026 Lusoris
#
# ai/sidecar — vmafx-node online training sidecar package.
#
# ADR-0781: sidecar online training — SGD + EMA + replay buffer.

from .online_trainer import OnlineTrainer, run_server
from .replay_buffer import ReplayBuffer, Sample
from .sgd_ema import SGDEMAConfig, SGDEMATrainer

__all__ = [
    "OnlineTrainer",
    "ReplayBuffer",
    "SGDEMAConfig",
    "SGDEMATrainer",
    "Sample",
    "run_server",
]
