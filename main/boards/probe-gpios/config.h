#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

// Diagnostic probe board — no peripherals defined.
// Audio sample rates required by the build system; values irrelevant since
// GetAudioCodec() returns nullptr.
#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

#endif // _BOARD_CONFIG_H_
