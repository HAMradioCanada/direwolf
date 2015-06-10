//
//    This file is part of Dire Wolf, an amateur radio packet TNC.
//
//    Copyright (C) 2011, 2012, 2013, 2014, 2015  John Langner, WB2OSZ
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU General Public License as published by
//    the Free Software Foundation, either version 2 of the License, or
//    (at your option) any later version.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU General Public License for more details.
//
//    You should have received a copy of the GNU General Public License
//    along with this program.  If not, see <http://www.gnu.org/licenses/>.
//


/*------------------------------------------------------------------
 *
 * Module:      direwolf.c
 *
 * Purpose:   	Main program for "Dire Wolf" which includes:
 *			
 *			AFSK modem using the "sound card."
 *			AX.25 encoder/decoder.
 *			APRS data encoder / decoder.
 *			APRS digipeater.
 *			KISS TNC emulator.
 *			APRStt (touch tone input) gateway
 *			Internet Gateway (IGate)
 *		
 *
 *---------------------------------------------------------------*/

#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <getopt.h>
#include <assert.h>
#include <string.h>
#include <signal.h>
#include <ctype.h>

#if __ARM__
//#include <asm/hwcap.h>
//#include <sys/auxv.h>		// Doesn't seem to be there.
				// We have libc 2.13.  Looks like we might need 2.17 & gcc 4.8
#endif

#if __WIN32__
#else
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#ifdef __OpenBSD__
#include <soundcard.h>
#else
#include <sys/soundcard.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#endif


#define DIREWOLF_C 1

#include "direwolf.h"
#include "version.h"
#include "audio.h"
#include "config.h"
#include "multi_modem.h"
#include "demod.h"
#include "hdlc_rec.h"
#include "hdlc_rec2.h"
#include "ax25_pad.h"
#include "decode_aprs.h"
#include "textcolor.h"
#include "server.h"
#include "kiss.h"
#include "kissnet.h"
#include "kiss_frame.h"
#include "nmea.h"
#include "gen_tone.h"
#include "digipeater.h"
#include "tq.h"
#include "xmit.h"
#include "ptt.h"
#include "beacon.h"
#include "redecode.h"
#include "dtmf.h"
#include "aprs_tt.h"
#include "tt_user.h"
#include "igate.h"
#include "symbols.h"
#include "dwgps.h"
#include "log.h"
#include "recv.h"


//static int idx_decoded = 0;

#if __WIN32__
static BOOL cleanup_win (int);
#else
static void cleanup_linux (int);
#endif

static void usage (char **argv);

#if __SSE__

static void __cpuid(int cpuinfo[4], int infotype){
    __asm__ __volatile__ (
        "cpuid":
        "=a" (cpuinfo[0]),
        "=b" (cpuinfo[1]),
        "=c" (cpuinfo[2]),
        "=d" (cpuinfo[3]) :
        "a" (infotype)
    );
}

#endif


/*-------------------------------------------------------------------
 *
 * Name:        main
 *
 * Purpose:     Main program for packet radio virtual TNC.
 *
 * Inputs:	Command line arguments.
 *		See usage message for details.
 *
 * Outputs:	Decoded information is written to stdout.
 *
 *		A socket and pseudo terminal are created for 
 *		for communication with other applications.
 *
 *--------------------------------------------------------------------*/

static struct audio_s audio_config;
static struct tt_config_s tt_config;
struct digi_config_s digi_config;


static int d_u_opt = 0;			/* "-d u" command line option to print UTF-8 also in hexadecimal. */
static int d_p_opt = 0;			/* "-d p" option for dumping packets over radio. */				

static int q_h_opt = 0;			/* "-q h" Quiet, suppress the "heard" line with audio level. */
static int q_d_opt = 0;			/* "-q d" Quiet, suppress the decoding of APRS packets. */


static struct misc_config_s misc_config;


