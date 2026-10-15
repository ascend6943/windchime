# Windchime 1.0.0

The first public release of Windchime, a native Linux tray app that turns typing into gentle chimes.

- Adjustable letter density and playback spacing.
- Warm, Airy, and Bright synthesized chimes, full volume control, and overlapping decays.
- Tray controls, login startup, and a configurable pause shortcut.
- A separate keyboard reader that emits note numbers, with pause, session-lock, heartbeat, and disconnect safeguards.
- An installer, uninstaller, source build, automated checks, and a demo sound.

The review before release fixed queued previews continuing after a lock-status poll, bounded keyboard processing so input cannot monopolize the helper loop, recovery after logind-bus failure, duplicate startup, audio-stream connection timeout, and installer repair. The source build now discovers system development headers and keeps generated files under `build/`.

The binary archive targets **Ubuntu 25.10 x86-64 / KDE Plasma 6.4.5 / Wayland / PipeWire**, using system Qt 6.9 libraries. Other environments should build from source and are not yet validated. Verify the archive against `SHA256SUMS` before installation.

No typed text is assembled or stored. Password-field recognition is not supported; pause before sensitive typing. Pasted text, dictation, and IME-composed input are outside this version's physical-keyboard model.
