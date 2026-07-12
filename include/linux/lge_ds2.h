#ifndef _LGE_DS2_H
#define _LGE_DS2_H

#ifdef CONFIG_LGE_DUAL_SCREEN
bool is_ds2_connected(void);
bool is_ds2_dp_hpd_high(void);
bool is_ds2_hallic_connected(void);
void set_hallic_status(bool enable);
#else
static inline bool is_ds2_connected(void) { return false; };
static inline bool is_ds2_dp_hpd_high(void) { return false; };
static inline bool is_ds2_hallic_connected(void) { return false; };
static inline void set_hallic_status(bool enable) { };
#endif

#endif // _LGE_DS2_H