int main (int argc, char *argv[])
{
	int err;
	int eof;
	int j;
	char config_file[100];
	int xmit_calibrate_option = 0;
	int enable_pseudo_terminal = 0;
	struct digi_config_s digi_config;
	struct igate_config_s igate_config;
	int r_opt = 0, n_opt = 0, b_opt = 0, B_opt = 0, D_opt = 0;	/* Command line options. */
	char P_opt[16];
	char l_opt[80];
	char input_file[80];
	
	int t_opt = 1;		/* Text color option. */				
	int d_k_opt = 0;	/* "-d k" option for serial port KISS.  Can be repeated for more detail. */					
	int d_n_opt = 0;	/* "-d n" option for Network KISS.  Can be repeated for more detail. */	
	int d_t_opt = 0;	/* "-d t" option for Tracker.  Can be repeated for more detail. */	
	int d_o_opt = 0;	/* "-d o" option for output control such as PTT and DCD. */	
			
	

	strcpy(l_opt, "");
	strcpy(P_opt, "");

#if __WIN32__

// Select UTF-8 code page for console output.
// http://msdn.microsoft.com/en-us/library/windows/desktop/ms686036(v=vs.85).aspx
// This is the default I see for windows terminal:  
// >chcp
// Active code page: 437

	//Restore on exit? oldcp = GetConsoleOutputCP();
	SetConsoleOutputCP(CP_UTF8);

#elif __CYGWIN__

/*
 * Without this, the ISO Latin 1 characters are displayed as gray boxes.
 */
	//setenv ("LANG", "C.ISO-8859-1", 1);
#else

/*
 * Default on Raspian & Ubuntu Linux is fine.  Don't know about others.
 *
 * Should we look at LANG environment variable and issue a warning
 * if it doesn't look something like  en_US.UTF-8 ?
 */

#endif

/*
 * Pre-scan the command line options for the text color option.
 * We need to set this before any text output.
 */

	t_opt = 1;		/* 1 = normal, 0 = no text colors. */
	for (j=1; j<argc-1; j++) {
	  if (strcmp(argv[j], "-t") == 0) {
	    t_opt = atoi (argv[j+1]);
	    //dw_printf ("DEBUG: text color option = %d.\n", t_opt);
	  }
	}

	// TODO: control development/beta/release by versio.h instead of changing here.

	text_color_init(t_opt);
	text_color_set(DW_COLOR_INFO);
	//dw_printf ("Dire Wolf version %d.%d (%s) Beta Test\n", MAJOR_VERSION, MINOR_VERSION, __DATE__);
	//dw_printf ("Dire Wolf DEVELOPMENT version %d.%d %s (%s)\n", MAJOR_VERSION, MINOR_VERSION, "F", __DATE__);
	dw_printf ("Dire Wolf version %d.%d\n", MAJOR_VERSION, MINOR_VERSION);


#if __WIN32__
	SetConsoleCtrlHandler (cleanup_win, TRUE);
#else
	setlinebuf (stdout);
	signal (SIGINT, cleanup_linux);
#endif


/* 
 * Starting with version 0.9, the prebuilt Windows version 
 * requires a minimum of a Pentium 3 or equivalent so we can
 * use the SSE instructions.
 * Try to warn anyone using a CPU from the previous
 * century rather than just dying for no apparent reason.
 *
 * Now, where can I find a Pentium 2 or earlier to test this?
 */

#if __SSE__
	int cpuinfo[4];
	__cpuid (cpuinfo, 0);
	if (cpuinfo[0] >= 1) {
	  __cpuid (cpuinfo, 1);
	  //dw_printf ("debug: cpuinfo = %x, %x, %x, %x\n", cpuinfo[0], cpuinfo[1], cpuinfo[2], cpuinfo[3]);
	  if ( ! ( cpuinfo[3] & (1 << 25))) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("------------------------------------------------------------------\n");
	    dw_printf ("This version requires a minimum of a Pentium 3 or equivalent.\n");
	    dw_printf ("If you are seeing this message, you are probably using a computer\n");
	    dw_printf ("from the previous century.  See comments in Makefile.win for\n");
	    dw_printf ("information on how you can recompile it for use with your antique.\n");
	    dw_printf ("------------------------------------------------------------------\n");
	  }
	}
	text_color_set(DW_COLOR_INFO);
