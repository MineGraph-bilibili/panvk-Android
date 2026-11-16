# csf-v14

Mali-G1 series, Mesa `PAN_ARCH` 14, CSF frontend.

Upstream Mesa already ships the v14 backend and the G1-Ultra,
G1-Premium and G1-Pro model rows. This series is for arch-scoped v14
quirks only; shared kbase, WSI and Android fixes live in the common
series.

No v14-specific patch is required for the first profile. Add patches
here when a G1 device exposes a v14-only issue.
