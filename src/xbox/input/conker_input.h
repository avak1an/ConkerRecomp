#ifndef CONKER_HOST_INPUT_H
#define CONKER_HOST_INPUT_H
void conker_input_init(void);
/* Entry guards return 1 only for devices/handles owned by this backend.
 * Other device classes continue through the original translated routine. */
int conker_input_get_devices(void);
int conker_input_get_changes(void);
int conker_input_open(void);
int conker_input_close(void);
int conker_input_get_state(void);
int conker_input_set_state(void);
#endif