#endif

/*
 * This has not been very well tested in 64 bit mode.
 */

#if 0
	if (sizeof(int) != 4 || sizeof(long) != 4 || sizeof(char *) != 4) {
	    text_color_set(DW_COLOR_ERROR);
	    dw_printf ("------------------------------------------------------------------\n");
	    dw_printf ("This might not work properly when compiled for a 64 bit target.\n");
	    dw_printf ("It is recommended that you rebuild it with gcc -m32 option.\n");
	    dw_printf ("------------------------------------------------------------------\n");
	}
#endif

/*
 * Default location of configuration file is current directory.
 * Can be overridden by -c command line option.
 * TODO:  Automatically search other places.
 */
	
	strcpy (config_file, "direwolf.conf");

/*
 * Look at command line options.
 * So far, the only one is the configuration file location.
 */

	strcpy (input_file, "");
	while (1) {
          int this_option_optind = optind ? optind : 1;
          int option_index = 0;
	  int c;
	  char *p;
          static struct option long_options[] = {
            {"future1", 1, 0, 0},
            {"future2", 0, 0, 0},
            {"future3", 1, 0, 'c'},
            {0, 0, 0, 0}
          };

	  /* ':' following option character means arg is required. */

          c = getopt_long(argc, argv, "P:B:D:c:pxr:b:n:d:q:t:Ul:",
                        long_options, &option_index);
          if (c == -1)
            break;

          switch (c) {

          case 0:				/* possible future use */
	    text_color_set(DW_COLOR_DEBUG);
            dw_printf("option %s", long_options[option_index].name);
            if (optarg) {
                dw_printf(" with arg %s", optarg);
            }
            dw_printf("\n");
            break;


          case 'c':				/* -c for configuration file name */

	    strcpy (config_file, optarg);
            break;

#if __WIN32__
#else
          case 'p':				/* -p enable pseudo terminal */
		
	    /* We want this to be off by default because it hangs */
	    /* eventually when nothing is reading from other side. */

	    enable_pseudo_terminal = 1;
            break;
#endif

          case 'B':				/* -B baud rate and modem properties. */
	 
	    B_opt = atoi(optarg);
            if (B_opt < 100 || B_opt > 10000) {
	      text_color_set(DW_COLOR_ERROR);
              dw_printf ("Use a more reasonable data baud rate in range of 100 - 10000.\n");
              exit (EXIT_FAILURE);
            }
            break;

	  case 'P':				/* -P for modem profile. */

	    //debug: dw_printf ("Demodulator profile set to \"%s\"\n", optarg);
	    strcpy (P_opt, optarg); 
	    break;	

          case 'D':				/* -D decrease AFSK demodulator sample rate */
	 
	    D_opt = atoi(optarg);
            if (D_opt < 1 || D_opt > 8) {
	      text_color_set(DW_COLOR_ERROR);
              dw_printf ("Crazy value for -D. \n");
              exit (EXIT_FAILURE);
            }
            break;

          case 'x':				/* -x for transmit calibration tones. */

	    xmit_calibrate_option = 1;
            break;

          case 'r':				/* -r audio samples/sec.  e.g. 44100 */
	 
	    r_opt = atoi(optarg);
	    if (r_opt < MIN_SAMPLES_PER_SEC || r_opt > MAX_SAMPLES_PER_SEC) 
	    {
	      text_color_set(DW_COLOR_ERROR);
              dw_printf("-r option, audio samples/sec, is out of range.\n");
	      r_opt = 0;
   	    }
            break;

          case 'n':				/* -n number of audio channels for first audio device.  1 or 2. */
	 
	    n_opt = atoi(optarg);
	    if (n_opt < 1 || n_opt > 2) 
	    {
	      text_color_set(DW_COLOR_ERROR);
              dw_printf("-n option, number of audio channels, is out of range.\n");
	      n_opt = 0;
   	    }
            break;

          case 'b':				/* -b bits per sample.  8 or 16. */
	 
	    b_opt = atoi(optarg);
	    if (b_opt != 8 && b_opt != 16) 
	    {
	      text_color_set(DW_COLOR_ERROR);
              dw_printf("-b option, bits per sample, must be 8 or 16.\n");
	      b_opt = 0;
   	    }
            break;

          case '?':

            /* Unknown option message was already printed. */
            usage (argv);
            break;

	  case 'd':				/* Set debug option. */
	
	    /* New in 1.1.  Can combine multiple such as "-d pkk" */

	    for (p=optarg; *p!='\0'; p++) {
	     switch (*p) {
	
	      case 'a':  server_set_debug(1); break;

	      case 'k':  d_k_opt++; kiss_serial_set_debug (d_k_opt); break;
	      case 'n':  d_n_opt++; kiss_net_set_debug (d_n_opt); break;

	      case 'u':  d_u_opt = 1; break;

		// separate out gps & waypoints.

	      case 't':  d_t_opt++; beacon_tracker_set_debug (d_t_opt); break;

	      case 'w':	 nmea_set_debug (1); break;		// not documented yet.
	      case 'p':  d_p_opt = 1; break;			// TODO: packet dump for xmit side.
	      case 'o':  d_o_opt++; ptt_set_debug(d_o_opt); break;	
#if AX25MEMDEBUG
	      case 'm':  ax25memdebug_set(); break;		// Track down memory leak.  Not documented.		
#endif
	      default: break;
	     }
	    }
	    break;
	      
	  case 'q':				/* Set quiet option. */
	
	    /* New in 1.2.  Quiet option to suppress some types of printing. */
	    /* Can combine multiple such as "-q hd" */

	    for (p=optarg; *p!='\0'; p++) {
	     switch (*p) {
	      case 'h':  q_h_opt = 1; break;
	      case 'd':  q_d_opt = 1; break;
	      default: break;
	     }
	    }
	    break;
	      
	  case 't':				/* Was handled earlier. */
	    break;


	  case 'U':				/* Print UTF-8 test and exit. */

	    dw_printf ("\n  UTF-8 test string: ma%c%cana %c%c F%c%c%c%ce\n\n", 
			0xc3, 0xb1,
			0xc2, 0xb0,
			0xc3, 0xbc, 0xc3, 0x9f);

	    exit (0);
	    break;

          case 'l':				/* -l for log file directory name */

	    strncpy (l_opt, optarg, sizeof(l_opt)-1);
            break;

          default:

            /* Should not be here. */
	    text_color_set(DW_COLOR_DEBUG);
            dw_printf("?? getopt returned character code 0%o ??\n", c);
            usage (argv);
          }
	}  /* end while(1) for options */

	if (optind < argc) 
	{

          if (optind < argc - 1) 
	  {
	    text_color_set(DW_COLOR_ERROR);
            dw_printf ("Warning: File(s) beyond the first are ignored.\n");
          }

	  strcpy (input_file, argv[optind]);

	}

