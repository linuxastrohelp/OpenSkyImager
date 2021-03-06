/*
 * imgCFWio.c
 *
 *  Created on: 20.11.2013
 *      Author: Giampiero Spezzano (gspezzano@gmail.com)
 *
 * Device access code (USB) is based on original QHYCCD.inc code from 
 * https://github.com/qhyccd-lzr
 *
 * Device access code (TTY) is original work based on the protocol document as
 * released by QHYCCD.inc:
 * http://qhyccd.com/en/left/qhy-colorwheel/
 * http://qhyccd.com/ccdbbs/index.php?topic=1083.0
 *
 * This file is part of "OpenSkyImager".
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "ttycom.h"
#include "ttylist.h"
#include "imgBase.h"
#include <sys/stat.h>
#include "imgCamio.h"
#include "imgCFWio.h"
#include "glibversions.h"

#define READ_TIME   5

static char cfwmsg[1024];
static int  cfwmode;
static char cfwtty[256];
static int  cfwttyfd;
static char cfwmodel[256];
static char cfwmodels[256];
static int  cfwmodid;
static int  cfwslotc;
static int  cfwslots[16];
static int  cfwpos;
static int  cfwIdle;
static int  cfwHasReset;

// Tty-Read thread
static GThread *thd_read = NULL;
// Pointer to function for the "callback" we want thread to execute upon finish
// Function takes a int param to report exit status
// Found no way to pass this reference to thread func directly, so used a module
// variable.
// This is not going to be thread safe. Hopefully this module won't ever be 
// used from more than one thread at the time
static gpointer (*postReadProcess)(int);

static gboolean tmr_run(gpointer data)
{
	
	// Executes the post process to inform the user (and rest of application)
	// About the result got
	if (postReadProcess != NULL)
	{
		postReadProcess(GPOINTER_TO_INT(data));
		postReadProcess = NULL;
	}
	return FALSE;
}

gpointer thd_qhy_tty_idle_run(gpointer thd_data)
{	
	int ttyret;
	char buf[1];
	int nbrw = 0;
	int i = 0;
	char resp = GPOINTER_TO_INT(thd_data);
	
	resp = (resp < 10) ? (resp + 49) : (resp - 10 + 97);
	buf[0] = '\0';
	cfwIdle = 0;
	// Attempts 3 reads of 5 seconds each, to allow enough time for to the CFW
	// to settle.
	while ((ttyret = tty_read(cfwttyfd, buf, sizeof(buf), READ_TIME, &nbrw)) == TTY_TIME_OUT)
	{
		if (++i > 3)
		{
			break;
		}
		//printf("Attempt read %d\n", i);
	}
	//printf("CFW returned '%c' waiting for '%c'\n", buf[0], resp);
	// Runs the post process to inform the user (and rest of application)
	// About the result got
	// The post process will work on the GUI, hence to be thread safe must be 
	// run from the main loop -> timer.
	cfwIdle = 1;
	ttyret = ((ttyret == TTY_OK) && ((buf[0] == 0x2D) || (buf[0] == resp)));
	//printf("ttyret = %d\n", ttyret);
	cfwpos = (ttyret == 1)? cfwpos : -1;
	g_timeout_add(1, tmr_run, GINT_TO_POINTER(ttyret));
	return 0;
}

gpointer thd_get_idle_run(gpointer thd_data)
{
	int i, status = -1;
	
	cfwIdle = 0;
	for (i = 0; i < 60; i++)
	{
		g_usleep(1000000);
		imgcam_wheel_getstatus(&status);
		//printf("Status: %d\n", status);
		if (status < 1)
		{
			// Idle, all ok
			break;
		}
	}
	cfwIdle = 1;
	// Executes the post process to inform the user (and rest of application)
	// About the result got
	// The post process will work on the GUI, hence to be thread safe must be 
	// Executed from the main loop -> timer.
	cfwpos = (status == 0)? cfwpos : -1;
	g_timeout_add(1, tmr_run, GINT_TO_POINTER(status == 0));
	return 0;
}

void imgcfw_init()
{
	cfwmsg[0] = '\0'; 
	cfwmode = 0;
	cfwtty[0] = '\0';
	cfwttyfd = -1;
	cfwmodid = -1;
	cfwmodel[0] = '\0';
	cfwmodels[0] = '\0';
	cfwslotc = 0;
	memset(cfwslots, 0, 16);
	cfwpos = -1;
	cfwHasReset = 0;
	cfwIdle = 1;
	postReadProcess = NULL;
}

char *imgcfw_get_msg()
{
	return cfwmsg;
}

int imgcfw_set_mode(int mode)
{
	int retval = 0;

	cfwmsg[0] = '\0';
	switch (mode)
	{
		case 0:
		case 1:
		case 2:
			// QHY-Serial (and 2)
			cfwmode = mode;
			retval = 1;
			break;
				
		case 99:
			// QHY-Through-camera
			if ((imgcam_connected() == 1) && (strlen(imgcam_get_camui()->whlstr) > 0))
			{
				cfwmode = mode;
				retval = 1;
			}
			else if (imgcam_connected() == 0)
			{
				sprintf(cfwmsg, C_("cfw","Camera is not connected"));
			}
			else if (strlen(imgcam_get_camui()->whlstr) == 0)
			{
				sprintf(cfwmsg, C_("cfw","Camera does not support direct CFW connection"));
			}
			break;
	}
	return (retval);
}

int imgcfw_get_mode()
{
	return cfwmode;
}

int imgcfw_set_tty(char *tty)
{
	int retval = 0;
	struct stat st;
	
	cfwmsg[0] = '\0';
	if (strlen(tty) > 0)
	{
		if (lstat(tty, &st) == 0 || S_ISLNK(st.st_mode))
		{
			strcpy(cfwtty, tty);
			retval = 1;
		}
		if (retval == 0)
		{
			sprintf(cfwmsg, C_("cfw","Serial port does not exist"));	
		}
	}
	else
	{
		retval = 1;
	}
	return (retval);
}

const char *imgcfw_get_tty()
{
	switch (cfwmode)
	{
		case 0:
		case 1:
			return cfwtty;	
		case 99:
			/// This will complete: "Filter wheel connected to %s"
			return C_("cfw", "camera");
	}
	return cfwtty;
}

int imgcfw_connect()
{
	int retval = 0;
	int ttyresult = 0;
	
	cfwmsg[0] = '\0';
	switch (cfwmode)
	{
		case 0:
			break;
		
		case 1:
		case 2:
			// QHY-Serial (and 2)
			if ((ttyresult = tty_connect(cfwtty, 9600, 8, PARITY_NONE, 1, &cfwttyfd)) == TTY_OK)
			{
				if ((retval = imgcfw_read_all()) == 0)
				{
					// If read failed, give up and close port
					tty_disconnect(cfwttyfd);
					cfwttyfd = -1;
				}
			}
			else
			{
				char ttyerr[512];
				tty_error_msg(ttyresult, ttyerr, 512);
				sprintf(cfwmsg, C_("cfw","Could not connect to CFW on serial port %s, error: %s"), cfwtty, ttyerr);
			}
			break;
			
		case 99:
			// Through-camera
			if ((imgcam_connected() == 1) && (strlen(imgcam_get_camui()->whlstr) > 0))
			{
				// Models name 
				// QHY can't autodetect model when in camera-through mode
				// Sbig does the auto detection and connection in core
				strcpy(cfwmodels, imgcam_get_camui()->whlstr);
				cfwHasReset = (imgcam_get_camid() == 2000);
				retval = 1;
			}
			else if (imgcam_connected() == 0)
			{
				sprintf(cfwmsg, C_("cfw","Camera is not connected"));
			}
			else if (strlen(imgcam_get_camui()->whlstr) == 0)
			{
				sprintf(cfwmsg, C_("cfw","Camera does not support direct CFW connection"));
			}
			break;
	}
	return (retval);
}

int imgcfw_disconnect()
{
	int retval = 0;
	
	cfwmsg[0] = '\0';
	switch (cfwmode)
	{
		case 0:
			break;
		
		case 1:
		case 2:
			// QHY-Serial (and 2)
			if ((retval = tty_disconnect(cfwttyfd)) == TTY_OK)
			{
				cfwttyfd = -1;
				cfwmodel[0] = '\0';
				cfwslotc = 0;
				memset(cfwslots, 0, 16);
				cfwpos = -1;
				cfwHasReset = 0;
			}
			else
			{
				char ttyerr[512];
				tty_error_msg(retval, ttyerr, 512);
				sprintf(cfwmsg, C_("cfw","Could not disconnect from CFW on serial port %s, error: %s"), cfwtty, ttyerr);
			}
			break;
			
		case 99:
			// Through-camera
			retval = TTY_OK;
			break;
	}
	return (retval == TTY_OK);
}

int imgcfw_read_all()
{
	// This is real time function so it will make the entire program wait
	// for answer. It's supposed to be immediate answer.
	// Should this will prove unsafe... we'll change.
	if (cfwmode == 0)
	{
		return (1);
	}
	else if (cfwmode == 1)
	{
		// QHY-Serial
		int nbrw = 0;
		char buf[32];
		char wbuf[3] = {0x53, 0x45, 0x47};
		int ttyresult = 0;
	
		cfwmsg[0] = '\0';
		cfwmodel[0] = '\0';
		if (cfwttyfd > -1)
		{
			/* Flush the input buffer */
			tcflush(cfwttyfd,TCIOFLUSH);
			// Sending "SEG"
			if ((ttyresult = tty_write(cfwttyfd, wbuf, sizeof(wbuf), &nbrw)) == TTY_OK)
			{
				// Reading answer
				if ((ttyresult = tty_read(cfwttyfd, buf, 17, READ_TIME, &nbrw)) == TTY_OK)
				{
					cfwmodid = buf[0];
					switch (cfwmodid)
					{
						case 0:
							// QHY 2" 5 positions
							/// Model name
							sprintf(cfwmodel, C_("cfw","5-QHY 5 slots 2\""));
							sprintf(cfwmodels, C_("cfw","|5-QHY 5 slots 2\""));
							cfwslotc = 5;
							cfwslots[0] = buf[1] * 256 + buf[2];
							cfwslots[1] = buf[3] * 256 + buf[4];
							cfwslots[2] = buf[5] * 256 + buf[6];
							cfwslots[3] = buf[7] * 256 + buf[8];
							cfwslots[4] = buf[9] * 256 + buf[10];
							break;
						
						default:
							/// Model name
							sprintf(cfwmodel, C_("cfw","8-QHY serial"));
							sprintf(cfwmodels, C_("cfw","|8-QHY serial"));
							cfwslotc = 8;
							// Load them all just in case
							cfwslots[0] = buf[1] * 256 + buf[2];
							cfwslots[1] = buf[3] * 256 + buf[4];
							cfwslots[2] = buf[5] * 256 + buf[6];
							cfwslots[3] = buf[7] * 256 + buf[8];
							cfwslots[4] = buf[9] * 256 + buf[10];
						
							cfwslots[5] = buf[11] * 256 + buf[12];
							cfwslots[6] = buf[13] * 256 + buf[14];
							cfwslots[7] = buf[15] * 256 + buf[16];
							break;
					}
				}
				else
				{
					char ttyerr[512];
					tty_error_msg(ttyresult, ttyerr, 512);
					sprintf(cfwmsg, C_("cfw","Could not read from CFW on serial port %s, error: %s"), cfwtty, ttyerr);
				}
			}
			else
			{
				char ttyerr[512];
				tty_error_msg(ttyresult, ttyerr, 512);
				sprintf(cfwmsg, C_("cfw","Could not write to CFW on serial port %s, error: %s"), cfwtty, ttyerr);
			}
		}
		return (ttyresult == TTY_OK);
	}
	else if (cfwmode == 2)
	{
		// QHY-Serial (2)
		int nbrw = 0;
		char buf[2];
		int ttyresult = 0;

		cfwmsg[0] = '\0';
		cfwmodel[0] = '\0';
		if (cfwttyfd > -1)
		{
			/* Flush the input buffer */
			tcflush(cfwttyfd,TCIOFLUSH);
			// Waiting for initial reset to end (triggered by connection)
			if ((ttyresult = tty_read(cfwttyfd, buf, 1, READ_TIME * 5, &nbrw)) == TTY_OK)
			{
				if (buf[0] == '1')
				{
					cfwmodid = 0;
					cfwslotc = 5;
					strcpy(cfwmodels, C_("camio","5-Positions|6-Positions|7-Positions|8-Positions:0"));
				}
			}
		}
		return ((ttyresult == TTY_OK) && (buf[0] == '1'));
	}
	else if (cfwmode == 99)
	{
		//Through camera
		return(1);
	
	}
	return(1);
}

