#ifndef WAVEFORM_H_
#define WAVEFORM_H_

/**
 * Start the signal generator: Waveform (0x7F0) every 2 ms, one phase chirped
 * 1 to 100 Hz over 20 s as sine, square and sawtooth. Needs zelos_can_init().
 *
 * @return 0.
 */
int waveform_start(void);

#endif /* WAVEFORM_H_ */
