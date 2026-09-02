`vmaf-tune` reports now embed a versioned `encoder_profile` payload and
ship `vmaf-tune encode-profile` to run one selected report recommendation
through FFmpeg; the FFmpeg patch stack adds advisory `-vmaf-profile` CLI
glue for profile-driven workflows.