int imgcfw_reset()
{
	if (cfwmode == 0)
	{
		return (1);
	}
	else if (cfwmode == 1)
	{
		int nbrw = 0;
		char wbuf[3] = {0x53, 0x45, 0x46};
		int ttyresult = 0;
	
		cfwmsg[0] = '\0';
		if (cfwttyfd > -1)
		{
			//printf("Reset request with valid / open ttyport\n");
			/* Flush the input buffer */
			tcflush(cfwttyfd,TCIOFLUSH);
			// Sending "SEF"
			if ((ttyresult = tty_write(cfwttyfd, wbuf, sizeof(wbuf), &nbrw)) == TTY_OK)
			{
				//printf("Reset request ok, successfully sent %d bytes, sent %c%c%c\n", nbrw, wbuf[0], wbuf[1], wbuf[2]);
				// Since there's no answer for this command we must assume all is good
			}
			else
			{
				char ttyerr[512];
				tty_error_msg(ttyresult, ttyerr, 512);
				printf(C_("cfw","Could not write to CFW on serial port %s, error: %s"), cfwtty, ttyerr);
				sprintf(cfwmsg, C_("cfw","Could not write to CFW on serial port %s, error: %s"), cfwtty, ttyerr);
			}
		}
		return (ttyresult == TTY_OK);
	}
	else if (cfwmode == 2)
	{
		int retval = 0;
		// QHY-Serial (2)
		if (imgcfw_disconnect())
		{
			if (imgcfw_connect())
			{
				retval = 1;
			}
		}
		return (retval);
	}
	else if ((cfwmode == 99))
	{
		int retval = 0;
		//Through camera
		retval = imgcam_wheel_reset();
		// TODO add a thread to check idle status?
		return(retval);
	
	}
	return(1);
}