/*
 * Get all types of configuration settings from configuration file.
 *
 * Possibly override some by command line options.
 */

	symbols_init ();

	config_init (config_file, &audio_config, &digi_config, &tt_config, &igate_config, &misc_config);

	if (r_opt != 0) {
	  audio_config.adev[0].samples_per_sec = r_opt;
	}
	if (n_opt != 0) {
	  audio_config.adev[0].num_channels = n_opt;
	  if (n_opt == 2) {
	    audio_config.achan[1].valid = 1;
	  }
	}
	if (b_opt != 0) {
	  audio_config.adev[0].bits_per_sample = b_opt;
	}
	if (B_opt != 0) {
	  audio_config.achan[0].baud = B_opt;

	  if (audio_config.achan[0].baud < 600) {
            audio_config.achan[0].modem_type = MODEM_AFSK;
            audio_config.achan[0].mark_freq = 1600;
            audio_config.achan[0].space_freq = 1800;
	    audio_config.achan[0].decimate = 3;
	  }
	  else if (audio_config.achan[0].baud > 2400) {
            audio_config.achan[0].modem_type = MODEM_SCRAMBLE;
            audio_config.achan[0].mark_freq = 0;
            audio_config.achan[0].space_freq = 0;
	  }
	  else {
            audio_config.achan[0].modem_type = MODEM_AFSK;
            audio_config.achan[0].mark_freq = 1200;
            audio_config.achan[0].space_freq = 2200;
	  }
	}

	if (strlen(P_opt) > 0) { 
	  /* -P for modem profile. */
	  /* TODO: Not yet documented.  Should probably since it is consistent with atest. */
	  strcpy (audio_config.achan[0].profiles, P_opt); 
	}	

	if (D_opt != 0) {
	    // Reduce audio sampling rate to reduce CPU requirements.
	    audio_config.achan[0].decimate = D_opt;
	}

	if (strlen(l_opt) > 0) {
	  strncpy (misc_config.logdir, l_opt, sizeof(misc_config.logdir)-1);
	}

	misc_config.enable_kiss_pt = enable_pseudo_terminal;

	if (strlen(input_file) > 0) {
	  strcpy (audio_config.adev[0].adevice_in, input_file);
	}

