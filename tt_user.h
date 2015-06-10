
/* tt_user.h */


#include "audio.h"

void tt_user_init (struct audio_s *p_audio_config, struct tt_config_s *p);

int tt_user_heard (char *callsign, int ssid, char overlay, char symbol, double latitude, 
		double longitude, char *freq, char *comment, char mic_e, char *dao);

void tt_user_background (void);