int imgcfw_set_model(char *model)
{
	sscanf(model, "%d-%s", &cfwslotc, cfwmodel);	
	return cfwslotc;
}

char *imgcfw_get_model()
{
	return cfwmodel;
}

char *imgcfw_get_models()
{
	return cfwmodels;
}

int imgcfw_is_reset()
{
	return cfwHasReset;
}

int imgcfw_is_idle()
{
	return cfwIdle;
}

int imgcfw_get_slotcount()
{
	return cfwslotc;
}

int imgcfw_get_slot()
{
	return cfwpos;
}

int imgcfw_set_slot(int slot, gpointer (*postProcess)(int))
{
	int retval = 0;
	int ttyresult = 0;
	int nbrw = 0;
	char wbuf[1];
	
	if (cfwIdle)
	{
		switch (cfwmode)
		{
			case 1:
			case 2:
				// Qhy Serial (also 2), chars "0", "1"..., "9" thus 0x30 (decimal 48) onward, then "a", "b", ..., "f"
				wbuf[0] = (slot < 11) ? (slot + 48) : (slot - 11 + 97);
				cfwmsg[0] = '\0';
				if (slot < cfwslotc)
				{
					if (slot != cfwpos)
					{
						/* Flush the input buffer */
						tcflush(cfwttyfd, TCIOFLUSH);
						if ((ttyresult = tty_write(cfwttyfd, wbuf, sizeof(wbuf), &nbrw)) == TTY_OK)
						{
							retval = 1;
							sprintf(cfwmsg, C_("cfw","Filter wheel moving to slot: %d"), slot + 1);
							//printf("%s\n", cfwmsg);
							cfwpos = slot;

							// Starting tty-read thread
							GError* thd_err = NULL;
							postReadProcess = postProcess;
							thd_read = g_thread_try_new("CFW-Change-Slot", thd_qhy_tty_idle_run, GINT_TO_POINTER(cfwpos), &thd_err);
							if (thd_read == NULL)
							{
								strcat(cfwmsg, C_("cfw","Could not start notification thread, btw slot selection command was sent ok."));
								//printf("%s\n", cfwmsg);
								postReadProcess = NULL;
							}
						}		
						else
						{
							char ttyerr[512];
							tty_error_msg(ttyresult, ttyerr, 512);
							sprintf(cfwmsg, C_("cfw","Could not write to CFW on serial port %s, error: %s"), cfwtty, ttyerr);
							//printf("%s\n", cfwmsg);
						}
					}
				}
				else
				{
					sprintf(cfwmsg, C_("cfw","Requested slot (%d) does not exist. Slots = %d"), slot + 1, cfwslotc);
					//printf("%s\n", cfwmsg);
				}
				break;
		
			case 99:
				if (imgcam_get_camid() < 1000)
				{
					// Qky-through-camera
					postReadProcess = postProcess;
					if ((retval = imgcam_wheel(slot)) == 1)
					{
						cfwpos = slot;
						sprintf(cfwmsg, C_("cfw","Filter wheel moving to slot: %d"), slot + 1);
						g_timeout_add_seconds((READ_TIME * 2), tmr_run, (gpointer)1);
					}
					else
					{
						sprintf(cfwmsg, "%s", imgcam_get_msg());
					}
				}
				else //if (imgcam_get_camid() == 2000)
				{
					// SBIG-Through-camera (and others?)
					postReadProcess = postProcess;
					if ((retval = imgcam_wheel(slot)) == 1)
					{
						sprintf(cfwmsg, C_("cfw","Filter wheel moving to slot: %d"), slot + 1);
						cfwpos = slot;

						// Starting a thread to get move finish
						GError* thd_err = NULL;
						postReadProcess = postProcess;
						thd_read = g_thread_try_new("CFW-Change-Slot", thd_get_idle_run, NULL, &thd_err);
						if (thd_read == NULL)
						{
							strcat(cfwmsg, C_("cfw","Could not start notification thread, btw slot selection command was sent ok."));
							postReadProcess = NULL;
						}
					}
					else
					{
						sprintf(cfwmsg, "%s", imgcam_get_msg());
					}
				}
				break;
		}
	}
	else
	{
		sprintf(cfwmsg, C_("cfw","Wait for current movement to end"));
	}
	return (retval);
}
