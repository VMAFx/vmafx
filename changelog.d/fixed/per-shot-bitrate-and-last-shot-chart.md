`vmaf-tune` per-shot report: Bitrate column now shows real kbps values (was "—" for
every shot because the bisect-predicate side-channel was not wired up); per-shot
timeline chart now renders the last shot's CRF band visibly (asymmetric x-axis
right-padding prevents matplotlib clip-box trimming). ADR-0531.