/*
 * Open the audio source 
 *	- soundcard
 *	- stdin
 *	- UDP
 * Files not supported at this time.
 * Can always "cat" the file and pipe it into stdin.
 */

	err = audio_open (&audio_config);
	if (err < 0) {
	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Pointless to continue without audio device.\n");
	  SLEEP_SEC(5);
	  exit (1);
	}

/*
 * Initialize the AFSK demodulator and HDLC decoder.
 */
	multi_modem_init (&audio_config);

/*
 * Initialize the touch tone decoder & APRStt gateway.
 */
	dtmf_init (&audio_config);
	aprs_tt_init (&tt_config);
	tt_user_init (&audio_config, &tt_config);

/*
 * Should there be an option for audio output level?
 * Note:  This is not the same as a volume control you would see on the screen.
 * It is the range of the digital sound representation.
*/
	gen_tone_init (&audio_config, 100);

	assert (audio_config.adev[0].bits_per_sample == 8 || audio_config.adev[0].bits_per_sample == 16);
	assert (audio_config.adev[0].num_channels == 1 || audio_config.adev[0].num_channels == 2);
	assert (audio_config.adev[0].samples_per_sec >= MIN_SAMPLES_PER_SEC && audio_config.adev[0].samples_per_sec <= MAX_SAMPLES_PER_SEC);

/*
 * Initialize the transmit queue.
 */

	xmit_init (&audio_config, d_p_opt);

/*
 * If -x option specified, transmit alternating tones for transmitter
 * audio level adjustment, up to 1 minute then quit.
 * TODO:  enhance for more than one channel.
 */

	if (xmit_calibrate_option) {

	  int max_duration = 60;  /* seconds */
	  int n = audio_config.achan[0].baud * max_duration;
	  int chan = 0;
	
	  text_color_set(DW_COLOR_INFO);
	  dw_printf ("\nSending transmit calibration tones.  Press control-C to terminate.\n");

	  ptt_set (OCTYPE_PTT, chan, 1);
	  while (n-- > 0) {

	    tone_gen_put_bit (chan, n & 1);

	  }
	  ptt_set (OCTYPE_PTT, chan, 0);
	  exit (0);
	}

/*
 * Initialize the digipeater and IGate functions.
 */
	digipeater_init (&audio_config, &digi_config);
	igate_init (&audio_config, &igate_config, &digi_config);

/*
 * Provide the AGW & KISS socket interfaces for use by a client application.
 */
	server_init (&audio_config, &misc_config);
	kissnet_init (&misc_config);

/*
 * Create a pseudo terminal and KISS TNC emulator.
 */
	kiss_init (&misc_config);
	kiss_frame_init (&audio_config);

/*
 * Open port for communication with GPS.
 */
	nmea_init (&misc_config);

/* 
 * Create thread for trying to salvage frames with bad FCS.
 */
	redecode_init (&audio_config);

/*
 * Enable beaconing.
 */
	beacon_init (&audio_config, &misc_config, &digi_config);


	log_init(misc_config.logdir);	

