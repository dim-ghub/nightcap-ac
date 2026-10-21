# Vox TTS Documentation

Welcome to the Vox TTS documentation. Here you will find comprehensive guides on the architecture, text-to-speech engines, configuration options, and development workflows.

## Documentation Index

- [Architecture & Internals](architecture.md)
  - Understand how Vox TTS integrates into Nightcap without virtual audio cables or separate companion apps.
  - Details on SDL3 function wrapping, keystroke reconstruction, microphone stream injection, and the Vulkan overlay layer.

- [Speech Engines Guide](engines.md)
  - Detailed overview of the three supported local speech engines:
    - **Piper (Neural AI)**: High-quality ONNX neural voices.
    - **eSpeak-NG**: Lightweight multi-accent retro formant speech.
    - **Animal Crossing (Animalese)**: Authentic villager voice synthesizer with English and Korean sound banks.
  - Instructions on downloading and managing custom voices and letter sound packs.

- [Configuration Reference](configuration.md)
  - Complete reference for `config.json`, including all options, valid ranges, and defaults.
  - Audio routing mechanics, volume control, and microphone mixing.
  - Backward compatibility and automatic migration from `nightcap-ac`.

- [Development & Testing Guide](development.md)
  - Prerequisites and build instructions.
  - Test harness design: how `vk_driver`, `mic_driver`, and Python integration tests simulate Roblox and Nightcap.
  - Environment variables for debugging and headless rendering.