/*
 * Get sound samples and decode them.
 * Use hot attribute for all functions called for every audio sample.
 */


	recv_init (&audio_config);
	recv_process ();

	exit (EXIT_SUCCESS);
}


/*-------------------------------------------------------------------
 *
 * Name:        app_process_rec_frame
 *
 * Purpose:     This is called when we receive a frame with a valid 
 *		FCS and acceptable size.
 *
 * Inputs:	chan	- Audio channel number, 0 or 1.
 *		subchan	- Which modem caught it.  
 *			  Special case -1 for DTMF decoder.
 *		pp	- Packet handle.
 *		alevel	- Audio level, range of 0 - 100.
 *				(Special case, use negative to skip
 *				 display of audio level line.
 *				 Use -2 to indicate DTMF message.)
 *		retries	- Level of bit correction used.
 *		spectrum - Display of how well multiple decoders did.
 *
 *
 * Description:	Print decoded packet.
 *		Optionally send to another application.
 *
 *--------------------------------------------------------------------*/

// TODO:  Use only one printf per line so output doesn't get jumbled up with stuff from other threads.


void app_process_rec_packet (int chan, int subchan, packet_t pp, alevel_t alevel, retry_t retries, char *spectrum)  
{	
	
	char stemp[500];
	unsigned char *pinfo;
	int info_len;
	char heard[AX25_MAX_ADDR_LEN];
	//int j;
	int h;
	char display_retries[32];

	assert (chan >= 0 && chan < MAX_CHANS);
	assert (subchan >= -1 && subchan < MAX_SUBCHANS);
	assert (pp != NULL);	// 1.1J+
     
	strcpy (display_retries, "");
	if (audio_config.achan[chan].fix_bits != RETRY_NONE || audio_config.achan[chan].passall) {
	  sprintf (display_retries, " [%s] ", retry_text[(int)retries]);
	}

	ax25_format_addrs (pp, stemp);

	info_len = ax25_get_info (pp, &pinfo);

	/* Print so we can see what is going on. */

	/* Display audio input level. */
        /* Who are we hearing?   Original station or digipeater. */

	if (ax25_get_num_addr(pp) == 0) {
	  /* Not AX.25. No station to display below. */
	  h = -1;
	  strcpy (heard, "");
	}
	else {
	  h = ax25_get_heard(pp);
          ax25_get_addr_with_ssid(pp, h, heard);
	}

	text_color_set(DW_COLOR_DEBUG);
	dw_printf ("\n");

	if (( ! q_h_opt ) && alevel.rec >= 0) {    /* suppress if "-q h" option */

	  if (h != -1 && h != AX25_SOURCE) {
	    dw_printf ("Digipeater ");
	  }

	  char alevel_text[32];

	  ax25_alevel_to_text (alevel, alevel_text);


	  /* As suggested by KJ4ERJ, if we are receiving from */
	  /* WIDEn-0, it is quite likely (but not guaranteed), that */
	  /* we are actually hearing the preceding station in the path. */

	  if (h >= AX25_REPEATER_2 && 
	        strncmp(heard, "WIDE", 4) == 0 &&
	        isdigit(heard[4]) &&
	        heard[5] == '\0') {

	    char probably_really[AX25_MAX_ADDR_LEN];


	    ax25_get_addr_with_ssid(pp, h-1, probably_really);

	    dw_printf ("%s (probably %s) audio level = %s  %s  %s\n", heard, probably_really, alevel_text, display_retries, spectrum);

	  }
	  else {

	    dw_printf ("%s audio level = %s  %s  %s\n", heard, alevel_text, display_retries, spectrum);
	  }
	}

	/* Version 1.2:   Cranking the input level way up produces 199. */
	/* Keeping it under 100 gives us plenty of headroom to avoid saturation. */

	// TODO:  suppress this message if not using soundcard input.
	// i.e. we have no control over the situation when using SDR.

	if (alevel.rec > 110) {

	  text_color_set(DW_COLOR_ERROR);
	  dw_printf ("Audio input level is too high.  Reduce so most stations are around 50.\n");
	}


// Display non-APRS packets in a different color.

// Display subchannel only when multiple modems configured for channel.

// -1 for APRStt DTMF decoder.

	if (subchan == -1) {
	  text_color_set(DW_COLOR_REC);
	  dw_printf ("[%d.dtmf] ", chan);
	}
	else {
	  if (ax25_is_aprs(pp)) {
	    text_color_set(DW_COLOR_REC);
	  }
	  else {
	    text_color_set(DW_COLOR_DEBUG);
	  }
	  if (audio_config.achan[chan].num_subchan > 1) {
	    dw_printf ("[%d.%d] ", chan, subchan);
	  }
	  else {
	    dw_printf ("[%d] ", chan);
	  }
	}

	dw_printf ("%s", stemp);			/* stations followed by : */

	// for APRS we generally want to display non-ASCII to see UTF-8.
	// for other, probably want to restrict to ASCII only because we are
	// more likely to have compressed data than UTF-8 text.

	// TODO: Might want to use d_u_opt for transmitted frames too.

	ax25_safe_print ((char *)pinfo, info_len, ( ! ax25_is_aprs(pp)) && ( ! d_u_opt) );
	dw_printf ("\n");

// Also display in pure ASCII if non-ASCII characters and "-d u" option specified.

	if (d_u_opt) {

	  unsigned char *p;
	  int n = 0;

	  for (p = pinfo; *p != '\0'; p++) {
	    if (*p >= 0x80) n++;
	  }

	  if (n > 0) {
	    text_color_set(DW_COLOR_DEBUG);
	    ax25_safe_print ((char *)pinfo, info_len, 1);
	    dw_printf ("\n");
	  }
	}

/* Optional hex dump of packet. */

	if (d_p_opt) {

	  text_color_set(DW_COLOR_DEBUG);
	  dw_printf ("------\n");
	  ax25_hex_dump (pp);
	  dw_printf ("------\n");
	}


/* Decode the contents of APRS frames and display in human-readable form. */
/* Suppress decoding if "-q d" option used. */

	if ( ( ! q_d_opt ) && ax25_is_aprs(pp)) {

	  decode_aprs_t A;

	  decode_aprs (&A, pp, 0);

	  //Print it all out in human readable format.

	  decode_aprs_print (&A);

	  // Send to log file.

	  log_write (chan, &A, pp, alevel, retries);

	  // Convert to NMEA waypoint sentence if we have a location.

 	  if (A.g_lat != G_UNKNOWN && A.g_lon != G_UNKNOWN) {
	    nmea_send_waypoint (strlen(A.g_name) > 0 ? A.g_name : A.g_src, 
		A.g_lat, A.g_lon, A.g_symbol_table, A.g_symbol_code, 
		DW_FEET_TO_METERS(A.g_altitude), A.g_course, DW_MPH_TO_KNOTS(A.g_speed), 
		A.g_comment);
	  }
	}


/* Send to another application if connected. */

	int flen;
	unsigned char fbuf[AX25_MAX_PACKET_LEN];

	flen = ax25_pack(pp, fbuf);

	server_send_rec_packet (chan, pp, fbuf, flen);
	kissnet_send_rec_packet (chan, fbuf, flen);
	kiss_send_rec_packet (chan, fbuf, flen);

/* 
 * If it came from DTMF decoder, send it to APRStt gateway.
 * Otherwise, it is a candidate for IGate and digipeater.
 *
 * TODO: It might be useful to have some way to simulate touch tone
 * sequences with BEACON sendto=R... for testing.
 */
	if (subchan == -1) {
	  if (tt_config.gateway_enabled && info_len >= 2) {
	    aprs_tt_sequence (chan, pinfo+1);
	  }
	}
	else { 
	
/* Send to Internet server if option is enabled. */
/* Consider only those with correct CRC. */

	  if (ax25_is_aprs(pp) && retries == RETRY_NONE) {

	    if (digi_config.filter_str[chan][MAX_CHANS] != NULL) {

// TODO1.2: filtering  - maybe it should be ig... so we don't waste time filtering if igate not used.

	    }
	    else {
	      igate_send_rec_packet (chan, pp);
	    }
	  }

/* Send out a regenerated copy. Applies to all types, not just APRS. */

	  digi_regen (chan, pp);


/* 
 *Note that the digipeater function can modify the packet in place so 
 * this is the last thing we should do with it. 
 * Again, use only those with correct CRC; We don't want to spread corrupted data!
 * Single bit change appears to be safe from observations so far but be cautious. 
 */

	  if (ax25_is_aprs(pp) && retries == RETRY_NONE) {

	    digipeater (chan, pp);
	  }
	}

	ax25_delete (pp);
	
} /* end app_process_rec_packet */


/* Process control C and window close events. */

#if __WIN32__

static BOOL cleanup_win (int ctrltype)
{
	if (ctrltype == CTRL_C_EVENT || ctrltype == CTRL_CLOSE_EVENT) {
	  text_color_set(DW_COLOR_INFO);
	  dw_printf ("\nQRT\n");
	  log_term ();
	  ptt_term ();
	  dwgps_term ();
	  SLEEP_SEC(1);
	  ExitProcess (0);
	}
	return (TRUE);
}


#else

static void cleanup_linux (int x)
{
	text_color_set(DW_COLOR_INFO);
	dw_printf ("\nQRT\n");
	log_term ();
	ptt_term ();
	dwgps_term ();
	SLEEP_SEC(1);
	exit(0);
}

#endif



static void usage (char **argv)
{
	text_color_set(DW_COLOR_ERROR);

	dw_printf ("\n");
	dw_printf ("Dire Wolf version %d.%d\n", MAJOR_VERSION, MINOR_VERSION);
	dw_printf ("\n");
	dw_printf ("Usage: direwolf [options]\n");
	dw_printf ("Options:\n");
	dw_printf ("    -c fname       Configuration file name.\n");
	dw_printf ("    -l logdir      Directory name for log files.  Use . for current.\n");
	dw_printf ("    -r n           Audio sample rate, per sec.\n");
	dw_printf ("    -n n           Number of audio channels, 1 or 2.\n");
	dw_printf ("    -b n           Bits per audio sample, 8 or 16.\n");
	dw_printf ("    -B n           Data rate in bits/sec for channel 0.  Standard values are 300, 1200, 9600.\n");
	dw_printf ("                     If < 600, AFSK tones are set to 1600 & 1800.\n");
	dw_printf ("                     If > 2400, K9NG/G3RUH style encoding is used.\n");
	dw_printf ("                     Otherwise, AFSK tones are set to 1200 & 2200.\n");
	dw_printf ("    -D n           Divide audio sample rate by n for channel 0.\n");
	dw_printf ("    -d             Debug options:\n");
	dw_printf ("       a             a = AGWPE network protocol client.\n");
	dw_printf ("       k             k = KISS serial port client.\n");
	dw_printf ("       n             n = KISS network client.\n");
	dw_printf ("       u             u = Display non-ASCII text in hexadecimal.\n");
	dw_printf ("       p             p = dump Packets in hexadecimal.\n");
	dw_printf ("       t             t = gps Tracker.\n");
	dw_printf ("       o             o = output controls such as PTT and DCD.\n");
	dw_printf ("    -q             Quiet (suppress output) options:\n");
	dw_printf ("       h             h = Heard line with the audio level.\n");
	dw_printf ("       d             d = Decoding of APRS packets.\n");
	dw_printf ("    -t n           Text colors.  1=normal, 0=disabled.\n");
#if __WIN32__
#else
	dw_printf ("    -p             Enable pseudo terminal for KISS protocol.\n");
#endif
	dw_printf ("    -x             Send Xmit level calibration tones.\n");
	dw_printf ("    -U             Print UTF-8 test string and exit.\n");
	dw_printf ("\n");

#if __WIN32__
#else
	dw_printf ("Complete documentation can be found in /usr/local/share/doc/direwolf.\n");
#endif
	exit (EXIT_FAILURE);
}



/* end direwolf.c */
